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

#ifndef __ZEDC_DEFS_H__
#define __ZEDC_DEFS_H__

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */

#include <libddcb.h>

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(a)  (sizeof((a)) / sizeof((a)[0]))
#endif

#ifndef ABS
#  define ABS(a)	 (((a) < 0) ? -(a) : (a))
#endif

static inline pid_t gettid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

extern int zedc_dbg;
extern FILE *zedc_log;

#define pr_err(fmt, ...) do {						\
		if (zedc_log)						\
			fprintf(zedc_log, "%08x.%08x %s:%u: Error: " fmt, \
				getpid(), gettid(), __FILE__, __LINE__,	\
				## __VA_ARGS__);			\
	} while (0)

#define pr_warn(fmt, ...) do {						\
		if (zedc_log)						\
			fprintf(zedc_log, "%08x.%08x %s:%u: Warn: " fmt, \
				getpid(), gettid(), __FILE__, __LINE__,	\
				## __VA_ARGS__);			\
	} while (0)

#define	pr_dbg(fmt, ...) do {						\
		if (zedc_log && zedc_dbg)				\
			fprintf(zedc_log, fmt, ## __VA_ARGS__);		\
	} while (0)

#define	pr_info(fmt, ...) do {						\
		if (zedc_log && zedc_dbg)				\
			fprintf(zedc_log, "%08x.%08x %s:%u: Info: " fmt, \
				getpid(), gettid(), __FILE__, __LINE__,	\
				## __VA_ARGS__);			\
	} while (0)

#define	pr_log(dbg, fmt, ...) do {					\
		if (zedc_log && (dbg))					\
			fprintf(zedc_log, "%08x.%08x %s:%u: Info: " fmt, \
				getpid(), gettid(), __FILE__, __LINE__,	\
				## __VA_ARGS__);			\
	} while (0)

/******************************************************************************
 * zEDC Support
 *****************************************************************************/

/* zedc device descriptor */
struct zedc_dev_t {
	int mode;
	int zedc_rc;		/* libzedc return codes; detailed info
				 * in cases were we needed to return.
				 */
	accel_t card;		/* Ptr. to card */
	int card_rc;		/* libcard return codes */
	int card_errno;
	int collect_debug_data;
};

/**
 * APP_ID:
 *   0x00000000475a4950 old
 *   0x00000002475a4950 new
 *           VV
 *             G Z I P
 */

static inline int is_zedc(zedc_handle_t zedc)
{
	uint64_t app_id = accel_get_app_id(zedc->card);
	return (app_id & 0xFFFFFFFF) == 0x475a4950;
}

static inline int dyn_huffman_supported(zedc_handle_t zedc)
{
	uint64_t app_id = accel_get_app_id(zedc->card);
	return (app_id & 0xFF00000000ull) >= 0x0200000000ull;
}

/*
 * RFC1951
 *
 * BTYPE specifies how the data are compressed, as follows:
 *   00 - no compression
 *   01 - compressed with fixed Huffman codes
 *   10 - compressed with dynamic Huffman codes
 *   11 - reserved (error)
 *
 * E.g. fixed Header 01, read from left ...
 *
 * RFC1951 End-Of-Block Marker = %000_0000
 */
#define HDR_BTYPE_NO	    0x00
#define HDR_BTYPE_FIXED	    0x02
#define HDR_BTYPE_DYN	    0x04
#define HDR_BTYPE_RES	    0x06
#define	  HDR_BFINAL	    0x01
#define FIXED_EOB	    0x00 /* 7 bits 0s */

/* RFC1952 GZIP */
#define FTEXT		    0x01
#define FHCRC		    0x02
#define FEXTRA		    0x04
#define FNAME		    0x08
#define FCOMMENT	    0x10

#define FNAME_MAXLEN	    64	/* ensure that we do not overflow our FIFO */
#define FCOMMENT_MAXLEN	    64	/* ensure that we do not overflow our FIFO */

/**
 * @brief	manage execution of an inflate or a deflate job
 * @param zedc	ZEDC device handle
 * @param cmd	pointer to command descriptor
 */
int zedc_execute_request(zedc_handle_t zedc, struct ddcb_cmd *cmd);
int zedc_alloc_workspace(zedc_streamp strm);
int zedc_free_workspace(zedc_streamp strm);

void zedc_asv_infl_print(zedc_streamp strm);
void zedc_asiv_infl_print(zedc_streamp strm);

void zedc_asv_defl_print(zedc_streamp strm, int dbg);
void zedc_asiv_defl_print(zedc_streamp strm, int dbg);

/**
 * @brief Prepare format specific deflate header when user
 *	calls initializes decompression.
 *	provided window_bits:
 *	   8 .... 15: ZLIB    / RFC1950 (window size 2^8 ... 2^15)
 *	  -8 ... -15: DEFLATE / RFC1951 (window size 2^8 ... 2^15)
 *	       >= 16: GZIP    / RFC1952
 */
int zedc_format_init(struct zedc_stream_s *strm);
unsigned long __adler32(unsigned long adl, const unsigned char *buf, int len);

#endif	/* __ZEDC_DEFS_H__ */
