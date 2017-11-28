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

/*
 * @brief This part of the libzedc library is responsible to perform
 * decompression (inflate) of the compressed data. The library
 * supports the data formats described in RFC1950, RFC1951, and
 * RFC1952.
 *
 * IBM Accelerator Family 'GenWQE'/zEDC
 */

/****************************************************************************
 * DeCompression (Inflate)
 ***************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <malloc.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <asm/byteorder.h>
#include <assert.h>
#include <stdbool.h>

#include <libcard.h>
#include <libzHW.h>
#include "hw_defs.h"

#define	INFLATE_HDR_OK			0
#define	INFLATE_HDR_NEED_MORE_DATA	1
#define	INFLATE_HDR_ZLIB_NEED_DICT	2
#define	INFLATE_HDR_ERROR		3

/**
 * @brief	estimate the amount of bytes consumed
 *		solely from the input stream
 */
static uint32_t inp_proc_update(uint32_t inp_processed, uint32_t proc_bits,
				uint32_t pre_scratch_bits)
{
	uint64_t in_total;	/* Bits total */

	/* total amount of bits consumed by decompressor */
	in_total  = (uint64_t)inp_processed * 8;
	in_total += (uint64_t)proc_bits;
	in_total -= (uint64_t)pre_scratch_bits;
	in_total = (in_total + 7ULL) / 8ULL;

	return (uint32_t)in_total; /* Return number of bytes consumed */
}

static void extract_new_tree(zedc_streamp strm)
{
	uint8_t *target;
	const uint8_t *src;
	uint64_t in_scratch_bytes;
	uint64_t hdr_offs;
	uint64_t hdr_start_total_bits;
	unsigned cnt;
	int64_t src_offs;

	in_scratch_bytes = (strm->scratch_bits + strm->scratch_ib) / 8;

	/* new tree detected (hdr_start > 0) */
	/* offs relative to first bit in scratch/data area */
	hdr_start_total_bits = (uint64_t)strm->hdr_start * 8 +
		strm->out_hdr_start_bits;

	hdr_offs = hdr_start_total_bits -
		strm->in_hdr_bits + strm->scratch_ib;

	strm->hdr_ib = hdr_offs % 8; /* ignore bits, rel to byte offs */
	cnt  = ((uint64_t)strm->out_hdr_bits + strm->hdr_ib + 7)/8;

	target = strm->wsp->tree;
	/* offset relative to beginning of input data area */
	src_offs = hdr_offs/8 - in_scratch_bytes;

	if ((hdr_start_total_bits == 0) && (strm->in_hdr_bits == 0)) {
		/*
		 * We didn't have a header before, tree starts in
		 * scratch/data avoid copying bytes that exist in
		 * scratch already, because scratch becomes the new
		 * tree.
		 */
		if (cnt > in_scratch_bytes) {
			cnt -= in_scratch_bytes;
			target += in_scratch_bytes;
			src_offs += in_scratch_bytes;
		} else
			cnt = 0;
	}

	/* NOTE: This is the same as in scratch_update() */
	/* copy abs(src_offs) bytes behind the tree ... */
	if (cnt > 0 && src_offs < 0) {
		src = strm->wsp->tree + strm->in_hdr_scratch_len + src_offs;
		memmove(target, src, abs(src_offs));
		target += abs(src_offs);
		cnt -= abs(src_offs);
		src_offs = 0;
	}
	/* copy remaining cnt bytes ... */
	if (cnt) {
		src = strm->next_in + src_offs;
		memmove(target, src, cnt);
	}

	strm->tree_bits = strm->out_hdr_bits;

	/* padding bits derived from actual tree */
	if (strm->tree_bits > 0) {
		strm->pad_bits  = 64ULL - ((strm->hdr_ib +
					       strm->tree_bits) % 64ULL);
		strm->pad_bits &= 63ULL;
	} else
		strm->pad_bits = 0;
}

/* Call this after tree update */
static void scratch_update(zedc_streamp strm)
{
	uint8_t *target;
	const uint8_t *src;
	uint64_t in_scratch_bytes;
	uint64_t scratch_offs;
	unsigned cnt;
	int64_t src_offs;

	in_scratch_bytes = (strm->scratch_bits + strm->scratch_ib) / 8;
	/* new tree detected (hdr_start > 0) */
	/* offs relative to first bit in scratch/data area */
	scratch_offs = (uint64_t)strm->inp_processed * 8ULL
			+ (uint64_t)strm->proc_bits
			- strm->in_hdr_bits
			+ strm->scratch_ib;

	/* target = start of scratch with new tree. */
	target = strm->wsp->tree +
		((strm->tree_bits + strm->hdr_ib + 63) & 0xFFFFFFC0) / 8;

	/* current processing offset relative to begin of input data area */
	src_offs = scratch_offs / 8 - in_scratch_bytes;
	if (src_offs >= 0) {
		cnt = (scratch_offs % 8) ? 1 : 0;
		strm->inp_data_offs = src_offs + cnt;
	} else {
		/* scratch bytes must at least persist */
		strm->inp_data_offs = 0;
		cnt = in_scratch_bytes - scratch_offs/8;
	}
	/* if output buffer is NOT full, copy all remaining input bytes
	   if output buffer is full, copy only a partial byte to scratch. */

	if (strm->avail_out > strm->outp_returned) {		/* not full */
		/* take into account if format != DEFLATE */
		cnt += strm->avail_in - strm->inp_data_offs;
		strm->inp_data_offs = strm->avail_in;
	}

	strm->scratch_bits = cnt * 8 - (scratch_offs % 8);
	strm->scratch_ib = scratch_offs % 8;

	/* NOTE: This is the same as in extract_new_tree() */
	/* copy abs(src_offs) bytes behind the tree ... */
	if (cnt > 0 && src_offs < 0) {
		src = strm->wsp->tree + strm->in_hdr_scratch_len + src_offs;
		memmove(target, src, abs(src_offs));
		target += abs(src_offs);
		cnt -= abs(src_offs);
		src_offs = 0;
	}
	/* copy remaining cnt bytes ... */
	if (cnt) {
		src = strm->next_in + src_offs;
		memmove(target, src, cnt);
	}
}

/**
 * @brief Procsses header and tree area in workspace
 *
 * HW reported that a complete tree was found. Called when hdr-bits
 * are provided and none written so far.
 *
 * HW reported that a complete tree was found. HDR_START then
 * represents offset in decomp's input-data which is composed of HDR +
 * TREE + SCRATCH + INPUT_STREAM Copy header to start of tree area in
 * workspace.
 */
static void setup_tree(zedc_streamp strm)
{
	uint64_t hdr_start_total_bits;

	/*
	 * If End-Of-Block has been passed or reached, all tree
	 * parameters are obsolete, a new tree is expected.
	 */
	if (strm->infl_stat & INFL_STAT_PASSED_EOB) {
		strm->tree_bits    = 0;
		strm->pad_bits     = 0;
		strm->hdr_ib       = 0;
		if (strm->infl_stat & INFL_STAT_REACHED_EOB) { /* on eob */
		     strm->out_hdr_bits = 0;
		     strm->out_hdr_start_bits = 0;	/* got from DDCB */
		}
		if (strm->infl_stat & INFL_STAT_FINAL_EOB) {
			strm->inp_data_offs = strm->in_data_used;
			strm->scratch_bits = 0;
			strm->eob_seen = 1;	/* final EOB seen */
			return;
		}
	}

	hdr_start_total_bits = (uint64_t)strm->hdr_start * 8 +
		strm->out_hdr_start_bits;

	/* Have we found a NEW header? */
	/*
	 * out_hdr_bits must indicate a header but it's not a new
	 * header if header start/_bits == 0, and in_hdr_bit != 0
	 * (tree exists at offset 0 of given tree)
	 */
	if ((strm->out_hdr_bits > 0) &&
	    ((hdr_start_total_bits > 0) || (strm->in_hdr_bits == 0))) {
		strm->tree_bits = strm->out_hdr_bits;
		extract_new_tree(strm);
	}
	scratch_update(strm);
}

/**
 * @brief	if an EOB marker was passed all tree and scratch data become
 *		obsolete. If HDR_START > 0 then copy tree data from
 *		input buffer to scratch area.
 *		As long as input_processed = 0 input data must be
 *		collected in scratch.
 *		If a valid tree is detected (out_hdr_bits > 0) the tree must
 *		be conserved in scratch and padding bytes must be appended.
 *
 * @param strm	inflate job context
 * @param asv	pointer to ASV part of DDCB
 */
static int post_scratch_upd(zedc_streamp strm)
{
	const uint8_t *src;
	uint8_t *target;
	uint16_t len;
	uint64_t count;
	zedc_handle_t zedc = (zedc_handle_t)strm->device;

	/* anything processed ? */
	if (strm->inp_processed || strm->proc_bits) {
		count = inp_proc_update(strm->inp_processed,
					strm->proc_bits,
					strm->pre_scratch_bits);
		strm->in_data_used = count;
		setup_tree(strm);
	}

	/* if no input data processed copy input-data to tree area */
	if ((strm->inp_processed == 0) && (strm->proc_bits == 0)) {
		/* Special CASE: empty Input */
		if (strm->avail_in >
		    (ZEDC_TREE_LEN - strm->in_hdr_scratch_len)) {

			pr_err("scratch buffer too small\n");
			zedc->zedc_rc = ZEDC_ERR_TREE_OVERRUN;
			return zedc->zedc_rc;
		}
		if (strm->avail_in) {
			target = strm->wsp->tree + strm->in_hdr_scratch_len;
			src = (uint8_t *)strm->next_in;
			memcpy(target, src, strm->avail_in);
			strm->inp_data_offs += strm->avail_in;
			strm->scratch_bits  += strm->avail_in * 8;
		}
	}

	/*
	 * If we cut within a copyblock a new header must be provided
	 * representing the remaining bytes in the block. Overwriting
	 * tree is valid because copy blocks always end on a byte
	 * boundary. OUT_HDR_BITS will always be 40 header type must
	 * be checked (HW 243728).
	 */
	if ((strm->copyblock_len) &&
	    ((strm->infl_stat & INFL_STAT_HDR_TYPE) == 0) &&
	    (strm->out_hdr_bits != 0)) {

		target = strm->wsp->tree;

		len  = strm->copyblock_len;
		if (strm->infl_stat & INFL_STAT_HDR_BFINAL) /* final block? */
			target[0] = 0x01;	/* restore final block */
		else
			target[0] = 0x00;

		*(uint16_t *)(target + 1) = __cpu_to_le16(len);
		*(uint16_t *)(target + 3) = __cpu_to_le16(~len);
		*(uint16_t *)(target + 5) = 0xaaaa;	/* dummy */

		strm->hdr_ib    = 0;
		strm->tree_bits = 40;	/* 5 bytes */
		strm->pad_bits  = 24;	/* total 64bit */
	}
	return ZEDC_OK;
}

/**
 * @brief	Remove ZLIB header from inflate stream
 *		ZLIB has two fixed header bytes and optionally
 *		a four byte Dictionary ID
 * @param strm	inflate stream context
 * @return	< 0	if compliance check fails
 *		 0	if success
 *
 * A zlib stream has the following structure:
 *
 *           0   1
 *         +---+---+
 *         |CMF|FLG|   (more-->)
 *         +---+---+
 *
 *      (if FLG.FDICT set)
 *
 *           0   1   2   3
 *         +---+---+---+---+
 *         |     DICTID    |   (more-->)
 *         +---+---+---+---+
 *
 *         +=====================+---+---+---+---+
 *         |...compressed data...|    ADLER32    |
 *         +=====================+---+---+---+---+
 */
static int inflate_rem_zlib_header(struct zedc_stream_s *strm)
{
	uint16_t val16;
	head_state	next_state = strm->header_state; /* Current State */
	bool more_data = false;
	int	rc  = INFLATE_HDR_OK;

	if (strm->prefx_idx < 1) {	/* min header bytes collected ? */
		strm->header_state = HEADER_START;
		return INFLATE_HDR_NEED_MORE_DATA;
	}

	while ((next_state != HEADER_DONE) && (false == more_data)) {
		switch (next_state) {
		case HEADER_START:
			if (strm->prefx_idx == 1) {
				val16 = ((uint16_t)strm->prefx[0] << 8) +
					strm->prefx[1];
				if ((val16 % 31) != 0) {
					pr_err("ZLIB header invalid (FCHECK)\n");
					return INFLATE_HDR_ERROR;
				}
				/* check CMF */
				if (((val16 & 0x0f00) != 0x0800) ||
					((val16 & 0xf000) > 0x7000)) {
					pr_err("ZLIB header invalid (CMF)\n");
					return INFLATE_HDR_ERROR;
				}
				if (val16 & 0x0020) { /* bit 5 of FLG = FDICT */
					next_state = ZLIB_ADLER;
					more_data = true;
				} else next_state = HEADER_DONE;
			} else more_data = true;
			break;
		case ZLIB_ADLER:
			if (strm->prefx_idx == 5) {
				/* zlib header with adler32 data ... */
				strm->dict_adler32 =
					((uint32_t)strm->prefx[2] << 24 |
					(uint32_t)strm->prefx[3] << 16 |
					(uint32_t)strm->prefx[4] << 8  |
					(uint32_t)strm->prefx[5]);
					strm->adler32 = strm->dict_adler32;
					strm->havedict = 0;
				next_state = HEADER_DONE;
				rc = INFLATE_HDR_ZLIB_NEED_DICT;
			} else more_data = true;
			break;
		case HEADER_DONE:
		default:
			break;
		}
	}

	strm->header_state = next_state;
	if (more_data)
		rc =  INFLATE_HDR_NEED_MORE_DATA;
	return rc; /* can be INFLATE_HDR_OK or INFLATE_HDR_ZLIB_NEED_DICT */
}

/**
 * @brief	Remove GZIP header from inflate stream
 *		GZIP can have a variable amount of header data
 *		depending on the FLAGs set. Re-enter until all
 *		flags are processed
 *
 * @param strm	inflate stream context
 *
 * @return	0	success
 *		1	need more data
 *		3	compliance check fails
 */
static int inflate_rem_gzip_header(struct zedc_stream_s *strm)
{
	uint8_t	flg;				/* GZIP FLG Byte */
	struct gzedc_header_s	*gz_h;
	int my_idx = 0;
	head_state	next_state = strm->header_state; /* Current State */
	bool more_data = false;

	if (strm->prefx_idx < 9)	/* min header bytes collected ? */
		return INFLATE_HDR_NEED_MORE_DATA;	/* Get more data */

	gz_h = strm->gzip_head;
	if (strm->prefx_idx == 9)
		strm->header_state = HEADER_START; /* Current State  = Start */

	flg = strm->prefx[3];			   /* Get FLG Byte */

	while ((next_state != HEADER_DONE) && (false == more_data)) {
		switch (next_state) {
		case HEADER_START:
			if ((strm->prefx[0] != 0x1f) ||		/* ID1 */
			    (strm->prefx[1] != 0x8b) ||		/* ID2 */
			    (strm->prefx[2] != 0x08)) {		/* CM */
				return INFLATE_HDR_ERROR;	/* Fault */
			}
			if (gz_h) {
				/* Get time, xflags and os */
				unsigned int tmp;
				memcpy(&tmp, &strm->prefx[4], 4);
				gz_h->time   = __le32_to_cpu(tmp);
				gz_h->xflags = strm->prefx[8];
				gz_h->os     = strm->prefx[9];
			}
			/* next is check flag */
			next_state = FLAGS_CHECK_EMPTY;
			break;
		case FLAGS_CHECK_EMPTY:
			if (flg == 0)
				next_state = HEADER_DONE;
			else next_state = FLAGS_CHECK_EXTRA;
			break;
		case FLAGS_CHECK_EXTRA:
			if (flg & 0x04) { /* FEXTRA bit set ? */
				more_data = true;
				next_state = FLAGS_GET_EXTRA_LEN1;
				/* FNAME is next */
			} else next_state = FLAGS_CHECK_FNAME;
			break;
		case FLAGS_GET_EXTRA_LEN1:
			strm->xlen  = (uint16_t)*strm->next_in;
			/* Reset Index to get extra data */
			strm->gzip_header_idx = 0;
			more_data = true;
			next_state = FLAGS_GET_EXTRA_LEN2; /* Next State */
			break;
		case FLAGS_GET_EXTRA_LEN2:
			strm->xlen |= (uint16_t)*strm->next_in << 8;
			if (gz_h)		      /* Save for get Header */
				gz_h->extra_len = strm->xlen;
			next_state = FLAGS_GET_EXTRA; /* Next State */
			more_data = true;
			break;
		case FLAGS_GET_EXTRA:
			/* get Extra binary data */
			if (1 == strm->xlen) {
				/* FNAME is Next State */
				next_state = FLAGS_CHECK_FNAME;
				more_data = false;
			} else {
				strm->xlen--;
				more_data = true;
			}
			if (gz_h) {
				my_idx = strm->gzip_header_idx; /* Get index */
				if (my_idx < (int)gz_h->extra_max) {
					gz_h->extra[my_idx++] = *strm->next_in;
					strm->gzip_header_idx = my_idx;
					/* and save back */
				} else return INFLATE_HDR_ERROR;/* Fault */
			}
			break;
		case FLAGS_CHECK_FNAME:
			if (flg & 0x08) { /* FNAME bit set ? */
				next_state = FLAGS_GET_FNAME;
				more_data = true;
				strm->gzip_header_idx = 0; /* Reset index */
			} else next_state = FLAGS_CHECK_FCOMMENT;
			break;
		case FLAGS_GET_FNAME:
			if (gz_h) {
				my_idx = strm->gzip_header_idx;	/* Get index */
				if (my_idx < (int)gz_h->name_max) {
					gz_h->name[my_idx++] = *strm->next_in;
					strm->gzip_header_idx = my_idx;
					/* and save back */
				} else return INFLATE_HDR_ERROR; /* Fault */
			}
			if (*strm->next_in == 0)
				next_state = FLAGS_CHECK_FCOMMENT;
			/* check FCOMMENT */
			else more_data = true;
			break;
		case FLAGS_CHECK_FCOMMENT:
			if (flg & 0x10) {
				/* FCOMMENT bit set ? */
				more_data = true;
				/* get FCOMMENT */
				next_state = FLAGS_GET_FCOMMENT;
				/* Reset index */
				strm->gzip_header_idx = 0;
			} else next_state = FLAGS_CHECK_FHCRC;
			break;
		case FLAGS_GET_FCOMMENT:
			if (gz_h) {
				my_idx = strm->gzip_header_idx;
				/* Get index */
				if (my_idx < (int)gz_h->comm_max) {
					gz_h->comment[my_idx++] =
						*strm->next_in;
					strm->gzip_header_idx = my_idx;
					/* and save back */
				} else return INFLATE_HDR_ERROR; /* Fault */
			}
			if (*strm->next_in == 0)
				next_state = FLAGS_CHECK_FHCRC;
			/* FHCRC is Next State */
			else more_data = true;
			/* Get more data */
			break;
		case FLAGS_CHECK_FHCRC:
			if (flg & 0x02) { /* FHCRC bit set ? */
				more_data = true;
				next_state = FLAGS_GET_FHCRC1;
			} else next_state = FLAGS_CHECK_FTEXT;
			break;
		case FLAGS_GET_FHCRC1:
			strm->gzip_hcrc = (uint16_t)*strm->next_in;
			/* Get 1st Byte */
			next_state = FLAGS_GET_FHCRC2;
			/* Next is 2nd byte */
			more_data = true;
			break;
		case FLAGS_GET_FHCRC2:
			/* 2nd byte of FHCRC */
			strm->gzip_hcrc |= (uint16_t)*strm->next_in << 8;
			/* Need more work here to compare deflate and
			   inflate */
			next_state = FLAGS_CHECK_FTEXT;
			/* Check FTEXT */
			break;
		case FLAGS_CHECK_FTEXT:
			if (flg & 0x01) {
				/* FTEXT bit set ? */
				if (gz_h) gz_h->text = 1;
				/* Set Text flag */
			}
			next_state = HEADER_DONE; /* Exit while */
			break;
		case HEADER_DONE: /* never reach this */
		default:
			/* only to make gcc happy */
			break;
		}
	}

	if (HEADER_DONE == next_state) {
		if (gz_h) gz_h->done = 1;
	}
	strm->header_state = next_state;
	if (more_data)
		return INFLATE_HDR_NEED_MORE_DATA;
	return INFLATE_HDR_OK;
}

/**
 * @brief	Remove header from GZIP or ZLIB files to get a plain
 *		inflate coded stream
 * @param strm	inflate stream context
 * @return	< 0	if GZIP/ZLIB header is invalid
 *		 0	if success
 */
static int inflate_format_rem_header(struct zedc_stream_s *strm, int flush)
{
	int rc, rc1;
	int	block_req = 0;
	zedc_handle_t zedc = NULL;

	if (strm->format == ZEDC_FORMAT_DEFL)
		return ZEDC_OK;		/* no header for DEFLATE/INFLATE */

	strm->data_type &= ~0x80;
	if (strm->prefx_len == 0) {	/* removing not yet prepared ? */
		strm->prefx_idx = 0;
		if (strm->format == ZEDC_FORMAT_GZIP)
			strm->prefx_len = 10;	/* min bytes to remove */
		else
			strm->prefx_len = 2;	/* format = ZLIB */
		if (ZEDC_BLOCK == flush)
			block_req = 1;
	}
	/*
	 * Copy header bytes to local buffer
	 * GZIP can have 'Extra Bytes' and 'Filename' in header.
	 *
	 * Restructuring this loop might help to avoid calling the
	 * rem_*_header() functions too often.
	 */
	rc = ZEDC_OK;
	rc1 = 0;
	while (strm->avail_in) {
		if (strm->prefx_idx < ZEDC_FORMAT_STORAGE)
			strm->prefx[strm->prefx_idx] = *strm->next_in;

		if (strm->format == ZEDC_FORMAT_GZIP)
			rc1 = inflate_rem_gzip_header(strm);
		else
			rc1= inflate_rem_zlib_header(strm);

		strm->next_in++;
		strm->avail_in--;
		strm->total_in++;
		strm->prefx_idx++;

		if (INFLATE_HDR_OK == rc1) {
			rc = ZEDC_OK;
			break;
		} else if (INFLATE_HDR_ERROR == rc1) {
			zedc = (zedc_handle_t)strm->device;
			zedc->zedc_rc = ZEDC_ERR_GZIP_HDR;
			rc = ZEDC_ERR_GZIP_HDR;
			break;	/* Error */
		} else if (INFLATE_HDR_ZLIB_NEED_DICT == rc1) {
			rc = ZEDC_NEED_DICT;
			break;
		}

		/* Continue with INFLATE_HDR_MORE_DATA */
	}

	if ((1 == block_req) && (ZEDC_OK == rc))
		strm->data_type |= 0x80;	/* Set Signal in data_type */

	return rc;
}

/**
 * @brief	Remove trailer from gzip (RFC1952) or ZLIB (RFC1950)
 *		encoded files
 *		A signal is needed to indicate End-Of-Final-Block has
 *		been detected
 * @param strm	decompression job context
 */
static int inflate_format_rem_trailer(struct zedc_stream_s *strm)
{
	uint32_t val32[2];
	zedc_handle_t zedc = (zedc_handle_t)strm->device;

	if (strm->format == ZEDC_FORMAT_DEFL)
		return ZEDC_OK;		/* no trailer for DEFLATE/INFLATE */

	if (strm->postfx_len == 0) {	/* removing not yet prepared ? */
		strm->postfx_idx  = 0;
		if (strm->format == ZEDC_FORMAT_GZIP)
			strm->postfx_len = 8;  /* GZIP: LEN/CRC32 */
		else
			strm->postfx_len = 4;  /* ZLIB: ADLER32 */
	}

	/* save trailer to local buffer */
	while ((strm->postfx_idx < strm->postfx_len) && strm->avail_in) {

		/* Can postfx_idx exceed the size of the postfx buffer
		   if the input data is too large? */

		strm->postfx[strm->postfx_idx++] = *strm->next_in++;
		strm->avail_in--;
		strm->total_in++;

		/*
		 * After 4 trailing bytes the checksum in both formats
		 * is present and can be verified.
		 */
		if ((strm->postfx_idx == 4) &&
		    (strm->format == ZEDC_FORMAT_GZIP)) {

			memcpy(&val32[0], &strm->postfx[0], 4);
			strm->file_crc32 = __le32_to_cpu(val32[0]);
			if (strm->file_crc32 != strm->crc32) {
				zedc->zedc_rc = ZEDC_ERR_CRC32;
				return zedc->zedc_rc;
			}
		}

		if (strm->postfx_idx >= strm->postfx_len) {
			if (strm->format == ZEDC_FORMAT_GZIP) {
				/* remaining eight bytes */
				memcpy(&val32[0], &strm->postfx[0], 8);

				/* val32[0] = CRC32 from GZIP stream */
				/* val32[1] = ISIZE from GZIP stream */
				strm->file_crc32 = __le32_to_cpu(val32[0]);
				strm->file_size  = __le32_to_cpu(val32[1]);
				/* compare trailer info and HW result */
				if (strm->file_crc32 != strm->crc32) {
					zedc->zedc_rc = ZEDC_ERR_CRC32;
					return zedc->zedc_rc;
				}
			} else {
				/* remaining 4 bytes (BE adler32) */
				memcpy(&val32[0], &strm->postfx[0], 4);

				/* val32[0] = ADLER32 from ZLIB stream */
				strm->file_adler32 = __be32_to_cpu(val32[0]);

				/* same value as HW returned ? */
				if (strm->file_adler32 != strm->adler32) {
					pr_err("ADLER32 mismatch: "
					       "%08llx/%08llx\n",
					       (long long)strm->file_adler32,
					       (long long)strm->adler32);

					zedc->zedc_rc = ZEDC_ERR_ADLER32;
					return zedc->zedc_rc;
				}
			}
		}
	}
	if (strm->postfx_idx == strm->postfx_len)
		return ZEDC_OK;		/* removing done */

	return 1;			/* must re-enter  */
}

/**
 * @brief	Figure out if data is left from previous task due to
 *		insufficent output buffer space.
 * @param strm	decompression job context
 */
int zedc_inflate_pending_output(struct zedc_stream_s *strm)
{
	return strm->obytes_in_dict;
}

/**
 * @brief	Enable wrapper code to access internal buffer.
 * 		If data is left from previous task due to insufficent
 *		output buffer space, this data must first be stored
 *		to the new output buffer.
 * @param strm	decompression job context
 */
int zedc_read_pending_output(struct zedc_stream_s *strm,
			uint8_t *buf, unsigned int len)
{
	uint8_t *pdict;
	unsigned int _len = 0;

	if (strm->obytes_in_dict == 0)
		return ZEDC_OK;
	if (strm->dict_len < strm->obytes_in_dict)
		return ZEDC_ERR_DICT_OVERRUN;

	/* obytes at end of dict */
	pdict = strm->wsp->dict[strm->wsp_page] +
		strm->out_dict_offs + strm->dict_len - strm->obytes_in_dict;

	while (len && strm->obytes_in_dict) {
		*buf++ = *pdict++;
		strm->obytes_in_dict--;
		len--;
		_len++;
	}
	return _len;
}

/**
 * @brief	If data is left from previous task due to insufficent
 *		output buffer space, this data must first be stored
 *		to the new output buffer.
 * @param strm	decompression job context
 */
static int inflate_flush_output_buffer(struct zedc_stream_s *strm)
{
	uint8_t *pdict;
	zedc_handle_t zedc = (zedc_handle_t)strm->device;

	if (strm->obytes_in_dict == 0)
		return ZEDC_OK;

	/*
	 * Unstored data was temporarily stored by HW at the end of
	 * dictionary. First restore these bytes if new output buffer
	 * is available.
	 */
	/* FIXME rename 'dict_len' to 'out_dict_used' to match spec */
	if (strm->dict_len < strm->obytes_in_dict) {
		pr_err("invalid 'obytes_in_dict' ZEDC_ERR_DICT_OVERRUN\n");
		zedc->zedc_rc = ZEDC_ERR_DICT_OVERRUN;
		return zedc->zedc_rc;
	}
	/* obytes at end of dict */
	pdict = strm->wsp->dict[strm->wsp_page] +
		strm->out_dict_offs + strm->dict_len - strm->obytes_in_dict;

	while (strm->avail_out && strm->obytes_in_dict) {
		*strm->next_out++ = *pdict++;
		strm->avail_out--;
		strm->total_out++;
		strm->obytes_in_dict--;
	}
	return ZEDC_OK;
}

/**
 * @brief	Post-process for inflate (RFC 1951)
 *		- save necessary states for 'save & restore'
 *		- store remaining data if output buffer is full
 * @param strm	decompression job context
 * @param asv	pointer to ASV area of processed DDCB
 * @return	0 if successful
 */
static void get_inflate_asv(struct zedc_stream_s *strm,
			    struct zedc_asv_infl *asv)
{
	/*
	 * If HW was not able to decompress data due to insufficient
	 * data INP_PROCESSED=0 is returned. Then additional input
	 * data is needed. Some output fields in DDCB don't represent
	 * its real values and must be left in its previous state.
	 *
	 * Invert condition. Condition means hw processed some data.
	 * If hardware was unable to process data, we need more input!
	 */
	if ((asv->inp_processed != 0) || (asv->proc_bits != 0)) {
		strm->out_hdr_bits = __be16_to_cpu(asv->out_hdr_bits);
		strm->hdr_start = __be32_to_cpu(asv->hdr_start);
		strm->out_hdr_start_bits = asv->hdr_start_bits;
	}

	strm->copyblock_len = __be16_to_cpu(asv->copyblock_len);
	strm->crc32 = __be32_to_cpu(asv->out_crc32);
	strm->adler32 = __be32_to_cpu(asv->out_adler32);

	/* prepare dictionary for next call */
	strm->dict_len = __be16_to_cpu(asv->out_dict_used);
	strm->out_dict_offs = asv->out_dict_offs;
	strm->outp_returned = __be32_to_cpu(asv->outp_returned);
	strm->inp_processed = __be32_to_cpu(asv->inp_processed);
	strm->proc_bits = asv->proc_bits;

	/* store values needed for next call */
	strm->obytes_in_dict = __be16_to_cpu(asv->obytes_in_dict);
	strm->infl_stat	= asv->infl_stat;
}
/**
 * @brief	Set ASIV part in Inflate DDCB
 * @param cmd	command params from user
 * @param strm	decompression job context
 * @param asiv	pointer to ASIV part of corresponding DDCB
 * @return	always 0
 */
static void set_inflate_asiv(struct zedc_stream_s *strm,
			     struct zedc_asiv_infl *asiv)
{
	int p;
	uint64_t len;

	/* genwqe_hexdump(stderr, strm->next_in,
	   MIN(strm->avail_in, (unsigned int)0x20)); */

	asiv->in_buff      = __cpu_to_be64((unsigned long)strm->next_in);
	asiv->in_buff_len  = __cpu_to_be32(strm->avail_in);
	asiv->out_buff     = __cpu_to_be64((unsigned long)strm->next_out);
	asiv->out_buff_len = __cpu_to_be32(strm->avail_out);

	/* setup header tree and scratch area */
	asiv->inp_scratch  = __cpu_to_be64((unsigned long)strm->wsp->tree);

	len = strm->hdr_ib + strm->tree_bits +
		strm->pad_bits +
		strm->scratch_ib +
		strm->scratch_bits;

	if (len % 8ULL)
		pr_warn("[%s] in_hdr_scratch_len: 0x%llx not consistent "
			"(0x%x 0x%x 0x%x 0x%x)\n",
			__func__, (long long)len,
			(unsigned int)strm->tree_bits,
			(unsigned int)strm->pad_bits,
			(unsigned int)strm->scratch_ib,
			(unsigned int)strm->scratch_bits);

	strm->in_hdr_scratch_len = (uint32_t)(len / 8ULL);
	strm->pre_scratch_bits = strm->tree_bits + strm->scratch_bits;

	/* This must not exceed ZEDC_TREE_LEN */
	if (strm->in_hdr_scratch_len > ZEDC_TREE_LEN)
		pr_warn("[%s] in_scratch_len=%d exceeds ZEDC_TREE_LEN=%d\n",
			__func__, strm->in_hdr_scratch_len, ZEDC_TREE_LEN);

	asiv->in_scratch_len = __cpu_to_be32(strm->in_hdr_scratch_len);

	asiv->scratch_ib  = strm->scratch_ib;
	asiv->hdr_ib	  = strm->hdr_ib;
	strm->in_hdr_bits = strm->tree_bits;
	asiv->in_hdr_bits    = __cpu_to_be16(strm->in_hdr_bits);

	/* toggle dictionary page */
	p = strm->wsp_page;
	asiv->in_dict  = __cpu_to_be64((unsigned long)strm->wsp->dict[p] +
				       strm->out_dict_offs);
	asiv->out_dict = __cpu_to_be64((unsigned long)strm->wsp->dict[p ^ 1]);
	strm->wsp_page ^= 1;

	asiv->in_dict_len  = __cpu_to_be32(strm->dict_len);
	asiv->out_dict_len = __cpu_to_be32(ZEDC_DICT_LEN);

	asiv->in_crc32	   = __cpu_to_be32(strm->crc32);
	asiv->in_adler32   = __cpu_to_be32(strm->adler32);
}

static int __save_buf_to_file(const char *fname, const uint8_t *buff, int len)
{
	int rc;
	FILE *fp;

	if (buff == NULL)
		return ZEDC_ERR_INVAL;

	if (len == 0)
		return ZEDC_ERR_INVAL;

	pr_err("preserving %s %d bytes ...\n", fname, len);

	fp = fopen(fname, "w+");
	if (!fp) {
		pr_err("Cannot open file %s: %s\n", fname, strerror(errno));
		return ZEDC_ERRNO;
	}
	rc = fwrite(buff, len, 1, fp);
	if (rc != 1) {
		pr_err("Cannot write all data: %d\n", rc);
		fclose(fp);
		return ZEDC_ERRNO;
	}
	fclose(fp);

	return ZEDC_OK;
}

int zedc_inflateSaveBuffers(zedc_streamp strm, const char *prefix)
{
	int rc;
	struct zedc_asiv_infl *asiv;
	struct ddcb_cmd *cmd;
	char fname[_POSIX_PATH_MAX];

	if (!strm)
		return ZEDC_STREAM_ERROR;

	cmd = &strm->cmd;
	asiv = (struct zedc_asiv_infl *)&cmd->asiv;

	/* Buffers to dump */
	snprintf(fname, sizeof(fname) - 1, "%s_in_buff.bin", prefix);
	rc = __save_buf_to_file(fname, (void *)(unsigned long)
				__be64_to_cpu(asiv->in_buff),
				__be32_to_cpu(asiv->in_buff_len));
	if (rc != ZEDC_OK)
		return rc;

	snprintf(fname, sizeof(fname) - 1, "%s_out_buf.bin", prefix);
	rc = __save_buf_to_file(fname, (void *)(unsigned long)
				__be64_to_cpu(asiv->out_buff),
				__be32_to_cpu(asiv->out_buff_len));
	if (rc != ZEDC_OK)
		return rc;

	snprintf(fname, sizeof(fname) - 1, "%s_in_dict.bin", prefix);
	rc = __save_buf_to_file(fname, (void *)(unsigned long)
				__be64_to_cpu(asiv->in_dict),
				__be32_to_cpu(asiv->in_dict_len));
	if (rc != ZEDC_OK)
		return rc;

	snprintf(fname, sizeof(fname) - 1, "%s_out_dict.bin", prefix);
	rc = __save_buf_to_file(fname, (void *)(unsigned long)
				__be64_to_cpu(asiv->out_dict),
				__be32_to_cpu(asiv->out_dict_len));
	if (rc != ZEDC_OK)
		return rc;

	snprintf(fname, sizeof(fname) - 1, "%s_inp_scratch.bin", prefix);
	rc = __save_buf_to_file(fname, (void *)(unsigned long)
				__be64_to_cpu(asiv->inp_scratch),
				__be32_to_cpu(asiv->in_scratch_len));
	if (rc != ZEDC_OK)
		return rc;

	return ZEDC_OK;
}

/**
 * @brief		main function for decompression
 * @param strm		Common zedc parameter set.
 * @param flush         Flush mode.
 * @return              ZEDC_OK, ZEDC_STREAM_END, ZEDC_STREAM_ERROR,
 *                      ZEDC_MEM_ERROR.
 *
 * Review error conditions. E.g. some functions do not have a return
 * code. Is that ok or do we need to add it?
 */
int zedc_inflate(zedc_streamp strm, int flush)
{
	int rc, zrc;
	uint32_t len;
	struct zedc_asiv_infl *asiv;
	struct zedc_asv_infl *asv;
	zedc_handle_t zedc;
	struct ddcb_cmd *cmd;

	unsigned int i, tries = 1;
	uint64_t out_dict = 0x0;
	uint32_t out_dict_len = 0x0;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	zedc = (zedc_handle_t)strm->device;
	if (!zedc)
		return ZEDC_STREAM_ERROR;

	cmd = &strm->cmd;
	ddcb_cmd_init(cmd);	/* clear completely */

	/*
	 * A limitation is needed to prevent internal overflow input
	 * buffer must be smaller than 4GiB - 1KiB since additional
	 * data can be added for S&R purposes.
	 */
	if (strm->avail_in > ZEDC_INFL_AVAIL_IN_MAX) {
		pr_err("input buffer too large\n");
		return ZEDC_MEM_ERROR;
	}

	strm->flush = flush;
	strm->inp_data_offs = 0;

	/*
	 * Pre-processing, restore data from previous task and copy
	 * obytes to output buffer.
	 */
	rc = inflate_flush_output_buffer(strm);
	if (rc) {
		pr_err("inflate failed rc=%d\n", rc);
		return ZEDC_STREAM_ERROR;
	}

	/* Did we reach End-Of-Final-Block (or seen it before) ? */
	if (strm->infl_stat & INFL_STAT_FINAL_EOB)
		strm->eob_seen = 1; /* final EOB seen */

	if (strm->eob_seen) {
		/* remove ZLIB/GZIP trailer */
		rc = inflate_format_rem_trailer(strm);
		if (rc < 0)		/* CRC or ADLER check failed */
			return ZEDC_DATA_ERROR;
		if (rc == 1)
			return ZEDC_OK;	/* need more trailer data */
		if (strm->obytes_in_dict == 0)
			return ZEDC_STREAM_END;
		return ZEDC_OK;	/* must re-enter */
	}

	/* Output buffer now full ? */
	if (strm->avail_out == 0)
		return ZEDC_OK;	/* must re-enter */

	/* Remove potential ZLIB/GZIP prefix */
	if (HEADER_DONE != strm->header_state) {
		rc = inflate_format_rem_header(strm, flush);
		if (ZEDC_OK != rc)
			return rc;
	}

	if (strm->data_type & 0x80)
		return ZEDC_OK; /* must re-enter */

	/* Exit if no input data present */
	if ((strm->avail_in == 0) && (strm->scratch_bits == 0))
		goto chk_ret;		/* check stat and return END or OK */

	/* Prepare Inflate DDCB */
	cmd->cmd	 = ZEDC_CMD_INFLATE;
	cmd->acfunc	 = DDCB_ACFUNC_APP;
	cmd->asiv_length = 0x70 - 0x18;	/* parts to be crc protected */
	cmd->asv_length	 = 0xc0 - 0x80;
	cmd->ats = 0;
	cmd->cmdopts = 0x0;
	asiv = (struct zedc_asiv_infl *)&cmd->asiv;
	asv = (struct zedc_asv_infl *)&cmd->asv;

	/* input buffer: Use always SGL here */
	if ((strm->dma_type[ZEDC_IN] & DDCB_DMA_TYPE_MASK) ==
	    DDCB_DMA_TYPE_FLAT)
		cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_infl, in_buff,
					  ATS_TYPE_FLAT_RD);
	else    cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_infl, in_buff,
					  ATS_TYPE_SGL_RD);

	/* output buffer */
	if ((strm->dma_type[ZEDC_OUT] & DDCB_DMA_TYPE_MASK) ==
		DDCB_DMA_TYPE_FLAT)
		cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_infl, out_buff,
					  ATS_TYPE_FLAT_RDWR);
	else    cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_infl, out_buff,
					  ATS_TYPE_SGL_RDWR);

	/* workspace */
	if ((strm->dma_type[ZEDC_WS] & DDCB_DMA_TYPE_MASK) ==
	    DDCB_DMA_TYPE_FLAT) {
		cmd->ats |= (ATS_SET_FLAGS(struct zedc_asiv_infl, in_dict,
					   ATS_TYPE_FLAT_RD) |
			     ATS_SET_FLAGS(struct zedc_asiv_infl, out_dict,
					   ATS_TYPE_FLAT_RDWR) |
			     ATS_SET_FLAGS(struct zedc_asiv_infl, inp_scratch,
					   ATS_TYPE_FLAT_RDWR));
	} else {
		cmd->ats |= (ATS_SET_FLAGS(struct zedc_asiv_infl, in_dict,
					   ATS_TYPE_SGL_RD) |
			     ATS_SET_FLAGS(struct zedc_asiv_infl, out_dict,
					   ATS_TYPE_SGL_RDWR) |
			     ATS_SET_FLAGS(struct zedc_asiv_infl, inp_scratch,
					   ATS_TYPE_SGL_RDWR));
	}

	if (strm->flags & ZEDC_FLG_CROSS_CHECK)
		cmd->cmdopts |= DDCB_OPT_INFL_RAS_CHECK;	/* + RAS */

	/* Setup ASIV part (in big endian byteorder) */
	set_inflate_asiv(strm, (struct zedc_asiv_infl *)&cmd->asiv);

	/*
	 * Optimization attempt: If we are called with Z_FINISH, and we
	 * assume that the data will fit into the provided output
	 * buffer, we try to run the hardware without dictionary save
	 * function. If we do not see INFL_STAT_FINAL_EOB, we need to
	 * restart with dictionary save option.
	 *
	 * The desire is to keep small transfers efficient. It will
	 * not have significant effect if we deal with huge data
	 * streams.
	 */
	cmd->cmdopts |= DDCB_OPT_INFL_SAVE_DICT;	/* SAVE_DICT */
	tries = 1;

	if ((strm->flags & ZEDC_FLG_SKIP_LAST_DICT) &&
	    (flush == ZEDC_FINISH) && (strm->avail_out > strm->avail_in * 2)) {
		//static int count = 0;

		out_dict = asiv->out_dict;
		out_dict_len = asiv->out_dict_len;

		cmd->cmdopts &= ~DDCB_OPT_INFL_SAVE_DICT;
		asiv->out_dict = 0x0;
		asiv->out_dict_len = 0x0;
		tries = 2;

		//if (count++ < 2)
		//	fprintf(stderr, "[%s] Try to optimize dict transfer: "
		//		"avail_in=%d avail_out=%d\n",
		//		__func__, strm->avail_in, strm->avail_out);
	}

	for (i = 0; i < tries; i++) {
		/* Execute inflate in HW */
		zedc_asiv_infl_print(strm);
		rc = zedc_execute_request(zedc, cmd);
		zedc_asv_infl_print(strm);

		strm->retc = cmd->retc;
		strm->attn = cmd->attn;
		strm->progress = cmd->progress;

		/*
		 * Dynamic/Fixed block decode: Distance is too far
		 * back in the dictionary: (RETC=104 ATTN=801a
		 * PROGR=0).
		 */
		if ((rc == DDCB_ERR_EXEC_DDCB) &&
		    (cmd->retc == DDCB_RETC_FAULT) && (cmd->attn == 0x801A)) {
			strm->adler32 = strm->dict_adler32;
			pr_err("inflate ZEDC_NEED_DICT\n");
			return ZEDC_NEED_DICT;
		}

		/*
		 * GenWQE treats success or failure a little
		 * differently than the CAPI implementation. CAPI
		 * flags success, if the DDCB was treated by hardware
		 * at all. This includes cases where RETC is not
		 * 0x102. For GenWQE we flag success only if there is
		 * a RETC of 0x102, this is done in the Linux driver.
		 *
		 * Doing this wrong, can lead to problems in the code
		 * below, which processes DDCB result data, which
		 * might not be valid, e.g. memmove() with wrong size.
		 */
		if ((rc < 0) || (cmd->retc != DDCB_RETC_COMPLETE)) {
			struct ddcb_cmd *cmd = &strm->cmd;

			pr_err("inflate failed rc=%d\n"
			       "DDCB returned (RETC=%03x ATTN=%04x PROGR=%x) "
			       "%s\n", rc, cmd->retc, cmd->attn, cmd->progress,
			       cmd->retc == 0x102 ? "" : "ERR");
			return ZEDC_STREAM_ERROR;
		}

		/* Wonderful, we have all the data we need, stop processing */
		if (asv->infl_stat & INFL_STAT_FINAL_EOB)
			break;

		/* What a pity, we guessed wrong and need to
		   repeat. We did not see the last byte in the last
		   block yet! */
		if ((strm->flags & ZEDC_FLG_SKIP_LAST_DICT) &&
		    (flush == ZEDC_FINISH)) {
			cmd->cmdopts |= DDCB_OPT_INFL_SAVE_DICT;
			asiv->out_dict = out_dict;
			asiv->out_dict_len = out_dict_len;
			pr_warn("[%s] What a pity, we guessed wrong "
				"and need to repeat\n", __func__);
		}
	}

	get_inflate_asv(strm, asv);
	rc = post_scratch_upd(strm);
	if (rc < 0) {
		pr_err("inflate scratch update failed rc=%d\n", rc);
		return ZEDC_STREAM_ERROR;
	}

	/* Sanity check: Hardware bug Get length of output data. Can
	   also be 0! */
	if (strm->outp_returned > strm->avail_out) {
		pr_err("OUTP_RETURNED too large (0x%x)\n",
					strm->outp_returned);
		return ZEDC_STREAM_ERROR;
	}

	strm->next_out  += strm->outp_returned;
	strm->avail_out -= strm->outp_returned;
	strm->total_out += strm->outp_returned;

	/* Sanity check: Hardware claims to have processed more input
	   data than offered. */
	len = strm->inp_data_offs;  /* Just input bytes from next_in,
				       not repeated tree, hdr, scratch bits */
	/* fprintf(stderr, "LEN(%s): len=%d\n", __func__, len); */

	if (len > strm->avail_in) {
		pr_err("consumed=%u/avail_in=%u\n", len, strm->avail_in);
		goto abort;
	}

	strm->next_in  += len;
	strm->avail_in -= len;
	strm->total_in += len;

	zrc = ZEDC_OK;		/* preset 0 */

	/* Did we reach End-Of-Final-Block (or seen it before) ? */
	if (strm->infl_stat & INFL_STAT_FINAL_EOB)
		strm->eob_seen = 1; /* final EOB seen */

	if (strm->eob_seen) {
		/* remove ZLIB/GZIP trailer */
		rc = inflate_format_rem_trailer(strm);
		if (rc < 0)		/* CRC or ADLER check failed */
			return ZEDC_DATA_ERROR;
		if (rc == 1)
			return ZEDC_OK;	/* need more trailer data */
		if (strm->obytes_in_dict == 0)
			return ZEDC_STREAM_END;
		return ZEDC_OK;	/* must re-enter */
	}

	/* If FEOB is in the middle of input and output is not
	   excausted yet, it might be just ok. */
	if (strm->avail_in && strm->avail_out) {
		pr_warn("[%s] input not completely processed "
			"(avail_in=%d avail_out=%d zrc=%d)\n",
			__func__, strm->avail_in, strm->avail_out, zrc);
	}

	return zrc;

 chk_ret:
	/* End of final block and no dict data to copy */
	if ((strm->infl_stat & INFL_STAT_FINAL_EOB) &&
	    (strm->obytes_in_dict == 0))
		return ZEDC_STREAM_END;	/* done */

	return ZEDC_OK;			/* must re-enter */

 abort:
	return ZEDC_STREAM_ERROR;
}

/**
 * @brief		Initialize inflate state
 * @param strm		Common zedc parameter set.
 */
static void __inflateInit_state(zedc_streamp strm)
{
	strm->total_in = strm->total_out = 0;

	/* initialize workspace */
	strm->wsp_page = 0; /* reset toggle input / output area */
	strm->dict_len = 0; /* ensure empty dictionary */
	strm->obytes_in_dict = 0;
	strm->out_dict_offs = 0;

	/* initialize GZIP/ZLIB returns */
	strm->file_crc32 = 0;
	strm->file_adler32 = 0;
	strm->dict_adler32 = 0;

	/* initialize inflate */
	strm->total_in = 0;
	strm->total_out = 0;
	strm->crc32 = 0;
	strm->adler32 = 1;
	strm->eob_seen = 0;
	strm->havedict = 0;

	strm->in_hdr_scratch_len = 0;
	strm->in_hdr_bits = 0;
	strm->hdr_ib = 0;
	strm->scratch_ib = 0;
	strm->scratch_bits = 0;	/* HW.... was missing */

	strm->inp_processed = 0;
	strm->outp_returned = 0;
	strm->proc_bits = 0;
	strm->infl_stat = 0;
	strm->hdr_start = 0;
	strm->out_hdr_bits = 0;
	strm->out_hdr_start_bits = 0;
	strm->copyblock_len = 0;

	strm->tree_bits = 0;
	strm->pad_bits = 0;
	strm->pre_scratch_bits = 0;
	strm->inp_data_offs = 0;
	strm->in_data_used = 0;

	/* Reset prefix and postfix buffers */
	strm->prefx_len = 0;
	strm->prefx_idx = 0;
	memset(strm->prefx, 0, sizeof(strm->prefx));
	strm->xlen = 0;
	strm->header_state = HEADER_START;

	strm->postfx_len = 0;
	strm->postfx_idx = 0;
	memset(strm->postfx, 0, sizeof(strm->postfx));

	ddcb_cmd_init(&strm->cmd); /* clear completely */

}

/**
 * @brief	inflate initialization
 * @param	strm	common zedc parameter set
 * @return	0 if success
 */
int zedc_inflateInit2(zedc_streamp strm, int windowBits)
{
	int rc;
	zedc_handle_t zedc;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	zedc = (zedc_handle_t)strm->device;
	if (!zedc)
		return ZEDC_STREAM_ERROR;

	if (!is_zedc(zedc))
		return ZEDC_ERR_ILLEGAL_APPID;

	rc = zedc_alloc_workspace(strm);
	if (rc != ZEDC_OK)
		return rc;

	/* initialize inflate */
	strm->windowBits = windowBits;
	__inflateInit_state(strm);

	/* initialize Save & Restore */
	rc = zedc_format_init(strm);
	if (rc != ZEDC_OK) { /* presets for DEFLATE, GZIP, ZLIB */
		zedc_free_workspace(strm);
		return rc;
	}

	return ZEDC_OK;
}

/**
 * @brief		Reset inflate stream. Do not deallocate memory.
 * @param strm		Common zedc parameter set
 * @param dictionary    Alternate dictionary data to be used.
 * @return              ZEDC_OK on success, else failure.
 */
int zedc_inflateSetDictionary(zedc_streamp strm,
			      const uint8_t *dictionary,
			      unsigned int dictLength)
{
	uint32_t a32;

	if (strm == NULL)
		return ZEDC_STREAM_ERROR;

	if (dictLength > ZEDC_DICT_LEN)
		return ZEDC_STREAM_ERROR;

	if (strm->format == ZEDC_FORMAT_ZLIB) {
		a32 = __adler32(1, dictionary, dictLength);
		if (a32 != strm->dict_adler32)
			return ZEDC_DATA_ERROR;
	}

	memcpy(&strm->wsp->dict[0], dictionary, dictLength);
	strm->dict_len = dictLength;
	strm->havedict = 1;	/* just need this once */
	strm->adler32 = 1;	/* back to default again */

	return ZEDC_OK;
}

/**
 * @brief		Get current input dictionary
 * @param strm		Stream
 * @param dictionary    dictionary buffer, 32KiB, used if not NULL
 * @param dictLength    length of dictionary, returned if not NULL
 * @return              ZEDC_OK on success, else failure.
 */
int zedc_inflateGetDictionary(zedc_streamp strm,
			      uint8_t *dictionary,
			      unsigned int *dictLength)
{
	unsigned int p;
	uint8_t *in_dict;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	if (dictLength)
		*dictLength = strm->dict_len;

	if (dictionary == NULL)
		return ZEDC_OK;

	p = strm->wsp_page;
	in_dict = strm->wsp->dict[p] + strm->out_dict_offs;

	memcpy(dictionary, in_dict, strm->dict_len);
	return ZEDC_OK;

}

/**
 * @brief		Reset inflate stream. Do not deallocate memory.
 * @param strm		Common zedc parameter set.
 * @return              ZEDC_OK on success, else failure.
 */
int zedc_inflateReset(zedc_streamp strm)
{
	int rc;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	__inflateInit_state(strm);

	rc = zedc_format_init(strm);
	if (rc != ZEDC_OK)
		return rc;

	return ZEDC_OK;
}

int zedc_inflateReset2(zedc_streamp strm, int windowBits)
{
	int rc;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	__inflateInit_state(strm);
	strm->windowBits = windowBits;

	rc = zedc_format_init(strm);
	if (rc != ZEDC_OK)
		return rc;

	return ZEDC_OK;
}

/**
 * @brief		End inflate (de-compress).
 * @param strm		Common zedc parameter set.
 * @return              ZEDC_OK on success, else failure.
 */
int zedc_inflateEnd(zedc_streamp strm)
{
	zedc_handle_t zedc;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	zedc = (zedc_handle_t)strm->device;
	if (!zedc)
		return ZEDC_STREAM_ERROR;

	zedc_free_workspace(strm);
	return ZEDC_OK;
}

int zedc_inflateGetHeader(zedc_streamp strm, gzedc_headerp head)
{
	strm->gzip_head = head;
	head->done = 0;
	return ZEDC_OK;
}
