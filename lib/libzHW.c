/*
 * Copyright 2015 International Business Machines
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
 * @brief De/Compression supporting RFC1950, RFC1951 and RFC1952.
 *
 * IBM Accelerator Family 'GenWQE'
 */

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

#include <deflate_ddcb.h>
#include <libddcb.h>
#include <libzHW.h>
#include "hw_defs.h"

FILE *zedc_log = NULL;
int zedc_dbg = 0;

/* lookup table for error text messages */
struct err_lookup {
	int		num;	/* error number */
	const char	*str;	/* corresponding error string */
};

static struct err_lookup zedc_errlist[] = {
	{ ZEDC_OK, "success" },
	{ ZEDC_ERRNO, "system error, please see errno" },
	{ ZEDC_STREAM_ERROR, "stream state was inconsistent (for example "
				"if next_in or next_out was NULL)" },
	{ ZEDC_DATA_ERROR, "invalid or incomplete inflate/deflate data" },
	{ ZEDC_MEM_ERROR, "out of memory" },
	{ ZEDC_BUF_ERROR, "no progress is possible (for example avail_in or "
				"avail_out was zero)" },
	{ ZEDC_ERR_CARD, "problem with the accelerator card detected, please "
				"see errno, carderr and returned data" },
	{ ZEDC_ERR_INVAL, "invalid parameter" },

	{ ZEDC_ERR_RETLEN, "returned invalid length" },
	{ ZEDC_ERR_RETOBITS, "hardware returned invalid output bytes" },
	{ ZEDC_ERR_TREE_OVERRUN, "hardware too many tree bits" },
	{ ZEDC_ERR_ZLIB_HDR, "illegal zlib header found" },
	{ ZEDC_ERR_ADLER32, "adler32 mismatch" },
	{ ZEDC_ERR_GZIP_HDR, "illegal gzip header found" },
	{ ZEDC_ERR_CRC32, "crc32 mismatch" },
	{ ZEDC_ERR_UNSUPPORTED, "currently unsupported function" },
	{ ZEDC_ERR_DICT_OVERRUN,  "dictionary overrun" },
	{ ZEDC_ERR_INP_MISSING,  "further input data missing" },
	{ ZEDC_ERR_ILLEGAL_APPID, "illegal application id" },
};

static int zedc_nerr = ARRAY_SIZE(zedc_errlist);

const char *zedc_Version(void)
{
	return GIT_VERSION;
}

void zedc_set_logfile(FILE *logfile)
{
  zedc_log = logfile;
}

int zedc_clearerr(zedc_handle_t zedc)
{
	if (!zedc)
		return ZEDC_ERR_INVAL;

	zedc->zedc_rc = 0;
	zedc->card_rc = 0;

	return ZEDC_OK;
}

/**
 * @brief		provide error message for a corresponding error number
 * @param errnum	error number
 *
 * @return		pointer to error text message
 */
const char *zedc_strerror(int errnum)
{
	int i;

	i = 0;
	while (i < zedc_nerr) {
		if (errnum == zedc_errlist[i].num)
			return zedc_errlist[i].str;
		i++;
	}
	return "unknown";
}

int zedc_carderr(zedc_handle_t zedc)
{
	if (!zedc)
		return ZEDC_ERR_INVAL;

	return zedc->card_rc;
}

int zedc_liberr(zedc_handle_t zedc)
{
	if (!zedc)
		return ZEDC_ERR_INVAL;

	return zedc->zedc_rc;
}

struct ddcb_cmd *zedc_last_cmd(struct zedc_stream_s *strm)
{
	if (!strm)
		return NULL;

	return &strm->cmd;
}

/**
 * @brief Print final compression/decompression status
 * FIXME Printing to stdout is normally not allowed! See zpipe use-case!
 * FIXME I think this should be removed from the library if possible.
 */
int zedc_pstatus(struct zedc_stream_s *strm, const char *task)
{
	int c;

	c = fprintf(stdout,
		    "%s finished (avail_in=%d avail_out=%d total_in=%ld "
		    "total_out=%ld)\n", task, strm->avail_in, strm->avail_out,
		    strm->total_in, strm->total_out);

	switch (strm->format) {
	case ZEDC_FORMAT_GZIP:
		c += fprintf(stdout, "  GZIP CRC32=0x%08x (eval=0x%08x)\n",
			     strm->file_crc32, strm->crc32);
		c += fprintf(stdout, "  GZIP ISIZE=0x%x (%u)\n",
			     strm->file_size, strm->file_size);
		break;

	case ZEDC_FORMAT_ZLIB:
		c += fprintf(stdout, "  ZLIB ADLER32=0x%08x (eval=0x%08x)\n",
			     strm->file_adler32, strm->adler32);
		break;

	default:
		break;
	}
	return c;
}

/**
 * @brief		enable or disable debug outputs from library.
 *			pr_info is activated or disabled
 * @param onoff		if 0 -> no outputs
 */
void zedc_lib_debug(int onoff)
{
	zedc_dbg = onoff;
}

/**
 * @brief	print 'Application Specific Invariant' part of inflate DDCB
  *
 * @param asiv	pointer to data area
 *
 */
void zedc_asiv_infl_print(zedc_streamp strm)
{
	struct ddcb_cmd *cmd = &strm->cmd;
	struct zedc_wsp *wsp = strm->wsp;
	struct zedc_asiv_infl *asiv = (struct zedc_asiv_infl *)cmd->asiv;
	uint32_t out_buff_len = __be32_to_cpu(asiv->out_buff_len);
	uint32_t in_buff_len  = __be32_to_cpu(asiv->in_buff_len);
	uint16_t in_hdr_bits  = __be16_to_cpu(asiv->in_hdr_bits);

	pr_info("Inflate ASIV (sent):\n"
		"  [20] IN_BUFF         = 0x%llx\n"
		"  [28] IN_BUFF_LEN     = 0x%x (%d)\n"
		"  [30] OUT_BUFF        = 0x%llx\n"
		"  [38] OUT_BUFF_LEN    = 0x%x (%d)\n"
		"  [40] IN_DICT         = 0x%llx\n"
		"  [60] IN_DICT_LEN     = 0x%x (%d)\n"
		"  [40] OUT_DICT        = 0x%llx\n"
		"  [60] OUT_DICT_LEN    = 0x%x (%d)\n"
		"  [50] IN_HDR_SCRATCH  = 0x%llx\n"
		"  [58] IN_SCRATCH_LEN  = 0x%x (%u)\n"
		"  [5c] IN_HDR_BITS     = %u (%u bytes + %u bits)\n"
		"  [5e] IN_HDR_IB       = %u\n"
		"  [5e] SCRATCH_IB      = %u\n"
		"  [2c] IN_CRC32        = 0x%08x\n"
		"  [3c] IN_ADLER32      = 0x%08x\n",
		(long long)__be64_to_cpu(asiv->in_buff),
		in_buff_len, in_buff_len,
		(long long)__be64_to_cpu(asiv->out_buff),
		out_buff_len, out_buff_len,
		(long long)__be64_to_cpu(asiv->in_dict),
		__be32_to_cpu(asiv->in_dict_len),
		__be32_to_cpu(asiv->in_dict_len),
		(long long)__be64_to_cpu(asiv->out_dict),
		__be32_to_cpu(asiv->out_dict_len),
		__be32_to_cpu(asiv->out_dict_len),
		(long long)__be64_to_cpu(asiv->inp_scratch),
		__be32_to_cpu(asiv->in_scratch_len),
		__be32_to_cpu(asiv->in_scratch_len),
		in_hdr_bits, in_hdr_bits / 8, in_hdr_bits % 8,
		asiv->hdr_ib, asiv->scratch_ib,
		__be32_to_cpu(asiv->in_crc32),
		__be32_to_cpu(asiv->in_adler32));

	pr_info("\n"
		"       ATS             = 0x%08llx\n"
		"       CMD             = 0x%02x\n"
		"       CMDOPTS         = 0x%02x\n",
		(long long)cmd->ats, cmd->cmd, cmd->cmdopts);

	if (zedc_dbg > 3) {
		pr_info("Workspace/Dict0:\n");
		ddcb_hexdump(zedc_log, wsp->dict[0], ZEDC_DICT_LEN);

		pr_info("Workspace/Dict1:\n");
		ddcb_hexdump(zedc_log, wsp->dict[1], ZEDC_DICT_LEN);

		pr_info("Workspace/Tree:\n");
		ddcb_hexdump(zedc_log, wsp->tree, ZEDC_TREE_LEN);
	}
}

/**
 * @brief	print 'Application Specific Invariant' part of deflate DDCB
 *
 * @param asiv	pointer to data area
 *
 */
void zedc_asiv_defl_print(zedc_streamp strm, int dbg)
{
	struct ddcb_cmd *cmd = &strm->cmd;
	struct zedc_asiv_defl *asiv = (struct zedc_asiv_defl *)cmd->asiv;
	uint32_t out_buff_len = __be32_to_cpu(asiv->out_buff_len);
	uint32_t in_buff_len = __be32_to_cpu(asiv->in_buff_len);

	pr_log(dbg, "Deflate ASIV (sent):\n"
	       "  [20] IN_BUFF         = 0x%llx\n"
	       "  [28] IN_BUFF_LEN     = 0x%x (%d)\n"
	       "  [2c] IN_CRC32        = 0x%08x\n"
	       "  [30] OUT_BUFF        = 0x%llx\n"
	       "  [38] OUT_BUFF_LEN    = 0x%x (%d)\n"
	       "  [3c] IN_ADLER32      = 0x%08x\n"
	       "  [40] IN_DICT         = 0x%llx\n"
	       "  [48] IN_DICT_LEN     = 0x%x (%d)\n"
	       "  [60] OUT_DICT        = 0x%llx\n"
	       "  [68] OUT_DICT_LEN    = 0x%x (%d)\n"
	       "  [7f] INUMBITS        = 0x%x\n",
	       (long long)__be64_to_cpu(asiv->in_buff),
	       in_buff_len, in_buff_len,
	       __be32_to_cpu(asiv->in_crc32),
	       (long long)__be64_to_cpu(asiv->out_buff),
	       out_buff_len, out_buff_len,
	       __be32_to_cpu(asiv->in_adler32),
	       (long long)__be64_to_cpu(asiv->in_dict),
	       __be32_to_cpu(asiv->in_dict_len),
	       __be32_to_cpu(asiv->in_dict_len),
	       (long long)__be64_to_cpu(asiv->out_dict),
	       __be32_to_cpu(asiv->out_dict_len),
	       __be32_to_cpu(asiv->out_dict_len),
	       asiv->inumbits);

	pr_log(dbg, "\n"
	       "       ATS             = 0x%08llx\n"
	       "       CMD             = 0x%02x\n"
	       "       CMDOPTS         = 0x%02x\n",
	       (long long)cmd->ats, cmd->cmd, cmd->cmdopts);

	pr_log(dbg, "  [7f] IBITS: %02x %02x %02x %02x %02x %02x %02x\n",
	       asiv->ibits[0], asiv->ibits[1], asiv->ibits[2],
	       asiv->ibits[3], asiv->ibits[4], asiv->ibits[5],
	       asiv->ibits[6]);
}

/**
 * @brief	print 'Application Specific Variant' part of deflate DDCB
 * @param asv	pointer to data area
 */
void zedc_asv_defl_print(zedc_streamp strm, int dbg)
{
	struct ddcb_cmd *cmd = &strm->cmd;
	struct zedc_asv_defl *asv = (struct zedc_asv_defl *)cmd->asv;
	uint32_t inp_processed = __be32_to_cpu(asv->inp_processed);
	uint32_t outp_returned = __be32_to_cpu(asv->outp_returned);

	pr_log(dbg, "Deflate ASV (received):\n"
	       "  [80] OUT_DICT_USED    = 0x%x (%d)\n"
	       "  [87] ONUMBITS         = 0x%x (%u)\n"
	       "  [90] OUT_CRC32        = 0x%08x\n"
	       "  [94] OUT_ADLER32      = 0x%08x\n"
	       "  [98] INP_PROCESSED    = 0x%x (%d)\n"
	       "  [9c] OUTP_RETURNED    = 0x%x (%d)\n"
	       "  [b8] OUT_DICT_OFFS    = 0x%x (%d)\n",
	       __be16_to_cpu(asv->out_dict_used),
	       __be16_to_cpu(asv->out_dict_used),
	       asv->onumbits, asv->onumbits, __be32_to_cpu(asv->out_crc32),
	       __be32_to_cpu(asv->out_adler32),
	       inp_processed, inp_processed, outp_returned, outp_returned,
	       asv->out_dict_offs, asv->out_dict_offs);

	pr_log(dbg, "\n"
	       "       ATS             = 0x%08llx\n"
	       "       CMD             = 0x%02x\n"
	       "       CMDOPTS         = 0x%02x\n",
	       (long long)cmd->ats, cmd->cmd, cmd->cmdopts);

	if (dbg) {
		pr_log(dbg, "  OBITS:\n");
		ddcb_hexdump(zedc_log, asv->obits, ZEDC_ONUMBYTES_v1);
		pr_log(dbg, "  OBITS_EXTRA:\n");
		ddcb_hexdump(zedc_log, asv->obits_extra, ZEDC_ONUMBYTES_EXTRA);
	}
}

/**
 * @brief	print 'Application Specific Variant' part of inflate DDCB
 * @param asv	pointer to data area
 */
void zedc_asv_infl_print(zedc_streamp strm)
{
	struct ddcb_cmd *cmd = &strm->cmd;
	struct zedc_wsp *wsp = strm->wsp;
	struct zedc_asv_infl *asv = (struct zedc_asv_infl *)cmd->asv;
	uint32_t inp_processed = __be32_to_cpu(asv->inp_processed);
	uint32_t outp_returned = __be32_to_cpu(asv->outp_returned);
	uint16_t hdr_bits      = __be16_to_cpu(asv->out_hdr_bits);

	pr_info("Inflate ASV (received):\n"
		"  [80] OUT_DICT_USED    = 0x%x (%u)\n"
		"  [82] COPYBLOCK_LEN    = 0x%x (%u)\n"
		"  [85] INFL_STAT        = 0x%x\n"
		"  [87] PROC_BITS        = 0x%x\n"
		"  [88] HDR_START        = 0x%x\n"
		"  [8d] HDR_START_BITS   = 0x%x\n"
		"  [8e] OUT_HDR_BITS     = 0x%x (%u) (%u bytes + %u bits)\n"
		"  [90] OUT_CRC32        = 0x%08x\n"
		"  [94] OUT_ADLER32      = 0x%08x\n"
		"  [98] INP_PROCESSED    = 0x%x (%u)\n"
		"  [9c] OUTP_RETURNED    = 0x%x (%u)\n"
		"  [b8] OUT_DICT_OFFS    = 0x%x (%u)\n"
		"  [b8] OBYTES_IN_DICT   = 0x%x (%u)\n",
		__be16_to_cpu(asv->out_dict_used),
		__be16_to_cpu(asv->out_dict_used),
		__be16_to_cpu(asv->copyblock_len),
		__be16_to_cpu(asv->copyblock_len),
		asv->infl_stat, asv->proc_bits,
		__be32_to_cpu(asv->hdr_start), asv->hdr_start_bits,
		hdr_bits, hdr_bits, hdr_bits / 8, hdr_bits % 8,
		__be32_to_cpu(asv->out_crc32),
		__be32_to_cpu(asv->out_adler32),
		inp_processed, inp_processed,
		outp_returned, outp_returned,
		asv->out_dict_offs, asv->out_dict_offs,
		__be16_to_cpu(asv->obytes_in_dict),
		__be16_to_cpu(asv->obytes_in_dict));

	pr_info("\n"
		"       ATS             = 0x%08llx\n"
		"       CMD             = 0x%02x\n"
		"       CMDOPTS         = 0x%02x\n",
		(long long)cmd->ats, cmd->cmd, cmd->cmdopts);

	if (zedc_dbg > 3) {
		pr_info("Workspace/Dict0:\n");
		ddcb_hexdump(zedc_log, wsp->dict[0], ZEDC_DICT_LEN);

		pr_info("Workspace/Dict1:\n");
		ddcb_hexdump(zedc_log, wsp->dict[1], ZEDC_DICT_LEN);

		pr_info("Workspace/Tree:\n");
		ddcb_hexdump(zedc_log, wsp->tree, ZEDC_TREE_LEN);
	}
}

/****************************************************************************
 * ZEDC Compression/Decompression device support
 ***************************************************************************/

void zedc_overwrite_slu_id(zedc_handle_t zedc __attribute__((unused)),
			   uint64_t slu_id __attribute__((unused)))
{
	/* FIXME disable for now */
	/* card_overwrite_slu_id(zedc->card, slu_id); */
}

void zedc_overwrite_app_id(zedc_handle_t zedc __attribute__((unused)),
			   uint64_t app_id __attribute__((unused)))
{
	/* FIXME disable for now */
	/* card_overwrite_app_id(zedc->card, app_id); */
}

/**
 * @brief		initialization of the ZEDC library.
 *			allocates and presets required memory, sets version
 *			numbers	and opens a zedc device.
 * @param dev_no	card number
 * @param mode		SIGIO mode
 * @param err_code	pointer to error code (return)
 *
 * @return		0 if success
 */
zedc_handle_t zedc_open(int dev_no, int dev_type, int mode, int *err_code)
{
	char *env;
	zedc_handle_t zedc;
	uint64_t app_id = DDCB_APPL_ID_GZIP;
	uint64_t app_id_mask = DDCB_APPL_ID_MASK;

	zedc = malloc(sizeof(*zedc));
	if (!zedc) {
		*err_code = ZEDC_ERRNO;
		return NULL;
	}
	memset(zedc, 0, sizeof(*zedc));
	zedc->mode = mode;

	/* Check Appl id GZIP Version 2 */
	if (dev_no == ACCEL_REDUNDANT) {
		app_id = DDCB_APPL_ID_GZIP2;
		app_id_mask = DDCB_APPL_ID_MASK_VER;
	}

	/* Check Appl id GZIP Version 2 */
	zedc->card = accel_open(dev_no, dev_type, mode, &zedc->card_rc,
				app_id, app_id_mask);
	if (zedc->card == NULL) {
		*err_code = ZEDC_ERR_CARD;
		goto free_zedc;
	}
	zedc->card_rc = 0;	/* FIXME */
	env = getenv("DDCB_DEBUG");
	if (env)
		zedc_dbg = atoi(env);

	*err_code = 0;
	return zedc;

 free_zedc:
	free(zedc);
	return NULL;
}

/**
 * @brief	manage execution of an inflate or a deflate job
 * @param zedc	ZEDC device handle
 * @param cmd	pointer to command descriptor
 */
int zedc_execute_request(zedc_handle_t zedc, struct ddcb_cmd *cmd)
{
	int rc = accel_ddcb_execute(zedc->card, cmd, &zedc->card_rc,
				    &zedc->card_errno);

	pr_info("  DDCB returned rc=%d card_rc=%d "
		"(RETC=%03x ATTN=%04x PROGR=%x) %s\n",
		rc, zedc->card_rc, cmd->retc, cmd->attn, cmd->progress,
		cmd->retc == 0x102 ? "" : "ERR");

	return rc;
}

/**
 * @brief	end ZEDC library accesses close all open files, free memory
 * @param zedc	pointer to the opened device descriptor
 * @return	ZEDC_OK if everything is ok.
 */
int zedc_close(zedc_handle_t zedc)
{
	if (!zedc)
		return ZEDC_ERR_INVAL;

	accel_close(zedc->card);
	free(zedc);
	return ZEDC_OK;
}

/**
 * @brief Memory allocation for compression/decompression buffers.
 */
void *zedc_memalign(zedc_handle_t zedc, size_t size, enum zedc_mtype mtype)
{
	void *ptr;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	if (!zedc)
		return NULL;

	/* normal operation */
	if ((mtype & DDCB_DMA_TYPE_MASK) == DDCB_DMA_TYPE_FLAT) {
		ptr = accel_malloc(zedc->card, size);
		if (ptr == MAP_FAILED)
			return NULL;
		return ptr;
	}

	ptr = memalign(page_size, size);
	if (ptr == MAP_FAILED)
		return NULL;

	if (mtype & DDCB_DMA_PIN_MEMORY) {
		zedc->card_rc = accel_pin_memory(zedc->card, ptr, size, 1);
		if (zedc->card_rc != DDCB_OK) {
			free(ptr);
			return NULL;
		}
	}
	return ptr;
}

/**
 * @brief Use driver to free memory.
 */
int zedc_free(zedc_handle_t zedc, void *ptr, size_t size,
	      enum zedc_mtype mtype)
{
	int rc;

	if (!zedc)
		return ZEDC_ERR_INVAL;

	if (ptr == NULL)
		return 0;

	/* normal operation */
	if ((mtype & DDCB_DMA_TYPE_MASK) == DDCB_DMA_TYPE_FLAT) {
		rc = accel_free(zedc->card, ptr, size);
		if (rc != DDCB_OK)
			return ZEDC_ERRNO;

		return 0;
	}

	if (mtype & DDCB_DMA_PIN_MEMORY) {
		zedc->card_rc = accel_unpin_memory(zedc->card, ptr, size);
		if (zedc->card_rc != DDCB_OK) {
			free(ptr);
			return ZEDC_ERR_CARD;
		}
	}

	free(ptr);
	return 0;
}

int zedc_pin_memory(zedc_handle_t zedc, const void *addr, size_t size,
		    int dir)
{
	if (!zedc)
		return ZEDC_ERR_INVAL;

	zedc->card_rc = accel_pin_memory(zedc->card, addr, size, dir);
	if (zedc->card_rc != DDCB_OK)
		return ZEDC_ERR_CARD;

	return ZEDC_OK;
}

int zedc_unpin_memory(zedc_handle_t zedc, const void *addr, size_t size)
{
	if (!zedc)
		return ZEDC_ERR_INVAL;

	zedc->card_rc = accel_unpin_memory(zedc->card, addr, size);
	if (zedc->card_rc != DDCB_OK)
		return ZEDC_ERR_CARD;

	return ZEDC_OK;
}

/**
 * @brief Prepare format specific deflate header when user
 *	calls initializes decompression.
 *	provided window_bits:
 *	  -8 ... -15: DEFLATE / RFC1951 (window size 2^8 ... 2^15)
 *	   8 ...  15: ZLIB    / RFC1950 (window size 2^8 ... 2^15)
 *        16 ...  23: GZIP    / RFC1952
 *	  24 ...  31: GZIP/ZLIB AUTOPROBE
 *      FIXME We do not do autoprobing at this point in time.
 */
int zedc_format_init(struct zedc_stream_s *strm)
{
	if ((strm->windowBits <= -8) && (strm->windowBits >= -15)) {
		strm->format = ZEDC_FORMAT_DEFL;
		return ZEDC_OK;
	}

	if ((strm->windowBits >= 8) && (strm->windowBits <= 15)) {
		strm->format = ZEDC_FORMAT_ZLIB;
		return ZEDC_OK;
	}

	if ((strm->windowBits >= 16) && (strm->windowBits <= 23)) {
		strm->format = ZEDC_FORMAT_GZIP;
		return ZEDC_OK;
	}

	if ((strm->windowBits >= 24) && (strm->windowBits <= 31)) {
		strm->format = ZEDC_FORMAT_GZIP;
		return ZEDC_OK;
	}


	/* pr_err("window_bits invalid (%d)\n", strm->windowBits); */
	return ZEDC_DATA_ERROR;
}

int zedc_alloc_workspace(zedc_streamp strm)
{
	zedc_handle_t zedc = (zedc_handle_t)strm->device;

	strm->wsp = zedc_memalign(zedc, sizeof(struct zedc_wsp),
				  strm->dma_type[ZEDC_WS]);
	if (strm->wsp == NULL)
		return ZEDC_MEM_ERROR;

	/* FIXME valgrind complained about this memory piece not being initialized */
	memset(strm->wsp, 0, sizeof(struct zedc_wsp));

	return ZEDC_OK;
}

int zedc_free_workspace(zedc_streamp strm)
{
	int rc;
	zedc_handle_t zedc = (zedc_handle_t)strm->device;

	rc = zedc_free(zedc, strm->wsp, sizeof(struct zedc_wsp),
		       strm->dma_type[ZEDC_WS]);

	strm->wsp = NULL;
	return rc;
}

#define BASE 65521 /* largest prime smaller than 65536 */

unsigned long __adler32(unsigned long adler,
			const unsigned char *buf, int len)
{
	unsigned long s1 = adler & 0xffff;
	unsigned long s2 = (adler >> 16) & 0xffff;
	int n;

	for (n = 0; n < len; n++) {
		s1 = (s1 + buf[n]) % BASE;
		s2 = (s2 + s1)     % BASE;
	}
	return (s2 << 16) + s1;
}
