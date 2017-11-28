/*
 * Copyright 2015, 2017 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <zlib.h>
#include <wrapper.h>
#include <libzHW.h>
#include <asm/byteorder.h>

/**
 * Hardware zlib implementation. This code is using the libzHW library
 * to do hardware supported inflate and deflate. To overcome
 * performance degragation by using small buffers the deflate
 * functionality is using sufficiently large buffers for input and
 * output.
 */

#undef CONFIG_DEBUG
#undef CONFIG_USE_PINNING	/* FIXME Driver has problems with
				   get_user_pages_fast not pinning all
				   requested pages. Need to work on a
				   fix for that before we can enable
				   this. */

/*
 * BUF_SIZE of 0 is used to avoid buffering. Env-variables can
 * overwrite those defaults.
 */
#define CONFIG_INFLATE_BUF_SIZE	 (128 * 1024)
#define CONFIG_DEFLATE_BUF_SIZE	 (768 * 1024)

/* FIXME Ensure values are really the same for newer/older zlib versions */
#define rc_zedc_to_libz(x) ((x))
#define rc_libz_to_zedc(x) ((x))

struct hw_state {
	int card_no;
	int card_type;
	unsigned int mode;

	zedc_stream h;		/* hardware compression context */
	int rc;			/* hardware return code e.g. Z_STREAM_END */
	unsigned int page_size;

	/* buffering for the moment only for compression */
	size_t  ibuf_total;	/* total_size of ibuf_base */
	size_t  ibuf_avail;	/* available bytes in ibuf */
	uint8_t *ibuf_base;	/* buffer for input data */
	uint8_t *ibuf;		/* current position in ibuf to put data */

	size_t  obuf_total;	/* total_size of obuf_base */
	size_t  obuf_avail;	/* available bytes in obuf */
	uint8_t *obuf_base;	/* buffer for output data */
	uint8_t *obuf;		/* current position in obuf to put data */
	uint8_t *obuf_next;	/* next position to read data */

	unsigned int inflate_req;  /* # of inflates */
	unsigned int deflate_req;  /* # of deflates */
};

/**
 * @return True if output buffer is empty, else False.
 */
static int output_buffer_empty(struct hw_state *s)
{
	return (s->obuf_avail == s->obuf_total);
}

/**
 * @return Remaining bytes in obuf.
 */
static int output_buffer_bytes(struct hw_state *s)
{
	return s->obuf - s->obuf_next;
}

#define ZEDC_VERBOSE_LIBCARD_MASK 0x0000ff00  /* debug flags for libcard */
#define ZEDC_VERBOSE_LIBZEDC_MASK 0x000000ff  /* debug flags for libzedc */
#define ZEDC_VERBOSE_DDCB	  0x00010000  /* dump DDCBs if requested */

static int zedc_verbose  = 0x00000000; /* verbosity flag */
static int zlib_xcheck = 1;
static unsigned int zlib_ibuf_total = CONFIG_DEFLATE_BUF_SIZE;
static unsigned int zlib_obuf_total = CONFIG_INFLATE_BUF_SIZE;

#define ZEDC_CARDS_LENGTH 128

/* Try to cache filehandles for faster access. Do not close them. */
static zedc_handle_t zedc_cards[ZEDC_CARDS_LENGTH + 1];

static zedc_handle_t __zedc_open(int card_no, int card_type, int mode,
				 int *err_code)
{
	int flags = (zlib_inflate_flags | zlib_deflate_flags);

	if ((flags & ZLIB_FLAG_CACHE_HANDLES) == 0x0)
		return zedc_open(card_no, card_type, mode,
				 err_code);

	if (card_no == -1) {
		if (zedc_cards[ZEDC_CARDS_LENGTH])
			return zedc_cards[ZEDC_CARDS_LENGTH];

		zedc_cards[ZEDC_CARDS_LENGTH] = zedc_open(card_no, card_type, mode,
					    err_code);
		return zedc_cards[ZEDC_CARDS_LENGTH];
	}

	if (card_no < 0 || card_no >= ZEDC_CARDS_LENGTH)
		return NULL;

	if (zedc_cards[card_no] != NULL) {
		return zedc_cards[card_no];
	}

	zedc_cards[card_no] = zedc_open(card_no, card_type, mode,
					err_code);
	return zedc_cards[card_no];
}

static int __zedc_close(zedc_handle_t zedc __unused)
{
	int flags = (zlib_inflate_flags | zlib_deflate_flags);

	if ((flags & ZLIB_FLAG_CACHE_HANDLES) == 0x0)
		return zedc_close(zedc);

	/* Ignore close in cached fd mode ... */
	return ZEDC_OK;
}

static void stream_zedc_to_zlib(z_streamp s, zedc_streamp h)
{
	s->next_in   = (uint8_t	*)h->next_in;   /* next input byte */
	s->avail_in  = h->avail_in;  /* number of bytes available at next_in */
	s->total_in  = h->total_in;  /* total nb of input bytes read so far */

	s->next_out  = h->next_out;  /* next output byte should be put there */
	s->avail_out = h->avail_out; /* remaining free space at next_out */
	s->total_out = h->total_out; /* total nb of bytes output so far */
}

static void stream_zlib_to_zedc(zedc_streamp h, z_streamp s)
{
	h->next_in   = s->next_in;   /* next input byte */
	h->avail_in  = s->avail_in;  /* number of bytes available at next_in */
	h->total_in  = s->total_in;  /* total nb of input bytes read so far */

	h->next_out  = s->next_out;  /* next output byte should be put there */
	h->avail_out = s->avail_out; /* remaining free space at next_out */
	h->total_out = s->total_out; /* total nb of bytes output so far */
}


/**
 * Take care CRC/ADLER is correctly reported to the upper levels.
 */
static void __fixup_crc_or_adler(z_streamp s, zedc_streamp h)
{
	s->adler = (h->format == ZEDC_FORMAT_GZIP) ? h->crc32 : h->adler32;
}

/**
 * See #152 The adler32 start value is 1 according to the specification.
 * If there was a call to deflateSetDictionary() the adler field in s
 * will be set to the adler32 value of the passed in dictionary.
 * Nevertheless the data processing needs to start with a 1. This
 * function takes are that on the 1st call of deflate when total_in
 * is still 0, we set the start value always to 1.
 */
static void __prep_crc_or_adler(z_streamp s, zedc_streamp h)
{
	if (s->total_in == 0) {
		if (h->format == ZEDC_FORMAT_ZLIB)
			s->adler = 1;
		else
			s->adler = 0;
	}
}

static void __free(void *ptr)
{
	if (ptr == NULL)
		return;
	free(ptr);
}

/**
 * Theoretical maximum size of the data is worst case of 9/8
 * of the input buffer. We add one page more because our
 * hardware encoder is sometimes storing some left-over bytes.
 *
 * zLib documentation: "The worst case choice of
 * parameters can result in an expansion of at most
 * 13.5%, plus eleven bytes."
 *
 * zEDC was better here than zEDCv2. zEDCv2 requires
 * us to increase the factor to 15/8, which wastes
 * some memory in most cases. What a pity.
 */
uLong h_deflateBound(z_streamp strm __attribute__((unused)), uLong sourceLen)
{
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	return sourceLen * 15/8 + page_size;
}

int h_deflateInit2_(z_streamp strm,
		    int level,
		    int method,
		    int windowBits,
		    int memLevel,
		    int strategy,
		    const char *version __unused,
		    int stream_size __unused)
{
	int rc, err_code = 0;
	struct hw_state *s;
	zedc_handle_t zedc;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	strm->total_in = 0;
	strm->total_out = 0;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return Z_MEM_ERROR;

	s->card_type = zlib_accelerator;
	s->card_no = zlib_card;
	s->mode = DDCB_MODE_ASYNC | DDCB_MODE_RDWR;

	if (zlib_deflate_flags & ZLIB_FLAG_USE_POLLING)
		s->mode |= DDCB_MODE_POLLING;

	zedc = __zedc_open(s->card_no, s->card_type, s->mode, &err_code);
	if (!zedc) {
		rc = Z_STREAM_ERROR;
		goto free_hw_state;
	}

	s->h.device = zedc;
	s->deflate_req = 0;
	s->page_size = page_size;

	/* Default is SGLIST */
	s->h.dma_type[ZEDC_IN]  = DDCB_DMA_TYPE_SGLIST;
	s->h.dma_type[ZEDC_OUT] = DDCB_DMA_TYPE_SGLIST;
	s->h.dma_type[ZEDC_WS]  = DDCB_DMA_TYPE_SGLIST;

	if (zlib_deflate_flags & ZLIB_FLAG_USE_FLAT_BUFFERS) {
		if (zlib_ibuf_total != 0) {
			s->h.dma_type[ZEDC_IN]  = DDCB_DMA_TYPE_FLAT;
			s->h.dma_type[ZEDC_OUT] = DDCB_DMA_TYPE_FLAT;
		}
		s->h.dma_type[ZEDC_WS]  = DDCB_DMA_TYPE_FLAT;
	}

#if defined(CONFIG_USE_PINNING)
	s->h.dma_type[ZEDC_IN]  |= DDCB_DMA_PIN_MEMORY;
	s->h.dma_type[ZEDC_OUT] |= DDCB_DMA_PIN_MEMORY;
	s->h.dma_type[ZEDC_WS]  |= DDCB_DMA_PIN_MEMORY;
#endif
	if (zlib_xcheck)
		s->h.flags |= ZEDC_FLG_CROSS_CHECK;

	if (zedc_verbose & ZEDC_VERBOSE_DDCB)
		s->h.flags |= ZEDC_FLG_DEBUG_DATA;

	if (zlib_deflate_flags & ZLIB_FLAG_OMIT_LAST_DICT)
		s->h.flags |= ZEDC_FLG_SKIP_LAST_DICT;

	if (zlib_ibuf_total) {
		s->ibuf_total = s->ibuf_avail = zlib_ibuf_total;
		s->ibuf_base = s->ibuf = zedc_memalign(zedc, s->ibuf_total,
						s->h.dma_type[ZEDC_IN]);
		if (s->ibuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto close_card;
		}

		s->obuf_total = s->obuf_avail =
			h_deflateBound(strm, zlib_ibuf_total);

		s->obuf_base = s->obuf = s->obuf_next =
			zedc_memalign(zedc, s->obuf_total,
				      s->h.dma_type[ZEDC_OUT]);
		if (s->obuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto free_ibuf;
		}
	}

	hw_trace("[%p] h_deflateInit2_: card_type=%d card_no=%d "
		 "zlib_ibuf_total=%d\n", strm, s->card_type, s->card_no,
		 zlib_ibuf_total);

	rc = zedc_deflateInit2(&s->h, level, method, windowBits, memLevel,
			       strategy);
	__fixup_crc_or_adler(strm, &s->h);

	if (rc != ZEDC_OK) {
		rc = rc_zedc_to_libz(rc);
		goto free_obuf;
	}

	strm->state = (void *)s; /* remember hardware state */
	return rc_zedc_to_libz(rc);

 free_obuf:
	zedc_free(zedc, s->obuf_base, s->obuf_total, s->h.dma_type[ZEDC_OUT]);
 free_ibuf:
	zedc_free(zedc, s->ibuf_base, s->ibuf_total, s->h.dma_type[ZEDC_IN]);
 close_card:
	__zedc_close(zedc);
 free_hw_state:
	__free(s);
	return rc;
}

/**
 * Implementation note: This mechanism will not work, if the caller is
 * using driver allocated memory. Currently only the device driver
 * keeps track of the allocated buffers. The library does not and can
 * therefore not initiate the a copy. This will cause the mechanism
 * only to work, if users use self allocated memory together with
 * hardware sglists.
 */
int h_deflateCopy(z_streamp dest, z_streamp source)
{
	struct hw_state *s_source;
	struct hw_state *s_dest;
	zedc_handle_t zedc;
	int rc = Z_OK, err_code;

	s_source = (struct hw_state *)source->state;
	s_dest = calloc(1, sizeof(*s_dest));
	if (s_dest == NULL) {
		pr_err("Cannot get destination buffer\n");
		return Z_MEM_ERROR;
	}
	memcpy(s_dest, s_source, sizeof(*s_dest));

	rc = rc_zedc_to_libz(zedc_deflateCopy(&s_dest->h, &s_source->h));
	if (rc != Z_OK) {
		pr_err("zEDC deflateCopy returned %d\n", rc);
		goto err_free_s_dest;
	}

	zedc = __zedc_open(s_dest->card_no, s_dest->card_type,
			   s_dest->mode, &err_code);
	if (!zedc) {
		pr_err("Cannot open accelerator handle\n");
		rc = Z_STREAM_ERROR;
		goto err_zedc_close;
	}
	s_dest->h.device = zedc;
	hw_trace("  Allocated zedc device %p\n", zedc);

	/*
	 * FIXME ... check if all that stuff below is really correct ...
	 *
	 * We need to allocate space for the buffers and make sure
	 * that the pointers point to the right addresses depending on
	 * the fill-level. Furthermore we need to copy the data over
	 * to the new buffers.
	 */
	if (s_source->ibuf_total) {
		s_dest->ibuf_total = s_source->ibuf_total;
		s_dest->ibuf_avail = s_source->ibuf_avail;
		s_dest->ibuf_base = zedc_memalign(zedc, s_dest->ibuf_total,
					s_dest->h.dma_type[ZEDC_IN]);
		if (s_dest->ibuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto err_zedc_close;
		}
		s_dest->ibuf = s_dest->ibuf_base +
			(s_source->ibuf - s_source->ibuf_base);
		memcpy(s_dest->ibuf_base, s_source->ibuf_base,
		       s_source->ibuf - s_source->ibuf_base);
	}
	if (s_source->obuf_total) {
		s_dest->obuf_total = s_source->obuf_total;
		s_dest->obuf_avail = s_source->obuf_avail;
		s_dest->obuf_base = zedc_memalign(zedc, s_dest->obuf_total,
					s_dest->h.dma_type[ZEDC_OUT]);
		if (s_dest->obuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto err_free_ibuf_base;
		}
		s_dest->obuf = s_dest->obuf_base +
			(s_source->obuf - s_source->obuf_base);
		s_dest->obuf_next = s_dest->obuf_base +
			(s_source->obuf_next - s_source->obuf_base);
		memcpy(s_dest->obuf_next, s_source->obuf_next,
		       s_dest->obuf_total - s_dest->obuf_avail);
	}

	dest->state = (void *)s_dest;
	return Z_OK;

 err_free_ibuf_base:
	free(s_dest->ibuf_base);
	s_dest->ibuf_base = NULL;
 err_zedc_close:
	__zedc_close(zedc);
 err_free_s_dest:
	free(s_dest);
	return rc;
}

int h_deflateReset(z_streamp strm)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_deflateReset\n", strm);
	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	/* reset buffers */
	strm->total_in = 0;
	strm->total_out = 0;

	s->deflate_req = 0;
	s->ibuf_avail = s->ibuf_total;
	s->ibuf       = s->ibuf_base;
	s->obuf_avail = s->obuf_total;
	s->obuf       = s->obuf_base;
	s->obuf_next  = s->obuf_base;
	s->rc	      = Z_OK;

	rc = zedc_deflateReset(h);
	__fixup_crc_or_adler(strm, h);

	return rc_zedc_to_libz(rc);
}

int h_deflateSetDictionary(z_streamp strm, const uint8_t *dictionary,
			   unsigned int dictLength)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_deflateSetDictionary dictionary=%p dictLength=%d\n",
		 strm, dictionary, dictLength);
	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	rc = zedc_deflateSetDictionary(h, dictionary, dictLength);
	hw_trace("[%p]    adler32=%08x  dict_adler32=%08x\n", strm,
		 h->adler32, h->dict_adler32);

	strm->adler = h->dict_adler32; /* See #152 */
	return rc_zedc_to_libz(rc);
}

int h_deflateSetHeader(z_streamp strm, gz_headerp head)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_deflateSetHeader headerp=%p\n", strm, head);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	if (sizeof(*head) != sizeof(gzedc_header))
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	rc = zedc_deflateSetHeader(h, (gzedc_header *)head);
	return rc_zedc_to_libz(rc);
}

static inline int __deflate(z_streamp strm, struct hw_state *s, int flush)
{
	int rc;
	zedc_stream *h = &s->h;

	hw_trace("[%p] h_deflate (%d): flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d\n", strm, s->deflate_req,
		 flush_to_str(flush), h->next_in, h->avail_in, h->next_out,
		 h->avail_out);

	rc = zedc_deflate(h, flush);
	__fixup_crc_or_adler(strm, h);
	s->deflate_req++;

	hw_trace("[%p]            flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d rc=%d\n", strm, flush_to_str(flush),
		 h->next_in, h->avail_in, h->next_out, h->avail_out, rc);

	return rc;
}

/**
 * Collect input data
 */
static int h_read_ibuf(z_streamp strm)
{
	int tocopy;
	struct hw_state *s = (struct hw_state *)strm->state;

	if ((s->ibuf_avail == 0) ||	      /* no input buffer space */
	    (strm->avail_in == 0))	      /* or no input data */
		return 0;

	tocopy = MIN(strm->avail_in, s->ibuf_avail);

	hw_trace("[%p]   *** collecting %d bytes ...\n", strm, tocopy);
	memcpy(s->ibuf, strm->next_in, tocopy);
	s->ibuf_avail -= tocopy;
	s->ibuf += tocopy;

	/* book-keeping for input buffer */
	strm->avail_in -= tocopy;
	strm->next_in += tocopy;
	strm->total_in += tocopy;

	return tocopy;
}

/**
 * Flush available output bytes to given stream.
 *
 * @strm   Compression stream used to push out data.
 * @return Remaining bytes in internal output buffer.
 */
static unsigned int h_flush_obuf(z_streamp strm)
{
	int tocopy;
	unsigned int obuf_bytes;
	struct hw_state *s = (struct hw_state *)strm->state;

	obuf_bytes = output_buffer_bytes(s);    /* remaining bytes in obuf */
	if (strm->avail_out == 0)		/* no output space available */
		return obuf_bytes;

	if (obuf_bytes == 0)			/* give out what is there */
		return obuf_bytes;

	tocopy = MIN(strm->avail_out, obuf_bytes);

	hw_trace("[%p]   *** giving out %d bytes, "
		 "remaining %d bytes in temporary, "
		 "%d in internal buffer\n",
		 strm, tocopy, obuf_bytes - tocopy,
		 zedc_inflate_pending_output(&s->h));

	memcpy(strm->next_out, s->obuf_next, tocopy);
	s->obuf_next += tocopy;
	s->obuf_avail += tocopy;  /* bytes were given out / FIXME (+)? */

	obuf_bytes = output_buffer_bytes(s);    /* remaining bytes in obuf */

	/* book-keeping for output buffer */
	strm->avail_out -= tocopy;
	strm->next_out += tocopy;
	strm->total_out += tocopy;

	return obuf_bytes;
}

/**
 * Optimization Remarks
 *
 * If ibuf_total is not 0 we use the allocated input and output
 * buffers instead of the user buffers. We collect the data into our
 * pre-pinnned buffers and compress when we have enough data or if
 * !Z_NO_FLUSH is true. When flushing is desired we ensure that we
 * always fill the available output buffer with data. The output data
 * comes from the pre-pinnned output buffer into the user buffer.
 *
 * We observed so far that using a 1 MiB buffer helps to improve
 * performance a lot if the input data is e.g. around 16 KiB per
 * request (zpipe.c defaults).
 */
int h_deflate(z_streamp strm, int flush)
{
	int rc = Z_OK, loops = 0;
	struct hw_state *s;
	zedc_stream *h;
	unsigned int obuf_bytes, ibuf_bytes;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	if (s->ibuf_total == 0) { /* Special case: buffering fully disabled */
		stream_zlib_to_zedc(h, strm);
		s->rc = rc_zedc_to_libz(__deflate(strm, s, flush));
		stream_zedc_to_zlib(strm, h);
		return s->rc;
	}

	__prep_crc_or_adler(strm, h);
	hw_trace("[%p] h_deflate: flush=%s avail_in=%d avail_out=%d "
		 "ibuf_avail=%d obuf_avail=%d adler32/cr32=%08x/%08x\n",
		 strm, flush_to_str(flush), strm->avail_in, strm->avail_out,
		 (int)s->ibuf_avail, (int)s->obuf_avail, h->adler32, h->crc32);

	do {
		hw_trace("[%p]   *** loop=%d flush=%s\n", strm, loops,
			 flush_to_str(flush));

		/* Collect input data ... */
		h_read_ibuf(strm);

		/* Give out what is already there */
		h_flush_obuf(strm);
		if (strm->avail_out == 0)	/* need more output space */
			return Z_OK;
		/*
		 * Here we start the hardware to do the compression
		 * job, user likes to flush or no more ibuf space
		 * avail.
		 */
		if ((flush != Z_NO_FLUSH) || (s->ibuf_avail == 0)) {
			ibuf_bytes = s->ibuf - s->ibuf_base; /* input bytes */

			hw_trace("[%p]   *** sending %d bytes to hardware ...\n",
				 strm, ibuf_bytes);

			s->obuf_next = h->next_out = s->obuf_base;  /* start */
			s->obuf_avail = s->obuf_total;
			h->next_in = s->ibuf_base;
			h->avail_in = ibuf_bytes;
			h->avail_out = s->obuf_total;

			/*
			 * If we still have more input data we must
			 * not tell hardware to finish/flush the
			 * compression stream. This happens if our
			 * buffer is smaller than the data the user
			 * provides.
			 */
			s->rc = rc_zedc_to_libz(__deflate(strm, s,
				  (strm->avail_in != 0) ? Z_NO_FLUSH : flush));

			s->obuf = h->next_out; /* end of output data */
			s->obuf_avail = h->avail_out;

			if (h->avail_in == 0) {	/* good: all input absorbed */
				s->ibuf = s->ibuf_base;
				s->ibuf_avail = s->ibuf_total;
			} else {
				pr_err("not all input absorbed! "
				       "avail_in is still %d bytes\n",
				       h->avail_in);
				return Z_STREAM_ERROR;
			}

			/* Sanity checking: obuf too small but input pending */
			if ((h->avail_in != 0) && (h->avail_out == 0)) {
				pr_err("obuf was not large enough!\n");
				return Z_STREAM_ERROR;
			}
		}

		if (strm->avail_in != 0)
			hw_trace("[%p] Not yet finished (avail_in=%d)\n",
				 strm, strm->avail_in);

		/* Give out what is already there */
		h_flush_obuf(strm);
		if (strm->avail_out == 0)	/* need more output space */
			return Z_OK;

		ibuf_bytes = s->ibuf - s->ibuf_base;  /* accumulated input */
		obuf_bytes = s->obuf - s->obuf_next;  /* bytes in obuf */

		if ((flush == Z_FINISH) &&	/* finishing desired */
		    (s->rc == Z_STREAM_END) &&	/* hardware saw FEOB */
		    (strm->avail_in == 0) &&	/* no more input from caller */
		    (ibuf_bytes == 0) &&	/* no more input in buf */
		    (obuf_bytes == 0))		/* no more outp data in buf */
			return Z_STREAM_END;	/* nothing to do anymore */

		loops++;
	} while (strm->avail_in != 0);

	return rc;
}

int h_deflateEnd(z_streamp strm)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;
	zedc_handle_t zedc;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;
	zedc = (zedc_handle_t)h->device;

	rc = zedc_deflateEnd(h);

	zedc_free(zedc, s->obuf_base, s->obuf_total, s->h.dma_type[ZEDC_OUT]);
	zedc_free(zedc, s->ibuf_base, s->ibuf_total, s->h.dma_type[ZEDC_IN]);
	__zedc_close(zedc);
	__free(s);
	return rc_zedc_to_libz(rc);
}

int h_inflateInit2_(z_streamp strm, int  windowBits,
		    const char *version __unused, int stream_size __unused)
{
	int rc, err_code = 0;
	struct hw_state *s;
	zedc_handle_t zedc;

	strm->total_in = 0;
	strm->total_out = 0;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return Z_MEM_ERROR;

	s->card_type = zlib_accelerator;
	s->card_no = zlib_card;
	s->mode = DDCB_MODE_ASYNC | DDCB_MODE_RDWR;

	if (zlib_inflate_flags & ZLIB_FLAG_USE_POLLING)
		s->mode |= DDCB_MODE_POLLING;

	hw_trace("[%p] h_inflateInit2_: card_type=%d card_no=%d "
		 "zlib_obuf_total=%d\n", strm, s->card_type, s->card_no,
		 zlib_obuf_total);

	zedc = __zedc_open(s->card_no, s->card_type, s->mode, &err_code);
	if (!zedc) {
		rc = Z_STREAM_ERROR;
		goto free_hw_state;
	}

	s->inflate_req = 0;
	s->h.avail_in = 0;
	s->h.next_in = ZEDC_NULL;
	s->h.device = zedc;

	/* Default is using SGLISTs */
	s->h.dma_type[ZEDC_IN]  = DDCB_DMA_TYPE_SGLIST;
	s->h.dma_type[ZEDC_OUT] = DDCB_DMA_TYPE_SGLIST;
	s->h.dma_type[ZEDC_WS]  = DDCB_DMA_TYPE_SGLIST;

	if (zlib_inflate_flags & ZLIB_FLAG_USE_FLAT_BUFFERS) {
		s->h.dma_type[ZEDC_IN]  = DDCB_DMA_TYPE_SGLIST;
		if (zlib_obuf_total != 0)
			s->h.dma_type[ZEDC_OUT] = DDCB_DMA_TYPE_FLAT;

		/* FIXME FIXME */
		pr_err(" NOTE: Potential hardware bug. We might get DDCBs\n"
		       "       with timeouts: RETC=0x110, ATTN=0xe004\n");

		s->h.dma_type[ZEDC_WS]  = DDCB_DMA_TYPE_FLAT;
	}
#if defined(CONFIG_USE_PINNING)
	s->h.dma_type[ZEDC_IN]  |= DDCB_DMA_PIN_MEMORY;
	s->h.dma_type[ZEDC_OUT] |= DDCB_DMA_PIN_MEMORY;
	s->h.dma_type[ZEDC_WS]  |= DDCB_DMA_PIN_MEMORY;
#endif
	if (zlib_xcheck)  /* FIXME Not needed/supported for inflate */
		s->h.flags |= ZEDC_FLG_CROSS_CHECK;

	if (zedc_verbose & ZEDC_VERBOSE_DDCB)
		s->h.flags |= ZEDC_FLG_DEBUG_DATA;

	if (zlib_inflate_flags & ZLIB_FLAG_OMIT_LAST_DICT)
		s->h.flags |= ZEDC_FLG_SKIP_LAST_DICT;

	/* We only use output buffering for inflate */
	if (zlib_obuf_total) {
		s->obuf_total = s->obuf_avail = zlib_obuf_total;
		s->obuf_base = s->obuf = s->obuf_next =
			zedc_memalign(zedc, s->obuf_total,
				      s->h.dma_type[ZEDC_OUT]);

		if (s->obuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto close_card;
		}
	}

	rc = zedc_inflateInit2(&s->h, windowBits);
	__fixup_crc_or_adler(strm, &s->h);

	if (rc != ZEDC_OK) {
		rc = rc_zedc_to_libz(rc);
		goto free_obuf;
	}

	strm->state = (void *)s; /* remember hardware state */
	return rc_zedc_to_libz(rc);

 free_obuf:
	zedc_free(zedc, s->obuf_base, s->obuf_total, s->h.dma_type[ZEDC_OUT]);
 close_card:
	__zedc_close(zedc);
 free_hw_state:
	__free(s);
	return rc;
}

int h_inflateReset(z_streamp strm)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_inflateReset\n", strm);
	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	/* reset buffers */
	strm->total_in = 0;
	strm->total_out = 0;

	s->inflate_req = 0;
	s->obuf_avail = s->obuf_total;
	s->obuf       = s->obuf_base;
	s->obuf_next  = s->obuf_base;
	s->rc	      = Z_OK;

	if (h->tree_bits + h->pad_bits + h->scratch_ib + h->scratch_bits)
		hw_trace("[%p] warn: (0x%x 0x%x 0x%x 0x%x)\n", strm,
			 (unsigned int)h->tree_bits,
			 (unsigned int)h->pad_bits,
			 (unsigned int)h->scratch_ib,
			 (unsigned int)h->scratch_bits);
	rc = zedc_inflateReset(h);
	__fixup_crc_or_adler(strm, h);

	return rc_zedc_to_libz(rc);
}

int h_inflateReset2(z_streamp strm, int windowBits)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_inflateReset2(windowBits=%d)\n", strm, windowBits);
	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	/* reset buffers */
	strm->total_in = 0;
	strm->total_out = 0;

	s->inflate_req = 0;
	s->obuf_avail = s->obuf_total;
	s->obuf       = s->obuf_base;
	s->obuf_next  = s->obuf_base;
	s->rc	      = Z_OK;

	if (h->tree_bits + h->pad_bits + h->scratch_ib + h->scratch_bits)
		hw_trace("[%p] warn: (0x%x 0x%x 0x%x 0x%x)\n", strm,
			 (unsigned int)h->tree_bits,
			 (unsigned int)h->pad_bits,
			 (unsigned int)h->scratch_ib,
			 (unsigned int)h->scratch_bits);
	rc = zedc_inflateReset2(h, windowBits);
	__fixup_crc_or_adler(strm, h);

	return rc_zedc_to_libz(rc);
}

int h_inflateSetDictionary(z_streamp strm, const uint8_t *dictionary,
			   unsigned int dictLength)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_inflateSetDictionary dictionary=%p dictLength=%d\n",
		 strm, dictionary, dictLength);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;

	h = &s->h;

	rc = zedc_inflateSetDictionary(h, dictionary, dictLength);

	return rc_zedc_to_libz(rc);
}

int h_inflateGetDictionary(z_streamp strm, uint8_t *dictionary,
			   unsigned int *dictLength)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_inflateGetDictionary dictionary=%p &dictLength=%p\n",
		 strm, dictionary, dictLength);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL) {
		return Z_STREAM_ERROR;
	}
	h = &s->h;

	rc = zedc_inflateGetDictionary(h, dictionary, dictLength);

	return rc_zedc_to_libz(rc);
}

int h_inflateGetHeader(z_streamp strm, gz_headerp head)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;

	hw_trace("[%p] h_inflateGetHeader headerp=%p\n", strm, head);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	if (sizeof(*head) != sizeof(gzedc_header))
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	rc = zedc_inflateGetHeader(h, (gzedc_header *)head);
	return rc_zedc_to_libz(rc);
}

static inline int __inflate(z_streamp strm, struct hw_state *s, int flush)
{
	int rc;
	zedc_stream *h = &s->h;

	hw_trace("[%p] __inflate (%d): flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_in=%ld total_out=%ld "
		 "crc/adler=%08x/%08x\n",
		 strm, s->inflate_req, flush_to_str(flush), h->next_in,
		 h->avail_in, h->next_out, h->avail_out, h->total_in,
		 h->total_out, h->crc32, h->adler32);

	rc = zedc_inflate(h, flush);
	__fixup_crc_or_adler(strm, h);

	hw_trace("[%p] ________h (%d) flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_in=%ld total_out=%ld "
		 "crc/adler=%08x/%08x rc=%s\n", strm, s->inflate_req,
		 flush_to_str(flush), h->next_in, h->avail_in, h->next_out,
		 h->avail_out, h->total_in, h->total_out, h->crc32,
		 h->adler32, ret_to_str(rc));

	s->inflate_req++;
	return rc;
}

/**
 * FIXME Circumvention for hardware deficiency
 *
 * Our hardware does not continue processing input bytes, once it has
 * no output bytes anymore. This causes our hardware missing the FEOB
 * information which can be in empty blocks which follow the regular
 * data. Software would return Z_STREAM_END in those cases and not
 * Z_OK, which is expected by some applications e.g. the MongoDB zlib
 * compression engine.
 *
 * It is possible recall hardware inflate with at least one output
 * byte, to get the desired Z_STREAM_END information from the
 * hardware, at the cost of an additional DDCB, which is itself
 * expensive too.
 *
 * Empty blocks are added by hardware support code and the software
 * implementation in different fashions. Z_SYNC_FLUSH does similar
 * things too. Hardware support code adds an empty fixed huffman block
 * followed by another empty fixed huffman block with the BFINAL bit
 * on. Software uses just the latter.
 */

#define CONFIG_CIRCUMVENTION_FOR_Z_STREAM_END

enum stream_state {
	READ_HDR,
	COPY_BLOCK,
	FIXED_HUFFMAN,
	DYN_HUFFMAN,
};

static const char *state_str[] = {
	"READ_HDR", "COPY_BLOCK", "FIXED_HUFFMAN", "DYN_HUFFMAN"
};

struct stream_ending {
	uint8_t d[16];
	unsigned int proc_bits;	/* processed bits in current byte */
	unsigned int remaining_bytes;
	unsigned int avail_in;
	unsigned int idx;
	unsigned int in_hdr_scratch_len;
	enum stream_state state;
};

/**
 * Retrieve @bits from @e. Without moving the position forward.
 */
static inline int get_bits(struct stream_ending *e, unsigned int bits,
			   uint64_t *d)
{
	int rc = 0;
	unsigned int b, proc_bits, idx;
	uint64_t data = 0ull;

	for (proc_bits = e->proc_bits, idx = e->idx, b = 0; b < bits; idx++) {
		for (; proc_bits < 8 && b < bits; proc_bits++, b++) {
			data <<= 1ull;

			if (idx >= e->avail_in) {
				rc = 1;
				continue;  /* no valid bytes anymore */
			}
			if (e->d[idx] & (1 << proc_bits))
				data |= 1ull;
		}
		proc_bits = 0;	/* start new byte at bit offset 0 */
	}
	*d = data;
	return rc;
}

/**
 * Move the position forward by @bits bits.
 */
static inline int drop_bits(struct stream_ending *e, unsigned int bits)
{
	unsigned int idx;

	/* hw_trace("proc_bits=%d idx=%d ---> ", e->proc_bits, e->idx); */
	idx = e->idx + (e->proc_bits + bits) / 8;
	if (idx >= e->avail_in) {
		/* hw_trace("EOF\n"); */
		return 1;	/* we do not have such many bits */
	}

	e->idx = idx;
	e->proc_bits = (e->proc_bits + bits) % 8;
	/* hw_trace("proc_bits=%d idx=%d\n", e->proc_bits, e->idx); */
	return 0;
}

/**
 * Copy blocks have their length information synched on a byte
 * boundary.  We need this to move the stream forward to a byte
 * position.
 */
static inline int sync_to_byte(struct stream_ending *e)
{
	if (e->proc_bits == 0)
		return 0;

	e->proc_bits = 0;
	e->idx++;
	return 0;
}

/**
 * There can be leftover input bytes in the scratch section. This is
 * used to figure out how many bytes are there to be considered.
 */
static inline unsigned int __in_hdr_scratch_len(zedc_streamp strm)
{
	unsigned int len;

	len = strm->hdr_ib + strm->tree_bits + strm->pad_bits +
		strm->scratch_ib + strm->scratch_bits;

	return (uint32_t)(len / 8ULL);
}

/**
 * I think we should be able to derive the info if we are in a dynamic
 * huffman block via the 3 header bits. But anyways ...
 *
 * If there are tree bits defined, we are for sure in a dynamic
 * huffman block. In this case we do not know the dynamic huffman end
 * of block symbol, which prevents software parsing the information in
 * the remaining bytes. Do not apply the BFINAL dectection
 * circumvention in this case.
 *
 *        BTYPE specifies how the data are compressed, as follows:
 *           00 - no compression
 *           01 - compressed with fixed Huffman codes
 *           10 - compressed with dynamic Huffman codes
 *           11 - reserved (error)
 */
static inline int __in_hdr_bits(zedc_streamp strm)
{
	unsigned int headerarea_size =
		((strm->tree_bits + strm->hdr_ib + 63)/64) * 8;
	uint8_t btype = (strm->infl_stat & INFL_STAT_HDR_TYPE) >> 5;
	const char *btype_str[] = { "NO_COMPRESSION", "FIXED_HUFFMAN",
				    "DYNAMIC_HUFFMAN", "RESERVED" };

	hw_trace("SCRATCH BITS: headerarea_size=%d hdr_ib=%d tree_bits=%d "
		 "pad_bits=%d scratch_ib=%d scratch_bits=%d "
		 "infl_stat.hdr_type=%s\n",
		 headerarea_size, strm->hdr_ib, strm->tree_bits,
		 strm->pad_bits, strm->scratch_ib, strm->scratch_bits,
		 btype_str[btype]);

	return strm->tree_bits;
}

static inline void __reset_hdr_scratch_len(zedc_streamp strm)
{
	strm->hdr_ib = 0;
	strm->tree_bits = 0;
	strm->pad_bits = 0;
	strm->scratch_ib = 0;
	strm->scratch_bits = 0;
}

/**
 * NOTES: Missing are reading more data if we run out of space in our
 * temporary buffer, more testing for corner cases, figuring out if we
 * are really at a header-start position (talk to hardware team).
 *
 * Consider moving this code at the end of DDCB processing. This is
 * where it really belongs, to mimic the exact zlib software
 * behavior. It could easily be, that this simplifies testing a lot,
 * since one could use the exact amount of output bytes and insist on
 * seeing Z_STREAM_END as return code. Now we need to call inflate() a
 * 2nd time (even with avail_out == 0), to get the Z_STREAM_END return
 * code.
 */
static inline int __check_stream_end(z_streamp strm)
{
	int rc, ret = Z_OK;
	uint64_t d;
	struct stream_ending e;
	struct hw_state *s = (struct hw_state *)strm->state;;
	zedc_stream *h = &s->h;
	unsigned int len;
	uint8_t offs;

	if (zlib_inflate_flags & ZLIB_FLAG_DISABLE_CV_FOR_Z_STREAM_END) {
		hw_trace("[%p] ZLIB_FLAG_DISABLE_CV_FOR_Z_STREAM_END\n", strm);
		return Z_OK;	/* No circumvention desired */
	}

	/*
	 * Do not try this ZLIB or GZIP, were we
	 * expect adler32 or crc32/data_size in the
	 * stream trailer. We want the lowlevel lib to
	 * do the checksum processing in this case.
	 */
	if (h->format != ZEDC_FORMAT_DEFL)
		return Z_OK;	/* No circumvention needed */

	hw_trace("[%p] CONFIG_CIRCUMVENTION_FOR_Z_STREAM_END\n", strm);

	/*
	 * fprintf(zlib_log, "SCRATCH\n");
	 * ddcb_hexdump(zlib_log, h->wsp->tree, __in_hdr_scratch_len(h));
	 * fprintf(zlib_log, "NEXT_IN\n");
	 * ddcb_hexdump(zlib_log, strm->next_in, MIN(strm->avail_in,
	 * 		(unsigned int)0x20));
	 * fprintf(zlib_log, "in_hdr_scratch_len=%d proc_bits=%d\n",
	 *	__in_hdr_scratch_len(h), h->proc_bits);
	 */
	rc = __in_hdr_bits(h);
	if (rc != 0) {
		hw_trace("    __in_hdr_bits %d: cannot parse "
			 "dynamic huffman block, returning\n", rc);
		return Z_OK;
	}

	/* Copy input data in one contignous buffer before analyzing it */
	memset(&e, 0, sizeof(e));
	e.state = READ_HDR;
	e.proc_bits = h->proc_bits;
	e.remaining_bytes = sizeof(e.d);
	e.avail_in = 0;
	e.idx = 0;
	e.in_hdr_scratch_len = __in_hdr_scratch_len(h);

	len = MIN(e.in_hdr_scratch_len, e.remaining_bytes);
	memcpy(&e.d[e.avail_in], h->wsp->tree, len);
	e.remaining_bytes -= len;
	e.avail_in += len;

	len = MIN(strm->avail_in, e.remaining_bytes);
	memcpy(&e.d[e.avail_in], strm->next_in, len);
	e.remaining_bytes -= len;
	e.avail_in += len;

	hw_trace("Accumulated input data (__in_hdr_scratch_len=%d "
		 "strm->avail_in=%d):\n",
		 e.in_hdr_scratch_len, strm->avail_in);

	if (zlib_hw_trace_enabled())
		ddcb_hexdump(zlib_log, e.d, e.avail_in);

	/* Now let us have a look what we have here */
	while (1) {
		/* fprintf(zlib_log, "STATE: %s\n", state_str[e.state]); */
		switch (e.state) {
		case READ_HDR:
			hw_trace("READ_HDR\n");

			rc = get_bits(&e, 3, &d);
			hw_trace("    d=%08llx rc=%d\n", (long long)d, rc);
			if (rc)
				goto go_home;
			drop_bits(&e, 3);

			switch (d & 0x3) {
			case 0x0:
				e.state = COPY_BLOCK;
				break;
			case 0x1:
				e.state = DYN_HUFFMAN;
				/* we need to stop, since the end
				   symbol is unknown to us */
				goto go_home;
			case 0x2:
				e.state = FIXED_HUFFMAN;
				break;
			case 0x3:  /* error */
			default:
				goto go_home;
			}
			if (d & 0x4) {
				hw_trace("  Z_STREAM_END/BFINAL potentially "
					 "detected!\n");
				ret = Z_STREAM_END;
			}
			break;

		case FIXED_HUFFMAN:
			hw_trace("FIXED_HUFFMAN\n");

			rc = get_bits(&e, 7, &d);
			hw_trace("    d=%08llx, 00000000 indicates empty "
				 "FIXED_HUFFMAN\n",
				 (long long)d);
			if (rc)
				goto go_home;

			drop_bits(&e, 7);
			if (d != 0x0)  /* end of stream required here */
				goto go_home;

			e.state = READ_HDR;

			/* If we saw the BFINAL bit, we can safely exit */
			if (ret == Z_STREAM_END)
				goto sync_avail_in;
			break;

		case COPY_BLOCK:
			hw_trace("COPY_BLOCK\n");

			sync_to_byte(&e);
			rc = get_bits(&e, 32, &d);
			hw_trace("    d=%08llx, 0000ffff indicates empty "
				 "COPY_BLOCK\n", (long long)d);
			if (rc)
				goto go_home;
			drop_bits(&e, 32);

			if (d != 0x0000ffff)    /* 0000ffff required here */
				goto go_home;

			e.state = READ_HDR;

			/* If we saw the BFINAL bit, we can safely exit */
			if (ret == Z_STREAM_END)
				goto sync_avail_in;

			break;

		default:
			hw_trace("Brrr STATE: %s\n", state_str[e.state]);
			goto go_home;
		}
	}
 sync_avail_in:
	/*
	 * Only if we saw Z_STREAM_END and no problems understanding
	 * the empty HUFFMAN or COPY_BLOCKs arose, we sync up the
	 * stream.
	 *
	 * For DEFLATE and ZLIB we need to read the adler32 or
	 * the crc32 and the uncompressed data size to finally say
	 * that everything is right. So let us not use the circumvention
	 * in this case.
	 */

	/*
	 * e.idx:                  number of bytes which were analyzed
	 * e.in_hdr_scratch_len:   bytes taken from scratch buffer
	 */
	if (e.idx <= e.in_hdr_scratch_len)
		offs = 0;       /* no avail_in adjustment needed */
	else {			/* do not consider bytes from scratch area */
		 /* add 1 idx starts at 0 */
		offs = e.idx - e.in_hdr_scratch_len + 1;
		__reset_hdr_scratch_len(h);
	}

	strm->avail_in -= offs;
	strm->next_in += offs;
	strm->total_in += offs;

	hw_trace("    e.idx=%d e.in_hdr_scratch_len=%d offs=%d "
		 "next_in=%02x\n", e.idx, e.in_hdr_scratch_len, offs,
		 *strm->next_in);
	return ret;		/* more data or even Z_STREAM_END found */

 go_home:
	hw_trace("    Aborting search for Z_STREAM_END for now!\n");
	return Z_OK;		/* more data required */
}

/**
 * FIXME We use always the internal buffer. Using the external one
 *       results in minimal performance gain when using sgl-described
 *       buffers, but flat buffers are better anyways.
 */
int h_inflate(z_streamp strm, int flush)
{
	int rc = Z_OK, use_internal_buffer = 1;
	zedc_stream *h;
	struct hw_state *s;
	unsigned int loops = 0;
	unsigned int obuf_bytes;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;

	if (s->obuf_total == 0) { /* Special case: buffering fully disabled */
		stream_zlib_to_zedc(h, strm);
		s->rc = rc_zedc_to_libz(__inflate(strm, s, flush));
		stream_zedc_to_zlib(strm, h);
		return s->rc;
	}

	/* Use internal buffer if the given output buffer is smaller */
	if ((s->h.dma_type[ZEDC_OUT] & DDCB_DMA_TYPE_MASK) ==
	    DDCB_DMA_TYPE_SGLIST)
		use_internal_buffer = (s->obuf_total > strm->avail_out);

	hw_trace("[%p] h_inflate: flush=%s avail_in=%d avail_out=%d "
		 "ibuf_avail=%d obuf_avail=%d use_int_buf=%d\n",
		 strm, flush_to_str(flush), strm->avail_in,
		 strm->avail_out, (int)s->ibuf_avail, (int)s->obuf_avail,
		 use_internal_buffer);

	/* No progress possible (no more input and no buffered output):
	   Z_BUF_ERROR */
	obuf_bytes = s->obuf - s->obuf_next; /* bytes in obuf */
	if ((obuf_bytes == 0) && (zedc_inflate_pending_output(h) == 0)) {
		hw_trace("[%p] OBYTES_IN_DICT %d bytes (1) This must be 0!\n",
			strm, h->obytes_in_dict);

		if (s->rc == Z_STREAM_END)   /* hardware saw FEOB */
			return Z_STREAM_END; /* nothing to do anymore */

		/*
		 * NOTE: strm->avail_in can be 0 but some bytes might
		 *       still be in the scratch buffer. This causes
		 *       one of our test-cases to fail. So the criteria
		 *       when to return Z_BUF_ERROR is currently wrong.
		 *       Therefore disabling Z_BUF_ERROR return here.
		 *       This causes a small deviation from what software zlib
		 *       does in situations when there is no input data
		 *       available.
		 */
		/* if (strm->avail_in == 0)
		 *        return Z_BUF_ERROR;
		 */
	}

	do {
		hw_trace("[%p] loops=%d flush=%s\n", strm, loops,
			 flush_to_str(flush));

		/* Give out what is already there */
		obuf_bytes = h_flush_obuf(strm);

		if ((s->rc == Z_STREAM_END) &&	/* hardware/sw saw FEOB */
		    (obuf_bytes == 0)) {	/* no more output in buf */
			unsigned int rem_bytes;

			/* no more output in temp? */
			rc = zedc_read_pending_output(h, strm->next_out,
						strm->avail_out);
			if (rc < 0) {
				hw_trace("[%s] err: Read temp buffer rc=%d!\n",
					__func__, rc);
				return rc;
			}

			hw_trace("[%s] collected %d bytes from dict buffer\n",
				__func__, rc);
			strm->avail_out -= rc;
			strm->total_out += rc;

			rem_bytes = zedc_inflate_pending_output(h);
			if (rem_bytes != 0)
				return Z_OK;	/* call me again */

			return Z_STREAM_END;	/* nothing to do anymore */
		}
		if (((obuf_bytes != 0) || zedc_inflate_pending_output(h)) &&
		    (strm->avail_out == 0))
			return Z_OK;		/* need new output buffer */

		/*
		 * Original idea: Do not send 0 data to HW
		 *
		 * Why it is needed regardless:
		 *   If the underlying code buffers output data, we
		 *   need to call it to get this data. We need to trust
		 *   the lowlevel code not to call hardware if not needed,
		 *   since that would impact performance.
		 */
		if ((0 == strm->avail_in) &&
		    ((Z_NO_FLUSH      == flush) ||
		     (Z_PARTIAL_FLUSH == flush) ||
		     (Z_FULL_FLUSH    == flush)))
			return Z_OK;

		if (!output_buffer_empty(s)) {
			pr_err("[%p] obuf should be empty here!\n", strm);
			return Z_DATA_ERROR;
		}

		/*
		 * Here we start the hardware to do the decompression
		 * job. We need to use hardware in any case to
		 * determine if we have seen a final end of block
		 * condition.
		 */
		hw_trace("[%p] Sending avail_in=%d bytes to hardware "
			 "(obuf_total=%d)\n", strm, strm->avail_in,
			 (int)s->obuf_total);

		h->next_in = strm->next_in;	/* use stream input buffer */
		h->avail_in = strm->avail_in;
		h->total_in = strm->total_in;

		if (use_internal_buffer) {	/* entire buffer */
			h->next_out = s->obuf_next = s->obuf_base;
			h->avail_out = s->obuf_total;
		} else {
			h->next_out = strm->next_out;
			h->avail_out = strm->avail_out;
		}
		h->total_out = strm->total_out;

		/* Call hardware to perform the decompression task. */
		s->rc = rc_zedc_to_libz(__inflate(strm, s, flush));

		strm->next_in = (uint8_t *)h->next_in; /* new pos ... */
		strm->avail_in = h->avail_in;	/* new pos in input data */
		strm->total_in = h->total_in;	/* new pos in input data */
		strm->data_type = h->data_type;

		if (use_internal_buffer) {	/* entire buffer */
			s->obuf = h->next_out;		/* end of out data */
			s->obuf_avail = h->avail_out;	/* available bytes */
		} else {
			strm->next_out = h->next_out;
			strm->avail_out = h->avail_out;
			strm->total_out = h->total_out;
		}

		/* Give out what is already there */
		h_flush_obuf(strm);

		if (s->rc == Z_NEED_DICT)
			return s->rc;

		if ((s->rc == Z_STREAM_ERROR) ||
		    (s->rc == Z_DATA_ERROR)   ||
		    (s->rc == Z_BUF_ERROR))
			return s->rc;

#ifdef CONFIG_CIRCUMVENTION_FOR_Z_STREAM_END	/* For MongoDB PoC */
		/* FIXME Experimental check for Z_STREAM_END here */
		if ((s->rc != Z_STREAM_END) && (strm->avail_out == 0)) {
			int _rc;

			_rc = __check_stream_end(strm);
			if (_rc == Z_STREAM_END) {
				hw_trace("    Suppress Z_STREAM_END %zd %zd (2)\n",
					 s->obuf_avail, s->obuf_total);
				s->rc = Z_STREAM_END;
			}
			hw_trace("[%p] .......... flush=%s avail_in=%d "
				 "avail_out=%d __check_stream=%s (2)\n", strm,
				 flush_to_str(flush), strm->avail_in,
				 strm->avail_out, ret_to_str(rc));
		}
#endif
		/* Hardware saw FEOB and output buffer is empty */
		if ((s->rc == Z_STREAM_END) &&  output_buffer_empty(s) &&
		    (zedc_inflate_pending_output(h) == 0)) {
			hw_trace("[%p] OBYTES_IN_DICT %d bytes (2) Must be 0!\n",
				strm, h->obytes_in_dict);
			return Z_STREAM_END;	/* nothing to do anymore */
		}

		if (strm->avail_out == 0)	/* need more output space */
			return Z_OK;

		hw_trace("[%p] data_type 0x%x\n", strm, strm->data_type);
		if (strm->data_type & 0x80) {
			hw_trace("[%p] Z_DO_BLOCK_EXIT\n", strm);
			return s->rc;
		}

		loops++;
	} while (strm->avail_in != 0); /* strm->avail_out == 0 handled above */

	hw_trace("[%p] __________ flush=%s avail_in=%d avail_out=%d\n",
		 strm, flush_to_str(flush), strm->avail_in,
		 strm->avail_out);

	return rc_zedc_to_libz(rc);
}

int h_inflateEnd(z_streamp strm)
{
	int rc;
	zedc_stream *h;
	struct hw_state *s;
	zedc_handle_t zedc;
	int ibuf_bytes, obuf_bytes;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	s = (struct hw_state *)strm->state;
	if (s == NULL)
		return Z_STREAM_ERROR;
	h = &s->h;
	zedc = (zedc_handle_t)h->device;

	ibuf_bytes = s->ibuf - s->ibuf_base;  /* accumulated input */
	obuf_bytes = s->obuf - s->obuf_next;  /* bytes in obuf */
	if (ibuf_bytes || obuf_bytes)
		hw_trace("[%p] In/Out buffer not empty! ibuf_bytes=%d "
		       "obuf_bytes=%d\n", strm, ibuf_bytes, obuf_bytes);

	rc = zedc_inflateEnd(h);

	zedc_free(zedc, s->obuf_base, s->obuf_total, s->h.dma_type[ZEDC_OUT]);
	__zedc_close((zedc_handle_t)h->device);
	__free(s);
	return rc_zedc_to_libz(rc);
}

/**
 * ZEDC_VERBOSE:
 *   0x0000cczz
 *         ||||
 *         ||``== libzedc debug flags
 *         ``==== libcard debug flags
 *
 */
void zedc_hw_init(void)
{
	char *verb = getenv("ZLIB_VERBOSE");
	char *accel = getenv("ZLIB_ACCELERATOR");
	char *ibuf_s = getenv("ZLIB_IBUF_TOTAL");
	char *obuf_s = getenv("ZLIB_OBUF_TOTAL");
	char *card = getenv("ZLIB_CARD");
	char *xcheck_str = getenv("ZLIB_CROSS_CHECK");

	ddcb_set_logfile(zlib_log);
	zedc_set_logfile(zlib_log);

	if (verb != NULL) {
		int z, c;

		zedc_verbose = str_to_num(verb);
		c = (zedc_verbose & ZEDC_VERBOSE_LIBCARD_MASK) >> 8;
		z = (zedc_verbose & ZEDC_VERBOSE_LIBZEDC_MASK) >> 0;

		ddcb_debug(c);
		zedc_lib_debug(z);
	}

	if (accel != NULL) {
		if (strncmp(accel, "CAPI", 4) == 0)
			zlib_accelerator = DDCB_TYPE_CAPI;
		else
			zlib_accelerator = DDCB_TYPE_GENWQE;
	}

	if (card != NULL) {
		if (strncmp(card, "RED", 3) == 0)
			zlib_card = ACCEL_REDUNDANT;
		else
			zlib_card = atoi(card);
	}

	if (xcheck_str != NULL)
		zlib_xcheck = str_to_num(xcheck_str);

	if (ibuf_s != NULL)
		zlib_ibuf_total = str_to_num(ibuf_s);

	if (obuf_s != NULL)
		zlib_obuf_total = str_to_num(obuf_s);

	/*
	 * USE_FLAT_BUFFERS and CACHE_HANDLES only work for GenWQE.
	 */
	if (zlib_accelerator != DDCB_TYPE_GENWQE) {
		zlib_deflate_flags &= ~(ZLIB_FLAG_USE_FLAT_BUFFERS |
					ZLIB_FLAG_CACHE_HANDLES);
		zlib_inflate_flags &= ~(ZLIB_FLAG_USE_FLAT_BUFFERS |
					ZLIB_FLAG_CACHE_HANDLES);
	}
}

void zedc_hw_done(void)
{
	unsigned int card_no;
	int flags = (zlib_inflate_flags | zlib_deflate_flags);

	if (zlib_log != stderr) {
		zedc_set_logfile(NULL);
		ddcb_set_logfile(NULL);
	}

	if ((flags & ZLIB_FLAG_CACHE_HANDLES) == 0x0)
		return;

	for (card_no = 0; card_no <= ZEDC_CARDS_LENGTH; card_no++) {
		if (zedc_cards[card_no] == NULL)
			continue;
		zedc_close(zedc_cards[card_no]);
	}
}
