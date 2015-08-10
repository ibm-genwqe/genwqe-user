/*
 * Copyright 2015, International Business Machines
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

#ifndef MIN
#  define MIN(a,b)	({ __typeof__ (a) _a = (a); \
			   __typeof__ (b) _b = (b); \
			_a < _b ? _a : _b; })
#endif

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

#define ZEDC_VERBOSE_LIBCARD_MASK 0x0000ff00  /* debug flags for libcard */
#define ZEDC_VERBOSE_LIBZEDC_MASK 0x000000ff  /* debug flags for libzedc */
#define ZEDC_VERBOSE_DDCB	  0x00010000  /* dump DDCBs if requested */

static int zedc_verbose  = 0x00000000; /* verbosity flag */
static int inflate_flags = 0x00000000;
static int deflate_flags = 0x00000000;

/* Try to cache filehandles for faster access. Do not close them. */
static zedc_handle_t zedc_cards[128 + 1];

static zedc_handle_t __zedc_open(int card_no, int card_type, int mode,
				 int *err_code)
{
	int flags = (inflate_flags | deflate_flags);

	if ((flags & ZLIB_FLAG_CACHE_HANDLES) == 0x0)
		return zedc_open(card_no, card_type, mode,
				 err_code);

	if (card_no == -1) {
		if (zedc_cards[128])
			return zedc_cards[128];

		zedc_cards[128] = zedc_open(card_no, card_type, mode,
					    err_code);
		return zedc_cards[128];
	}

	if (card_no < 0 || card_no >= 128)
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
	int flags = (inflate_flags | deflate_flags);

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
static void __fixup_crc_or_adler( z_streamp s, zedc_streamp h)
{
	s->adler = (h->format == ZEDC_FORMAT_GZIP) ? h->crc32 : h->adler32;
}

static void __free(void *ptr)
{
	if (ptr == NULL)
		return;
	free(ptr);
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
	char *card = getenv("ZLIB_CARD");
	char *accel = getenv("ZLIB_ACCELERATOR");
	char *xcheck_str = getenv("ZLIB_CROSS_CHECK");
	char *ibuf_total_str = getenv("ZLIB_IBUF_TOTAL");
	int rc, err_code = 0;
	int xcheck = 1;	    /* default is yes, do hw cross checking */
	unsigned int ibuf_total = CONFIG_DEFLATE_BUF_SIZE;
	struct hw_state *s;
	zedc_handle_t zedc;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	strm->total_in = 0;
	strm->total_out = 0;

	if (ibuf_total_str != NULL)
		ibuf_total = str_to_num(ibuf_total_str);

	/* NOTE: It is very dangerous to turn this off!! */
	if (xcheck_str != NULL)
		xcheck = atoi(xcheck_str);

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return Z_MEM_ERROR;

	s->card_no = 0;
	s->card_type = DDCB_TYPE_GENWQE;

	if (card != NULL) {
		if (strncmp(card, "RED", 3) == 0)
			s->card_no = ACCEL_REDUNDANT;
		else
			s->card_no = atoi(card);
	}
	if (accel != NULL) {
		if (strncmp(accel, "CAPI", 4) == 0)
			s->card_type = DDCB_TYPE_CAPI;
		else
			s->card_type = DDCB_TYPE_GENWQE;
	}

	zedc = __zedc_open(s->card_no, s->card_type,
			   DDCB_MODE_ASYNC | DDCB_MODE_RDWR, &err_code);
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

	if (deflate_flags & ZLIB_FLAG_USE_FLAT_BUFFERS) {
		if (ibuf_total != 0) {
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
	if (xcheck)
		s->h.flags |= ZEDC_FLG_CROSS_CHECK;

	if (zedc_verbose & ZEDC_VERBOSE_DDCB)
		s->h.flags |= ZEDC_FLG_DEBUG_DATA;

	if (deflate_flags & ZLIB_FLAG_OMIT_LAST_DICT)
		s->h.flags |= ZEDC_FLG_SKIP_LAST_DICT;

	if (ibuf_total) {
		s->ibuf_total = s->ibuf_avail = ibuf_total;
		s->ibuf_base = s->ibuf = zedc_memalign(zedc, s->ibuf_total,
						s->h.dma_type[ZEDC_IN]);
		if (s->ibuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto close_card;
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
		 * some memory in most cases. What a pitty.
		 */
		s->obuf_total = s->obuf_avail = ibuf_total * 15/8 +
			page_size;

		s->obuf_base = s->obuf = s->obuf_next =
			zedc_memalign(zedc, s->obuf_total,
				      s->h.dma_type[ZEDC_OUT]);
		if (s->obuf_base == NULL) {
			rc = Z_MEM_ERROR;
			goto free_ibuf;
		}
	}

	hw_trace("[%p] h_deflateInit2_: card_no=%d card_type=%d ibuf_total=%d\n",
		 strm, s->card_no, s->card_type, ibuf_total);
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
			   DDCB_MODE_ASYNC | DDCB_MODE_RDWR, &err_code);
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

	hw_trace("[%p] h_deflate (%d): flush=%d next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d\n", strm, s->deflate_req, flush,
		 h->next_in, h->avail_in, h->next_out, h->avail_out);

	rc = zedc_deflate(h, flush);
	__fixup_crc_or_adler(strm, h);
	s->deflate_req++;

	hw_trace("[%p]            flush=%d next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d rc=%d\n", strm, flush,
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

	hw_trace("[%p] *** collecting %d bytes %p -> %p ...\n", strm, tocopy,
		 strm->next_in, s->ibuf);
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
 * Flush available output bytes
 */
static int h_flush_obuf(z_streamp strm)
{
	int tocopy;
	unsigned int obuf_bytes;
	struct hw_state *s = (struct hw_state *)strm->state;

	if (strm->avail_out == 0)		/* no output space available */
		return 0;

	obuf_bytes = s->obuf - s->obuf_next;    /* remaining bytes in obuf */
	if (obuf_bytes == 0)			/* give out what is there */
		return 0;

	tocopy = MIN(strm->avail_out, obuf_bytes);

	hw_trace("[%p] *** giving out %d bytes ...\n", strm, tocopy);
	memcpy(strm->next_out, s->obuf_next, tocopy);
	s->obuf_next += tocopy;
	s->obuf_avail += tocopy;  /* bytes were given out / FIXME (+)? */

	/* book-keeping for output buffer */
	strm->avail_out -= tocopy;
	strm->next_out += tocopy;
	strm->total_out += tocopy;

	return tocopy;
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
 * performance a lot if the input data is e.g. arround 16 KiB per
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

	hw_trace("[%p] h_deflate: flush=%d avail_in=%d avail_out=%d "
		 "ibuf_avail=%d obuf_avail=%d\n",
		 strm, flush, strm->avail_in, strm->avail_out,
		 (int)s->ibuf_avail, (int)s->obuf_avail);

	do {
		hw_trace("[%p]     loops=%d flush=%d %s\n", strm, loops, flush,
			 flush_to_str(flush));

		/* Collect input data ... */
		h_read_ibuf(strm);

		/* Give out what is already there */
		h_flush_obuf(strm);
		if (strm->avail_out == 0)	/* need more ouput space */
			return Z_OK;
		/*
		 * Here we start the hardware to do the compression
		 * job, user likes to flush or no more ibuf space
		 * avail.
		 */
		if ((flush != Z_NO_FLUSH) || (s->ibuf_avail == 0)) {
			ibuf_bytes = s->ibuf - s->ibuf_base; /* input bytes */

			hw_trace("[%p] *** sending %d bytes to hardware ...\n",
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
				pr_err("not all input absorbed!\n");
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
		if (strm->avail_out == 0)	/* need more ouput space */
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
	char *card = getenv("ZLIB_CARD");
	char *accel = getenv("ZLIB_ACCELERATOR");
	char *xcheck_str = getenv("ZLIB_CROSS_CHECK");
	char *ibuf_total_str = getenv("ZLIB_OBUF_TOTAL");
	int rc, err_code = 0;
	int xcheck = 1;	    /* default is yes, do hw cross checking */
	struct hw_state *s;
	unsigned int ibuf_total = CONFIG_INFLATE_BUF_SIZE;
	zedc_handle_t zedc;

	strm->total_in = 0;
	strm->total_out = 0;

	if (ibuf_total_str != NULL)
		ibuf_total = str_to_num(ibuf_total_str);

	/* NOTE: It is very dangerous to turn this off!! */
	if (xcheck_str != NULL)
		xcheck = atoi(xcheck_str);

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return Z_MEM_ERROR;

	s->card_no = 0;
	s->card_type = DDCB_TYPE_GENWQE;

	if (card != NULL) {
		if (strncmp(card, "RED", 3) == 0)
			s->card_no = ACCEL_REDUNDANT;
		else
			s->card_no = atoi(card);
	}
	if (accel != NULL) {
		if (strncmp(accel, "CAPI", 4) == 0)
			s->card_type = DDCB_TYPE_CAPI;
		else
			s->card_type = DDCB_TYPE_GENWQE;
	}

	hw_trace("[%p] h_inflateInit2_: card_no=%d card_type=%d ibuf_total=%d\n",
		 strm, s->card_no, s->card_type, ibuf_total);

	zedc = __zedc_open(s->card_no, s->card_type,
			   DDCB_MODE_ASYNC | DDCB_MODE_RDWR, &err_code);
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

	if (inflate_flags & ZLIB_FLAG_USE_FLAT_BUFFERS) {
		s->h.dma_type[ZEDC_IN]  = DDCB_DMA_TYPE_SGLIST;
		if (ibuf_total != 0)
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
	if (xcheck)	  /* FIXME Not needed/supported for inflate */
		s->h.flags |= ZEDC_FLG_CROSS_CHECK;

	if (zedc_verbose & ZEDC_VERBOSE_DDCB)
		s->h.flags |= ZEDC_FLG_DEBUG_DATA;

	if (inflate_flags & ZLIB_FLAG_OMIT_LAST_DICT)
		s->h.flags |= ZEDC_FLG_SKIP_LAST_DICT;

	/* We only use output buffering for inflate */
	if (ibuf_total) {
		s->obuf_total = s->obuf_avail = ibuf_total;
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

	hw_trace("[%p] h_inflateGetDictionary dictionary=%p dictLength=%p\n",
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

	hw_trace("[%p] h_inflate (%d): flush=%d next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_in=%ld total_out=%ld "
		 "crc/adler=%08x/%08x\n",
		 strm, s->inflate_req, flush, h->next_in, h->avail_in,
		 h->next_out, h->avail_out, h->total_in, h->total_out,
		 h->crc32, h->adler32);

	rc = zedc_inflate(h, flush);
	__fixup_crc_or_adler(strm, h);
	s->inflate_req++;

	hw_trace("[%p]            flush=%d next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_in=%ld total_out=%ld "
		 "crc/adler=%08x/%08x rc=%d %s\n",
		 strm, flush, h->next_in, h->avail_in, h->next_out,
		 h->avail_out, h->total_in, h->total_out, h->crc32,
		 h->adler32, rc, ret_to_str(rc));

	return rc;
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

	hw_trace("[%p] h_inflate: flush=%d %s avail_in=%d avail_out=%d "
		 "ibuf_avail=%d obuf_avail=%d use_int_buf=%d\n",
		 strm, flush, flush_to_str(flush), strm->avail_in,
		 strm->avail_out, (int)s->ibuf_avail, (int)s->obuf_avail,
		 use_internal_buffer);

	/* No progress possible (no more input and no buffered output):
	   Z_BUF_ERROR */
	obuf_bytes = s->obuf - s->obuf_next; /* bytes in obuf */
	if (obuf_bytes == 0) {
		if (s->rc == Z_STREAM_END)   /* hardware saw FEOB */
			return Z_STREAM_END; /* nothing to do anymore */

		if (strm->avail_in == 0)
			return Z_BUF_ERROR;
	}

	do {
		hw_trace("[%p] loops=%d flush=%d %s\n", strm, loops, flush,
			 flush_to_str(flush));

		/* Give out what is already there */
		h_flush_obuf(strm);

		obuf_bytes = s->obuf - s->obuf_next;  /* bytes in obuf */
		if ((s->rc == Z_STREAM_END) &&	/* hardware saw FEOB */
		    (obuf_bytes == 0))		/* no more outp data in buf */
			return Z_STREAM_END;	/* nothing to do anymore */

		if (strm->avail_out == 0)	/* need more ouput space */
			return Z_OK;

		if (s->obuf_avail != s->obuf_total) {
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
		h->avail_in = strm->avail_in;	/* use stream input buffer */
		h->total_in = strm->total_in;

		if (use_internal_buffer) {	/* entire buffer */
			h->next_out = s->obuf_next = s->obuf_base;
			h->avail_out = s->obuf_total;
		} else {
			h->next_out = strm->next_out;
			h->avail_out = strm->avail_out;
		}
		h->total_out = strm->total_out;

		/* FIXME HACK to limit the output buffer mapping effort! */
		//if (flush == Z_FINISH)
		//	h->avail_out =
		//		((h->avail_in * 4) + s->page_size - 1) &
		//		~(s->page_size - 1);
		// Does not work yet.

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

		obuf_bytes = s->obuf - s->obuf_next;  /* bytes in obuf */
		if ((s->rc == Z_STREAM_END) &&	/* hardware saw FEOB */
		    (obuf_bytes == 0))		/* no more outp data in buf */
			return Z_STREAM_END;	/* nothing to do anymore */

		if (strm->avail_out == 0)	/* need more ouput space */
			return Z_OK;
		hw_trace("[%p] data_type 0x%x\n", strm, strm->data_type);
		if (strm->data_type & 0x80) {
			hw_trace("[%p] Z_DO_BLOCK_EXIT\n", strm);
			return s->rc;
		}

		loops++;
	} while (strm->avail_in != 0); /* strm->avail_out == 0 handled above */

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
		pr_err("[%p] In/Out buffer not empty! ibuf_bytes=%d "
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
	const char *inflate_impl = getenv("ZLIB_INFLATE_IMPL");;
	const char *deflate_impl = getenv("ZLIB_DEFLATE_IMPL");;

	if (verb != NULL) {
		int z, c;

		zedc_verbose = str_to_num(verb);
		c = (zedc_verbose & ZEDC_VERBOSE_LIBCARD_MASK) >> 8;
		z = (zedc_verbose & ZEDC_VERBOSE_LIBZEDC_MASK) >> 0;

		ddcb_debug(c);
		zedc_lib_debug(z);
	}

	if (inflate_impl != NULL)
		inflate_flags = strtol(inflate_impl, (char **)NULL, 0) &
			~ZLIB_IMPL_MASK;

	if (deflate_impl != NULL)
		deflate_flags = strtol(deflate_impl, (char **)NULL, 0) &
			~ZLIB_IMPL_MASK;
}

void zedc_hw_done(void)
{
}
