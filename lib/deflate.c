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

/**
 * @brief This part of the libzedc library is responsible to perform
 * compression (deflate) of the compressed data. The library supports
 * the data formats described in RFC1950, RFC1951, and RFC1952.
 *
 * IBM Accelerator Family 'GenWQE'/zEDC
 */

/****************************************************************************
 * Compression (Deflate)
 ***************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <asm/byteorder.h>
#include <assert.h>

#include <libcard.h>
#include <libzHW.h>
#include "hw_defs.h"

static inline int output_data_avail(struct zedc_stream_s *strm)
{
	return strm->avail_out;
}

static inline int input_data_avail(struct zedc_stream_s *strm)
{
	return strm->avail_in != 0;
}

static inline int push_output_byte(struct zedc_stream_s *strm, uint8_t data)
{
	if (strm->avail_out == 0)
		return 0;	/* no byte written */

	*strm->next_out = data;
	strm->total_out++;
	strm->next_out++;
	strm->avail_out--;
	return 1;		/* one byte written */
}

/**
 * @brief		Prepare and insert format specific header bytes
 *			(RFC1950 / RFC1952)
 * @param strm		userspace job context
 */
static int deflate_add_header(struct zedc_stream_s *strm)
{
	struct zedc_fifo *f = &strm->out_fifo;

	switch (strm->format) {
	case ZEDC_FORMAT_DEFL:
		return 0;		/* no extra header for DEFLATE */

	case ZEDC_FORMAT_GZIP: {
		uint8_t flg = 0x00, os = 0xff;
		uint8_t xfl = 0x04;	/* XFL 4: fastest algorithm */
		unsigned int i, name_len = 0, c_len = 0, e_len = 0;
		uint32_t mt = (uint32_t)time(NULL);

		/* Note: ZEDC_FIFO_SIZE = 256 */
		fifo_push(f, 0x1f);	/* ID1 */
		fifo_push(f, 0x8b);	/* ID2 */
		fifo_push(f, 0x08);	/* CM  */
		struct gzedc_header_s *gz_h = strm->gzip_head;
		if (gz_h) {
			if (gz_h->name) name_len = strlen(gz_h->name);
			if (name_len) flg |= FNAME;
			if (gz_h->comment) c_len = strlen(gz_h->comment);
			if (c_len) flg |= FCOMMENT;
			if (gz_h->extra) {
				e_len = gz_h->extra_len;
				flg |= FEXTRA;
			}
			os = gz_h->os;
			mt = gz_h->time;
			//xfl = gz_h->xflags;
			/* Check if we are not going to overflow the FIFO */
			if ((name_len + c_len + e_len) > 240)
				return 1;
			if (gz_h->xflags & 0x01)
				flg |= FTEXT;
			if (gz_h->xflags & 0x02)
				flg |= FHCRC;
		}

		fifo_push(f, flg);	/* FLG */
		fifo_push32(f, __cpu_to_le32(mt)); /* MT */
		fifo_push(f, xfl);
		fifo_push(f, os);	/* OS  */
		if (flg & FEXTRA) {
			fifo_push(f, e_len & 0xff);
			fifo_push(f, (e_len >> 8) & 0xff);
			for (i = 0; i < e_len; i++)
				fifo_push(f, gz_h->extra[i]);
		}

		if (flg & FNAME)
			for (i = 0; i <= name_len; i++)
				fifo_push(f, strm->gzip_head->name[i]);
		if (flg & FCOMMENT)
			for (i = 0; i <= c_len; i++)
				fifo_push(f, strm->gzip_head->comment[i]);
		if (flg & FHCRC) {
			if (gz_h) {
				/* insert some dummy CRC for now , add
				   code later */
				fifo_push(f, 0xde);
				fifo_push(f, 0xef);
			}
		}
		break;
	}
	case ZEDC_FORMAT_ZLIB: {
		/*
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
		if (strm->havedict) {
			fifo_push(f, 0x78);	/* CMF */
			fifo_push(f, 0xbb);	/* FLG with FDICT set */
			fifo_push32(f, __cpu_to_be32(strm->dict_adler32));
		} else {
			fifo_push(f, 0x78);	/* CMF */
			fifo_push(f, 0x9c);	/* FLG */
		}
		break;
	}
	}
	strm->header_added = 1;
	return 0;
}

/**
 * Write to the output stream.
 */
static void deflate_write_out_fifo(struct zedc_stream_s *strm)
{
	uint8_t data;
	struct zedc_fifo *f = &strm->out_fifo;

	while (output_data_avail(strm) && fifo_pop(f, &data) == 1) {
		push_output_byte(strm, data);
	}
	return;
}

static void __deflateInit_state(zedc_streamp strm)
{
	/* zedc_handle_t zedc = (zedc_handle_t)strm->device; */

	fifo_init(&strm->out_fifo);
	fifo_init(&strm->in_fifo);
	strm->total_in = strm->total_out = 0;

	/* initialize workspace */
	strm->wsp_page = 0;	/* reset toggle input / output area */
	strm->dict_len = 0;	/* ensure empty dictionary */
	strm->obytes_in_dict = 0;
	strm->out_dict_offs = 0;

	/* initialize Save & Restore */
	strm->obyte = HDR_BTYPE_FIXED;		/* deflate header */
	strm->onumbits = 3;			/* deflate header = 3 bits */

	strm->crc32 = 0;
	strm->adler32 = 1;
	strm->dict_adler32 = 0;

	strm->header_added = 0;			/* status flags */
	strm->eob_added = 0;
	strm->trailer_added = 0;
	strm->havedict = 0;

	strm->in_hdr_scratch_len = 0;
	strm->in_hdr_bits = 0;
	strm->hdr_ib = 0;
	strm->scratch_ib = 0;

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
	strm->inp_data_offs = 0;
	strm->in_data_used = 0;
}

/**
 * @brief		initialize subsequent zedc_deflate() calls
 * @param strm		common zedc parameter set
 * @param level		compression level
 */
int zedc_deflateInit2(zedc_streamp strm,
		      int level,
		      int method,
		      int windowBits,
		      int memLevel,
		      int strategy)
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

	strm->windowBits   = windowBits;
	strm->level	   = level;
	strm->method	   = method;
	strm->memLevel	   = memLevel;
	strm->strategy	   = strategy;
	__deflateInit_state(strm);

	rc = zedc_format_init(strm);
	if (rc != ZEDC_OK) {	/* presets for DEFLATE, GZIP, ZLIB */
		zedc_free_workspace(strm);
		return rc;
	}
	return ZEDC_OK;
}

int zedc_deflateSetDictionary(zedc_streamp strm,
			      const uint8_t *dictionary,
			      unsigned int dictLength)
{
	if (!strm)
		return ZEDC_STREAM_ERROR;

	/* We cannot set the dictionary after we have already written
	   the zlib header! */
	if (strm->header_added == 1)
		return ZEDC_STREAM_ERROR;

	if (dictLength > ZEDC_DICT_LEN)
		return ZEDC_STREAM_ERROR;

	memcpy(&strm->wsp->dict[0], dictionary, dictLength);
	strm->dict_len = dictLength;
	strm->dict_adler32 = __adler32(1, dictionary, dictLength);
	strm->havedict = 1;

	return ZEDC_OK;
}

int zedc_deflateCopy(zedc_streamp dest, zedc_streamp source)
{
	int rc;

	memcpy(dest, source, sizeof(*dest));
	rc = zedc_alloc_workspace(dest);
	if (rc != ZEDC_OK)
		return rc;

	/* Try only to copy what is really needed ... */
	unsigned int p = dest->wsp_page;
	memcpy(dest->wsp->tree, source->wsp->tree, sizeof(*dest->wsp->tree));
	memcpy(dest->wsp->dict[p], source->wsp->dict[p],
	       sizeof(dest->wsp->dict[p]));

	return ZEDC_OK;
}

int zedc_deflateReset(zedc_streamp strm)
{
	int rc;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	__deflateInit_state(strm);

	rc = zedc_format_init(strm);
	if (rc != ZEDC_OK)	/* presets for DEFLATE, GZIP, ZLIB */
		return rc;

	return ZEDC_OK;
}

/**
 * @brief	add trailer for gzip coding (RFC1952) or
 *		add trailer for zlib coding (RFC1950)
 *		depending on 'windowBits'
 * @param strm	userspace job context
 */
static int deflate_add_trailer(struct zedc_stream_s *strm)
{
	struct zedc_fifo *f = &strm->out_fifo;

	if (!strm->eob_added)
		return 0;		/* EOB must be written first */

	if (strm->trailer_added)
		return 0;		/* Don't add it multiple times */

	switch (strm->format) {
	case ZEDC_FORMAT_DEFL:
		break;			/* no extra trailer for DEFLATE */

	case ZEDC_FORMAT_GZIP:
		/* prepare GZIP trailer */
		fifo_push32(f, __cpu_to_le32(strm->crc32));
		fifo_push32(f, __cpu_to_le32(strm->total_in));
		break;

	case ZEDC_FORMAT_ZLIB:
		/* prepare ZLIB trailer */
		fifo_push32(f, __cpu_to_be32(strm->adler32));
		break;
	}
	strm->trailer_added = 1;
	return 1;
}

/* bitmask to isolate valid bits from deflate */
static const uint8_t bmsk[8] = { 0xff, 0x01, 0x03, 0x07,
				 0x0f, 0x1f, 0x3f, 0x7f };

/**
 * We are at the end of compression (no input data available) An extra
 * zero byte must be appended as end-of-block marker if this was the
 * last block in the compressed stream.
 *
 * (RFC1951 End-Of-Block Marker = %000_0000).
 *
 * To sync up the stream at the end we like to write this pattern:
 *   [F_EOB, F_HDR(BFINAL), F_EOB] = 0000_000.0_11.00_0000_0.<BB>
 *                                 = { 0x00, 0x03, 0x00 }
 *                                 = 7 + 3 + 7 = 17 bits
 */
static int deflate_write_eob(struct zedc_stream_s *strm)
{
	struct zedc_fifo *f = &strm->out_fifo;
	/* const uint8_t eob_sync[3] = { 0x80, 0x01, 0x00 }; urg, bit-order */

	/* Avoid adding EOBs multiple times */
	if (strm->eob_added == 1)
		return 0;

	/* If we have remaining single bits, we cannot add the EOB yet */
	if (strm->onumbits >= 8)
		return 0;

	if (strm->onumbits == 0) {
		fifo_push(f, 0x80); /*0b10000000 */
		fifo_push(f, 0x01); /*0b00000001 */
		fifo_push(f, 0x00); /*0b00000000 */
	} else {
		fifo_push(f, strm->obyte & bmsk[strm->onumbits]);
		fifo_push(f, 0x03 << (strm->onumbits - 1)); /*0b00000011 ... */
		fifo_push(f, 0x00); /*0b00000000 */
	}

	strm->onumbits = 0;
	strm->eob_added = 1;
	return 1;			/* EOB stored to FIFO */
}

/*
 * Add sync flush for rfc1951:
 *	7 bits for End of Block
 *	1 bit for BFINAL
 *	2 bits for End of Fix Hufman block
 *	16 Bits with 0....0 for Length
 *	16 Bits with 1....1 for (not)Length
 */
static void deflate_sync_flush(struct zedc_stream_s *strm)
{
	struct zedc_fifo *f = &strm->out_fifo;
	uint8_t	data = 0;

	if (strm->onumbits == 0) {
		fifo_push(f, 0);
		fifo_push(f, 0);
	} else {
		data = strm->obyte & bmsk[strm->onumbits];
		fifo_push(f, data);
		fifo_push(f,0);
		if (strm->onumbits > 6)	/* if data is more than 6 bits */
			fifo_push(f, 0); /* add 1 or 2 more in the next byte */
		strm->onumbits = 0;
	}

	fifo_push(f, 0);			/* Add Len */
	fifo_push(f, 0);			/* Add Len */
	fifo_push(f, 0xff);			/* Add n_Len */
	fifo_push(f, 0xff);			/* Add n_Len */
	strm->obyte = HDR_BTYPE_FIXED;		/* deflate header */
	strm->onumbits = 3;			/* deflate header = 3 bits */
	return;
}

/**
 * @brief	Post-process for deflate (RFC 1951)
 *		- store remaining data if output buffer is full
 *		- mask valid bits of last byte
 * @param strm	userspace job context
 * @param asv	pointer to ASV area of processed DDCB
 * @return	0 if successful
 *		< 0 if failed
 */
static int deflate_process_results(struct zedc_stream_s *strm,
				   struct zedc_asv_defl *asv)
{
	unsigned int len, i;
	zedc_handle_t zedc = (zedc_handle_t)strm->device;
	struct zedc_fifo *f = &strm->out_fifo;

	len = strm->inp_processed = __be32_to_cpu(asv->inp_processed);
	strm->outp_returned = __be32_to_cpu(asv->outp_returned);

	/* sum of uncompressed bytes used for RFC 1952) */
	if (len > strm->avail_in) {
		pr_err("inp_processed=%d avail_in=%d invalid: "
		       "  retc=%x attn=%x progress=%x\n",
		       strm->inp_processed, strm->avail_in,
		       strm->retc, strm->attn, strm->progress);

		/* Now become really verbose ... Let's see what happens. */
		zedc_asiv_defl_print(strm, 1);
		zedc_asv_defl_print(strm, 1);

		zedc->zedc_rc = ZEDC_ERR_RETLEN;
		return zedc->zedc_rc;
	}
	strm->avail_in -= len;
	strm->next_in  += len;
	strm->total_in += len;

	/* get length of output data */
	len = strm->outp_returned;

	/* Sanity check */
	if ((len == 0) || (len > strm->avail_out)) {
		pr_err("outp_returned=%u inp_processed=%d "
		       "avail_in=%d avail_out=%d invalid: "
		       "  retc=%x attn=%x progress=%x\n",
		       strm->outp_returned, strm->inp_processed,
		       strm->avail_in, strm->avail_out,
		       strm->retc, strm->attn, strm->progress);

		/* Now become really verbose ... Let's see what happens. */
		zedc_asiv_defl_print(strm, 1);
		zedc_asv_defl_print(strm, 1);

		zedc->zedc_rc = ZEDC_ERR_RETLEN;
		return zedc->zedc_rc;
	}

	/* Check if onumbits are valid for new or for old hardware */
	if (dyn_huffman_supported(zedc)) {
		if (asv->onumbits > (ZEDC_ONUMBYTES_v1 +
				     ZEDC_ONUMBYTES_EXTRA) * 8) {
			pr_err("onumbits %d too large (O)\n", asv->onumbits);
			zedc->zedc_rc = ZEDC_ERR_RETOBITS;
			return zedc->zedc_rc;
		}
	} else {
		if (asv->onumbits > ZEDC_ONUMBYTES_v0 * 8) {
			pr_err("onumbits %d too large (N)\n", asv->onumbits);
			zedc->zedc_rc = ZEDC_ERR_RETOBITS;
			return zedc->zedc_rc;
		}
	}

	strm->next_out  += len;
	strm->avail_out -= len;
	strm->total_out += len;

	/*
	 * Store onumbits for next DDCB.
	 *
	 * if ONUMBITS == 0:
	 * - Output buffer contains all bits on a byte boundary.
	 * if ONUMBITS == 1...7:
	 * - there are partial bits which must be appended in the
	 *   output buffer
	 * if ONUMBITS > 7:
	 * - there are bytes provided in OBYTES/OBYTES_EXTRA which
	 *   could not be stored due to a completely filled output
	 *   buffer. This must be done in a subsequent cycle after
	 *   emptied the output buffer.
	 */

	/* Sanity check: Hardware put not all required bits into output buf */
	if ((strm->avail_out != 0) && (asv->onumbits > 7)) {
		pr_err("** err: unstored data bytes **\n");
		zedc->zedc_rc = ZEDC_ERR_RETOBITS;
		return zedc->zedc_rc;
	}

	/* Push remaining bytes into output FIFO */
	if (dyn_huffman_supported(zedc)) {
		/*
		 * For the new format we can get more bytes than
		 * originally expected. In the v1 buffer there is one
		 * more byter and there is one byte in the middle of
		 * the DDCB data, which has a different meaning
		 * (out_dict_offs). We need to jump over it.
		 */
		for (i = 0, strm->onumbits = asv->onumbits;
		     (strm->onumbits > 7) && (i < ZEDC_ONUMBYTES_v1);
		     i++, strm->onumbits -= 8) {
			fifo_push(f, asv->obits[i]);
		}
		if ((strm->onumbits) && (i < ZEDC_ONUMBYTES_v1)) {
			strm->obyte = asv->obits[i];
			return 0;
		}

		for (i = 0;
		     (strm->onumbits > 7) && (i < ZEDC_ONUMBYTES_EXTRA);
		     i++, strm->onumbits -= 8) {
			fifo_push(f, asv->obits_extra[i]);
		}
		if ((strm->onumbits) && (i < ZEDC_ONUMBYTES_EXTRA)) {
			strm->obyte = asv->obits_extra[i];
			return 0;
		}
	} else {
		for (i = 0, strm->onumbits = asv->onumbits;
		     (strm->onumbits > 7) && (i < ZEDC_ONUMBYTES_v0);
		     i++, strm->onumbits -= 8) {
			fifo_push(f, asv->obits[i]);
		}
		/* copy the incomplete remaining byte */
		if (strm->onumbits)
			strm->obyte = asv->obits[i];
	}
	return 0;
}

/**
 * @brief	do deflate (compress)
 * @param strm	common zedc parameter set
 * @param flush	flag if pending output data should be written
 */
int zedc_deflate(zedc_streamp strm, int flush)
{
	int rc, p;
	struct zedc_asiv_defl *asiv;
	struct zedc_asv_defl *asv;
	zedc_handle_t zedc;
	struct ddcb_cmd *cmd;
	struct zedc_fifo *f;

	unsigned int i, tries = 1;
	uint64_t out_dict = 0x0;
	uint32_t out_dict_len = 0x0;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	f = &strm->out_fifo;
	zedc = (zedc_handle_t)strm->device;
	if (!zedc)
		return ZEDC_STREAM_ERROR;

	strm->flush = flush;
	cmd = &strm->cmd;
	ddcb_cmd_init(cmd);

	/* Add ZLIB/GZIP prefix if needed */
	if (0 == strm->header_added) {
		if (deflate_add_header(strm))
			return ZEDC_STREAM_ERROR;
	}

	/* Ensure that output FIFO gets written first */
	deflate_write_out_fifo(strm);
	if (!output_data_avail(strm))
		return ZEDC_OK;

	/* Instructed to finish and no input data: write EOB and trailer */
	if ((strm->flush == ZEDC_FINISH) && !input_data_avail(strm)) {
		deflate_write_eob(strm);
		deflate_add_trailer(strm);
		deflate_write_out_fifo(strm);
	}

	/* End-Of-Block added, and written out */
	if ((strm->eob_added) && (strm->trailer_added) && fifo_empty(f))
		return ZEDC_STREAM_END;	/* done */

	/* Don't ask hardware if we have no output space */
	if (!output_data_avail(strm))
		return ZEDC_OK;

	/* Don't ask hardware if we have nothing to process */
	if (!input_data_avail(strm))
		return ZEDC_OK;

	/* Prepare Deflate DDCB */
	cmd->cmd = ZEDC_CMD_DEFLATE;
	cmd->acfunc = DDCB_ACFUNC_APP;
	cmd->cmdopts = DDCB_OPT_DEFL_SAVE_DICT;		/* SAVE_DICT  */

	if (strm->flags & ZEDC_FLG_CROSS_CHECK)
		cmd->cmdopts |= DDCB_OPT_DEFL_RAS_CHECK;/* RAS */

	/* Set DYNAMIC_HUFFMAN */
	if (dyn_huffman_supported(zedc) && (strm->strategy != ZEDC_FIXED))
		cmd->cmdopts |= DDCB_OPT_DEFL_IBUF_INDIR;

	cmd->asiv_length = 0x70 - 0x18;	/* range for crc protection */
	cmd->asv_length	 = 0xc0 - 0x80;
	cmd->ats = 0;

	/* input buffer */
	if ((strm->dma_type[ZEDC_IN] & DDCB_DMA_TYPE_MASK) ==
	    DDCB_DMA_TYPE_FLAT)
		cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_defl, in_buff,
					  ATS_TYPE_FLAT_RD);
	else    cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_defl, in_buff,
					  ATS_TYPE_SGL_RD);

	/* output buffer */
	if ((strm->dma_type[ZEDC_OUT] & DDCB_DMA_TYPE_MASK) ==
	    DDCB_DMA_TYPE_FLAT)
		cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_defl, out_buff,
					  ATS_TYPE_FLAT_RDWR);
	else    cmd->ats |= ATS_SET_FLAGS(struct zedc_asiv_defl, out_buff,
					  ATS_TYPE_SGL_RDWR);

	/* workspace */
	if ((strm->dma_type[ZEDC_WS] & DDCB_DMA_TYPE_MASK) ==
	    DDCB_DMA_TYPE_FLAT) {
		cmd->ats |= (ATS_SET_FLAGS(struct zedc_asiv_defl, in_dict,
					   ATS_TYPE_FLAT_RD) |
			     ATS_SET_FLAGS(struct zedc_asiv_defl, out_dict,
					   ATS_TYPE_FLAT_RDWR));
	} else {
		cmd->ats |= (ATS_SET_FLAGS(struct zedc_asiv_defl, in_dict,
					   ATS_TYPE_SGL_RD) |
			     ATS_SET_FLAGS(struct zedc_asiv_defl, out_dict,
					   ATS_TYPE_SGL_RDWR));
	}

	/* Setup ASIV part (provided in big endian byteorder) */
	asiv = (struct zedc_asiv_defl *)&cmd->asiv;
	asv = (struct zedc_asv_defl *)&cmd->asv;
	asiv->in_buff      = __cpu_to_be64((unsigned long)strm->next_in);
	asiv->in_buff_len  = __cpu_to_be32(strm->avail_in);
	asiv->out_buff     = __cpu_to_be64((unsigned long)strm->next_out);
	asiv->out_buff_len = __cpu_to_be32(strm->avail_out);

	/* Toggle workspace page (in <-> out) */
	p = strm->wsp_page;
	asiv->in_dict  = __cpu_to_be64((unsigned long)strm->wsp->dict[p] +
				       strm->out_dict_offs);
	asiv->out_dict = __cpu_to_be64((unsigned long)strm->wsp->dict[p ^ 1]);
	strm->wsp_page ^= 1;

	asiv->in_dict_len = __cpu_to_be32(strm->dict_len);
	asiv->out_dict_len = __cpu_to_be32(ZEDC_DICT_LEN);

	asiv->ibits[0] = strm->obyte;
	asiv->inumbits = strm->onumbits;
	asiv->in_crc32 = __cpu_to_be32(strm->crc32);
	asiv->in_adler32 = __cpu_to_be32(strm->adler32);

	/*
	 * Optimization attempt: If we are called with Z_FINISH, and
	 * we assume that the data will fit into the provided output
	 * buffer, we try to run the hardware without dictionary save
	 * function. If we do not see all data absorbed and all
	 * available output written, we need to restart with
	 * dictionary save option.
	 *
	 * The desire is to keep small transfers efficient. It will
	 * not have significant effect if we deal with huge data
	 * streams.
	 */
	cmd->cmdopts |= DDCB_OPT_DEFL_SAVE_DICT;
	tries = 1;

	if ((strm->flags & ZEDC_FLG_SKIP_LAST_DICT) &&
	    (((flush == ZEDC_FINISH) || (flush == ZEDC_FULL_FLUSH)) &&
	     (strm->avail_out >= strm->avail_in))) {

		out_dict = asiv->out_dict;
		out_dict_len = asiv->out_dict_len;

		cmd->cmdopts &= ~DDCB_OPT_DEFL_SAVE_DICT;
		asiv->out_dict = 0x0;
		asiv->out_dict_len = 0x0;
		tries = 2;
	}

	for (i = 0; i < tries; i++) {
		zedc_asiv_defl_print(strm, zedc_dbg);
		rc = zedc_execute_request(zedc, cmd);
		zedc_asv_defl_print(strm, zedc_dbg);

		strm->retc = cmd->retc;
		strm->attn = cmd->attn;
		strm->progress = cmd->progress;

		/* Check for unexecuted DDCBs too, where RETC is 0x000. */
		if ((rc < 0) || (cmd->retc == 0x000)) {
			struct ddcb_cmd *cmd = &strm->cmd;

			pr_err("deflate failed rc=%d card_rc=%d\n"
			       "  DDCB returned "
			       "(RETC=%03x ATTN=%04x PROGR=%x) %s\n",
			       rc, zedc->card_rc, cmd->retc,
			       cmd->attn, cmd->progress,
			       cmd->retc == 0x102 ? "" : "ERR");

			return ZEDC_STREAM_ERROR;
		}

		/* Great, all data absorbed and all data fitted into output */
		if ((strm->avail_in  == __be32_to_cpu(asv->inp_processed)) &&
		    (strm->avail_out >= __be32_to_cpu(asv->outp_returned)))
			break;

		/* What a pity, need to repeat to get back dictionary */
		if ((strm->flags & ZEDC_FLG_SKIP_LAST_DICT) &&
		    (((flush == ZEDC_FINISH) || (flush == ZEDC_FULL_FLUSH)) &&
		     (strm->avail_out >= strm->avail_in))) {
			cmd->cmdopts |= DDCB_OPT_DEFL_SAVE_DICT;
			asiv->out_dict = out_dict;
			asiv->out_dict_len = out_dict_len;

			pr_warn("[%s] What a pity, optimization did "
				"not work\n"
				"  (RETC=%03x ATTN=%04x PROGR=%x)\n",
				__func__, cmd->retc, cmd->attn, cmd->progress);
		}
	}

	/* Analyze ASV part (provided in big endian byteorder!) */
	strm->crc32 = __be32_to_cpu(asv->out_crc32);
	strm->adler32 = __be32_to_cpu(asv->out_adler32);
	strm->dict_len = __be16_to_cpu(asv->out_dict_used);
	strm->out_dict_offs = asv->out_dict_offs;

	if (strm->out_dict_offs >= 16) {
		pr_err("DICT_OFFSET too large (%u)\n", strm->out_dict_offs);
		return ZEDC_STREAM_ERROR;
	}

	/* Post-processing of DDCB status */
	rc  = deflate_process_results(strm, asv);
	if (rc < 0)
		return ZEDC_STREAM_ERROR;

	/* Instructed to finish and no input data, write EOB and trailer */
	if ((strm->flush == ZEDC_FINISH) && !input_data_avail(strm)) {
		deflate_write_eob(strm);	/* Add EOB */
		deflate_add_trailer(strm);	/* ZLIB/GZIP postfix */
		deflate_write_out_fifo(strm);
	}

	/* Handle ZEDC_SYNC_FLUSH + ZEDC_PARTIAL_FLUSH the same way
	   Testcase CDHF_03 */
	if ((strm->flush == ZEDC_SYNC_FLUSH) ||
		(strm->flush == ZEDC_PARTIAL_FLUSH)) {
		deflate_sync_flush(strm);
		deflate_write_out_fifo(strm);
	}

	/* FIX for HW290108 Testcase CDHF_06 */
	if (strm->flush == ZEDC_FULL_FLUSH) {
		deflate_sync_flush(strm);
		deflate_write_out_fifo(strm);
		strm->dict_len = 0;
	}

	/* End-Of-Block added, and written out */
	if ((strm->eob_added) && (strm->trailer_added) && fifo_empty(f))
		return ZEDC_STREAM_END;	/* done */

	return ZEDC_OK;
}

/**
 * @brief		end deflate (compress)
 * @param strm		common zedc parameter set
 */
int zedc_deflateEnd(zedc_streamp strm)
{
	zedc_handle_t zedc;
	struct zedc_fifo *f;

	if (!strm)
		return ZEDC_STREAM_ERROR;

	f = &strm->out_fifo;
	zedc = (zedc_handle_t)strm->device;
	if (!zedc)
		return ZEDC_STREAM_ERROR;

	while (!fifo_empty(f)) {
		uint8_t data;
		fifo_pop(f, &data);
		pr_err("FIFO not empty: %02x\n", data);
	}

	zedc_free_workspace(strm);
	return ZEDC_OK;
}

int zedc_deflateSetHeader(zedc_streamp strm, gzedc_headerp head)
{
	strm->gzip_head = head;
	return ZEDC_OK;
}
