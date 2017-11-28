/*
 * Copyright 2014, 2016 International Business Machines
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

#ifndef __LIBZHW_H__
#define __LIBZHW_H__

/*
 * @brief Compression/decompression supporting RFC1950, RFC1951 and
 * RFC1952. The data structure is similar to the one described in
 * zlib.h, but contains some more information required to do the
 * hardware compression/decompression.
 *
 * In addition to the compression/decompression related functions/data
 * it defines functions to open/operate/close the GenWQE card which is
 * used to implement the hardware accelerated
 * compression/decompression.
 *
 * IBM Accelerator Family 'GenWQE'/zEDC Compression
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <libddcb.h>
#include <deflate_ddcb.h>
#include <deflate_fifo.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DDCB_APPL_ID_GZIP	0x00000000475a4950ull /* The GZIP APPL id */
#define DDCB_APPL_ID_GZIP2	0x00000002475a4950ull /* The GZIP2 APPL id */

/* Different zlib versions used different codes for flush! */
#define ZEDC_NO_FLUSH		0
#define ZEDC_PARTIAL_FLUSH	1
#define ZEDC_SYNC_FLUSH		2
#define ZEDC_FULL_FLUSH		3
#define ZEDC_FINISH		4
#define ZEDC_BLOCK		5

#define ZEDC_NO_COMPRESSION	0
#define ZEDC_BEST_SPEED		1
#define ZEDC_BEST_COMPRESSION	9
#define ZEDC_DEFAULT_COMPRESSION (-1)

#define ZEDC_FILTERED		1
#define ZEDC_HUFFMAN_ONLY	2
#define ZEDC_RLE		3
#define ZEDC_FIXED		4
#define ZEDC_DEFAULT_STRATEGY	0

/* Fragile, since return codes might not match local zlib implementation */
#define ZEDC_OK			0
#define ZEDC_STREAM_END		1
#define ZEDC_NEED_DICT		2
#define ZEDC_ERRNO		(-1)  /* see errno for more details */
#define ZEDC_STREAM_ERROR	(-2)  /* please see errno or zedc_liberr */
#define ZEDC_DATA_ERROR		(-3)  /* see zedc_carderr for more details */
#define ZEDC_MEM_ERROR		(-4)
#define ZEDC_BUF_ERROR		(-5)

/* zEDC specific enhancements */
#define ZEDC_ERR_CARD		(-307)	/* see zedc_carderr for details */
#define ZEDC_ERR_INVAL		(-308)	/* illegal parameters */
#define ZEDC_ERR_RETLEN		(-309)	/* returned invalid length */
#define ZEDC_ERR_RETOBITS	(-310)	/* returned invalid output bytes */
#define ZEDC_ERR_TREE_OVERRUN	(-311)	/* too many tree bits  */
#define ZEDC_ERR_ZLIB_HDR	(-312)	/* illegal zlib header found */
#define ZEDC_ERR_ADLER32	(-313)	/* adler32 mismatch */
#define ZEDC_ERR_GZIP_HDR	(-314)	/* illegal gzip header found */
#define ZEDC_ERR_CRC32		(-315)	/* crc32 mismatch */
#define ZEDC_ERR_UNSUPPORTED	(-316)	/* currently unsupported function */
#define ZEDC_ERR_DICT_OVERRUN	(-317)	/* dictionary overrun */
#define ZEDC_ERR_INP_MISSING	(-318)	/* further input data missing */
#define ZEDC_ERR_ILLEGAL_APPID	(-319)	/* unsupported APP_ID */

#define ZEDC_NULL		NULL
#define ZEDC_DEFLATED		8
/* The deflate compression method (the only one supported) */

#define	ZEDC_FORMAT_DEFL	0
#define	ZEDC_FORMAT_ZLIB	1
#define	ZEDC_FORMAT_GZIP	2

#define	ZEDC_FORMAT_STORAGE	18	 /* GZIP/ZLIB header storage */

/* NOTE: Always turn CROSS_CHECK on, otherwise you loose data protection */
#define ZEDC_FLG_CROSS_CHECK	(1 << 0) /* flag: inflate<->deflate check */
#define ZEDC_FLG_DEBUG_DATA	(1 << 1) /* flag: collect debug data */

/*
 * The SKIP_LAST_DICT flag can be used to omit transmitting the last
 * dictionary on an inflate/deflate request. If the output buffer is
 * not large enough the DDCB will be repeated with the SAVE_DICT flag
 * enabled, such that compression/decompression can properly
 * continue. It might help to reduce hardware time especially for many
 * independent small transfers. E.g. 64KiB data will cause an
 * osolete 32KiB dictionary transfer with zEDC/zEDCv2 bitstreams.
 *
 * For large files the effect is not noticeable.
 *
 * Note: This flag cannot be used in verification tools like
 * genwqe_zcomp, since there we check dictionary consistency by
 * comparing the hardware dictionary with a private software
 * maintained dictionary (-z option).
 */
#define ZEDC_FLG_SKIP_LAST_DICT	(1 << 2) /* flag: try to omit last dict */

/**
 * We might have addresses within the ASIV data. Those need to be
 * replaced by valid DMA addresses to the buffer, sg-list or
 * child-block in the kernel driver handling the request.
 */
enum zedc_mtype {
	DDCB_DMA_TYPE_MASK   = 0x18, /**< mask off type */
	DDCB_DMA_TYPE_FLAT   = 0x08, /**< contignous DMA block */
	DDCB_DMA_TYPE_SGLIST = 0x10, /**< DMA sg-list */
	DDCB_DMA_WRITEABLE   = 0x04, /**< memory writeable? */
	DDCB_DMA_PIN_MEMORY  = 0x20, /**< pin sgl memory after allocation */
};

/* Index for zedc_mtype information */
#define ZEDC_IN  0		/* input buffer */
#define ZEDC_OUT 1		/* output buffer */
#define ZEDC_WS  2		/* workspace buffer */

/**< data structure for dict check for integrity check by genwqe_zedc  */
struct zedc_dict_ref_s {
	uint8_t		*addr;		/* local reference dictionary */
	unsigned	wr;		/* wr offset */
	unsigned	in_offs;
	unsigned long	last_total;
};

typedef enum e_head_state {
	HEADER_START = 0,	/* Enter */
	FLAGS_CHECK_EMPTY,	/* No Flags set State */
	FLAGS_CHECK_EXTRA, FLAGS_GET_EXTRA_LEN1, FLAGS_GET_EXTRA_LEN2,
	FLAGS_GET_EXTRA,
	FLAGS_CHECK_FNAME, FLAGS_GET_FNAME,
	FLAGS_CHECK_FCOMMENT, FLAGS_GET_FCOMMENT,
	FLAGS_CHECK_FHCRC, FLAGS_GET_FHCRC1, FLAGS_GET_FHCRC2,
	FLAGS_CHECK_FTEXT,
	ZLIB_ADLER,		/* State for zlib only */
	HEADER_DONE
} head_state;

/*
 * Gzip header information passed to and from zlib routines. See RFC
 * 1952 for more details on the meanings of these fields.
 */
typedef struct gzedc_header_s {
	int text;
	unsigned long time;	/* modification time */
	int xflags;		/* extra flags (not used for write) */
	int os;			/* operating system */
	uint8_t *extra;	/* pointer to extra field or Z_NULL if none */
	unsigned int extra_len; /* extra field len (valid if extra!=Z_NULL) */
	unsigned int extra_max; /* space at extra (only when reading hdr) */
	char *name;	/* ptr to zero-terminated filename or Z_NULL */
	unsigned int name_max;	/* space at name (only when reading header) */
	char *comment;	/* ptr to zero-terminated comment or Z_NULL */
	unsigned int comm_max;	/* space at comment (only when reading hdr) */
	int hcrc;		/* true if there was or will be a header crc */
	int done;		/* true when done reading gzip header
				   (not used when writing a gzip
				   file) */
} gzedc_header;

typedef gzedc_header *gzedc_headerp;

/**
 * @note Data structure which should match what libz offers plus some
 * additional changes needed for hardware compression/decompression.
 *
 * FIXME This data-structure is way too large. Fields are duplicated
 * with content which is already in the DDCB execution request. We
 * could define two DDCB request data structures and alternate between
 * those to keep the amount of copying data small. Also the different
 * naming between this and the DDCB request data structures causes the
 * code to become error prone and badly readable.
 *
 * FIXME We have here three FIFOs which serve a similar purpose:
 *
 *  1)  prefx:  used to contain the header data which did not fit
 *      into the user output buffer.
 *  1a) in addition there is the obytes array which contains some more
 *      output data too.
 *      We already removed in this version: obytes[], good
 *
 *  => Merging those buffers makes a lot of sense, because both create similar
 *     code for the same purpose!
 *
 *  2)  postfx: used to hold the trailer data e.g. CRC32/ADLER32/LEN
 *      in case of RFC1950, RFC1952 before it is completely read in.
 *
 *  => Adding the ZLIB/GZ trailers is very similar to adding the EOB,
 *     or FEOB for RFC1951. Merging would make here very much sense
 *     too.
 */
typedef struct zedc_stream_s {
	/* parameters for the supported functions */
	int		level;		/**< compression level */
	int		method;		/**< must be Z_DEFLATED for zlib */
	int		windowBits;
			/*  -15..-8 = raw deflate, window size (2^-n)
			 *    8..15 = zlib window size (2^n) default=15
			 *   24..31 = gzip encoding */

	int		memLevel;	/**< 1...9 (default=8) */
	int		strategy;	/**< force compression algorithm */
	int		flush;
	int		data_type;	/**< best guess dtype: ascii/binary*/

	/* stream data management */
	const uint8_t	*next_in;	/**< next input byte */
	unsigned int	avail_in;	/**< # of bytes available at next_in */
	unsigned long	total_in;	/**< total nb of inp read so far */

	uint8_t		*next_out;	/**< next obyte should be put there */
	unsigned int	avail_out;	/**< remaining free space at next_out*/
	unsigned long	total_out;	/**< total nb of bytes output so far */

	uint32_t	crc32;		/**< data crc32 */
	uint32_t	adler32;	/**< data adler32 */

	/*
	 * PRIVATE AREA
	 *
	 * The definitions below are not intended for normal use. We
	 * have them at the moment here, because we liked to dump some
	 * internals for problem determination and where too lazy to
	 * hide them and add access functions. When moving towards a
	 * potential Linux product this might change.
	 */

	/* Hardware request specific data */
	void *device;			/**< ref to compr/decompr device */
	struct ddcb_cmd cmd;		/* RETC/ATTN/PROGRESS */
	uint16_t retc;			/**< after DDCB processing */
	uint16_t attn;			/**< after DDCB processing */
	uint32_t progress;		/**< after DDCB processing  */

	/* Parameters for supported formats */
	int format;			/**< DEFL, GZIP, ZLIB */
	int flags;			/* control memory handling behavior */

	/* Save & Restore values for successive DDCB exchange */
	struct zedc_fifo out_fifo;	/* FIFO for output data e.g. hdrs */
	struct zedc_fifo in_fifo;	/* FIFO for read data e.g. hdrs */
	head_state	header_state;	/* State when decoding Header */
	uint16_t	gzip_hcrc;	/* The value of the header CRC */
	int	gzip_header_idx;	/* Index need for getting header data */

	/* Incomplete output data */
	int onumbits;			/* remaining bits 0..7 */
	uint8_t obyte;			/* incomplete byte */

	/* Status bits */
	int		eob_seen;	/* inflate: EOB seen */
	int		eob_added;	/* deflate: EOB added */
	int		header_added;	/* deflate: header was added */
	int		trailer_added;	/* deflate: trailer was added */
	int havedict;			/* inflate/deflate: have dictionary */

	/* temporary workspace (dict, tree, scratch) */
	struct zedc_wsp *wsp;		/* workspace for deflate and inflate */
	int		wsp_page;	/**< toggeling workspace page */
	enum zedc_mtype dma_type[3];    /* dma types for in, out, ws */

	/* GZIP/ZLIB specific parameters */
	uint32_t	file_size;	/**< GZIP input file size */
	uint32_t	file_adler32;	/**< checksum from GZIP Trailer */
	uint32_t	file_crc32;	/**< checksum from ZLIB Trailer */
	uint32_t	dict_adler32;   /* expected adler32 for the dict */
	struct gzedc_header_s *gzip_head;	/* for GZIP only */

	/* scratch and tree management */
	/* ASIV to DDCB */
	uint32_t	in_hdr_scratch_len; /**< to DDCB */
	uint16_t	in_hdr_bits;	/**< next valid HDR/TREE */
	uint8_t		hdr_ib;		/**< to DDCB   */
	uint8_t		scratch_ib;	/**< ignored bits in scratch */

	/* ASV from DDCB */
	uint32_t	inp_processed;
	uint32_t	outp_returned;
	uint8_t		proc_bits;

#define INFL_STAT_PASSED_EOB     0x01
#define INFL_STAT_FINAL_EOB      0x04
#define INFL_STAT_REACHED_EOB    0x08
#define INFL_STAT_HDR_TYPE_MASK  0x60
#define INFL_STAT_HDR_BFINAL     0x80

	uint8_t		infl_stat;      /* 0x01: EOB passed
					 * 0x04: FINAL_EOB reached
					 * 0x60: ...?
					 * 0x08: exactly on eob
					 * 0x80: was final block? */
	uint32_t	hdr_start;	/**< offset in input buffer */
	uint16_t	out_hdr_bits;	/**< from DDCB */
	uint8_t		out_hdr_start_bits; /**< from DDCB */
	uint16_t	copyblock_len;

	/* SR variables */
	uint32_t	tree_bits;	/**< valid bits in tree area    */
	uint32_t	pad_bits;	/**< padding bits behind tree   */
	uint32_t	scratch_bits;	/**< valid bits in scratch area */
	uint64_t	pre_scratch_bits; /**< scratch part of inp_processed */
	uint32_t	inp_data_offs;	/**< processed bytes from inp-buffer */
	uint32_t	in_data_used;

	/* dictionary management */
	uint16_t	dict_len;	/**< previous dictionary length */
	uint8_t		out_dict_offs;	/**< add to INPUT_DICT address */
	uint16_t	obytes_in_dict;

	/* FIXME Replace those special purpose buffers with FIFOs */
	int		prefx_len;	/**< GZIP/ZLIB prefix length */
	int		prefx_idx;	/**< GZIP/ZLIB prefix index  */
	uint8_t		prefx[ZEDC_FORMAT_STORAGE];
	uint16_t	xlen;

	int		postfx_len;	/**< GZIP/ZLIB postfix length */
	int		postfx_idx;	/**< GZIP/ZLIB postfix index  */
	uint8_t		postfx[ZEDC_FORMAT_STORAGE];
} zedc_stream;

typedef struct zedc_stream_s *zedc_streamp;


/****************************************************************************
 * Compression/Decompression device - zedc device handle
 ***************************************************************************/

typedef struct zedc_dev_t *zedc_handle_t;

zedc_handle_t zedc_open(int card_no, int card_type, int mode, int *err_code);
int zedc_close(zedc_handle_t zedc);

void zedc_overwrite_slu_id(zedc_handle_t zedc, uint64_t slu_id);
void zedc_overwrite_app_id(zedc_handle_t zedc, uint64_t app_id);

/**
 * @brief Prepare buffer to do DMA transactions. The driver will
 * create DMA mappings for this buffer and will allocate memory to
 * hold and sglist which describes the buffer. When executing DDCBs
 * the driver will use the cached entry before it tries to dynamically
 * allocate a new one. The intend is to speed up performance. The
 * resources are freed on device close or when calling the unpin
 * function.
 *
 * @param [in] card      card handle
 * @param [in] addr      user space address of memory buffer
 * @param [in] size      size of user space memory buffer
 * @param [in] direction 0: read/1: read and write
 * @return	         DDCB_LIB_OK on success or negative error code.
 */
int zedc_pin_memory(zedc_handle_t zedc, const void *addr, size_t size,
		    int dir);

/**
 * @brief Remove the pinning and free the dma-addresess within the driver.
 *
 * @param [in] card      card handle
 * @param [in] addr      user space address of memory buffer
 * @param [in] size      size of user space memory buffer or use 0 if you
 *                       don't know the size.
 * @return	         DDCB_LIB_OK on success or negative error code.
 */
int zedc_unpin_memory(zedc_handle_t zedc, const void *addr, size_t size);

/* DMA memory allocation/deallocation */
void *zedc_memalign(zedc_handle_t zedc, size_t size, enum zedc_mtype mtype);
int  zedc_free(zedc_handle_t zedc, void *ptr, size_t size,
	       enum zedc_mtype mtype);

/* Error Handling and Information */
int  zedc_pstatus(struct zedc_stream_s *strm, const char *task);
int  zedc_clearerr(zedc_handle_t zedc);
const char *zedc_strerror(int errnum);

/**
 * Retrieve error information from low-level components: libzedc,
 * libcard, libc. During library execution it might happen that we get
 * errors from multiple sources e.g. libcard or libc. It may also
 * happen that we notice an error when interpreting the data we got
 * from libcard, even if the DDCB was executed successfully.
 * E.g. when the data returned in the DDCB was inconsistent and the
 * hardware did not notice it because it had a bug itself. Another
 * case might be if there are programming errors in the data
 * interpretation itself leading to inconsistent state in our stream
 * data structure. Same happens if errors are induced for testing
 * purposes.
 *
 * Use card_strerror(errnum) to print the corresponding error message.
 */
int zedc_carderr(zedc_handle_t zedc);

/**
 * Use this to get a detailed description e.g. in case of
 * ZLIB_STREAM_ERROR or ZLIB_DATA_ERROR. In those cases the library
 * will return the simplified error codes due to compatibility
 * requirements with libz. Nevertheless the user can use this or the
 * function above to figure out a more detailed error cause.
 *
 * Use zedc_strerror(errnum) to print the corresponding error message.
 */
int zedc_liberr(zedc_handle_t zedc);

struct ddcb_cmd   *zedc_last_cmd(struct zedc_stream_s *strm);

/****************************************************************************
 * Compression
 ***************************************************************************/

int zedc_deflateInit2(zedc_streamp strm,
		       int level,
		       int method,
		       int windowBits,
		       int memLevel,
		       int strategy);

int zedc_deflateParams(zedc_streamp strm, int level, int strategy);
int zedc_deflateReset(zedc_streamp strm);
int zedc_deflateSetDictionary(zedc_streamp strm,
			      const uint8_t *dictionary,
			      unsigned int dictLength);
int zedc_deflatePrime(zedc_streamp strm, int bits, int value);
int zedc_deflateCopy(zedc_streamp dest, zedc_streamp source);
int zedc_deflatePending(zedc_streamp strm, unsigned *pending, int *bits);

int zedc_deflate(zedc_streamp strm, int flush);
int zedc_deflateEnd(zedc_streamp strm);

int zedc_deflateSetHeader(zedc_streamp strm, gzedc_headerp head);

/****************************************************************************
 * Decompression
 ***************************************************************************/

int zedc_inflateInit2(zedc_streamp strm, int windowBits);

int zedc_inflateReset(zedc_streamp strm);
int zedc_inflateReset2(zedc_streamp strm, int windowBits);
int zedc_inflateSetDictionary(zedc_streamp strm,
			      const uint8_t *dictionary,
			      unsigned int dictLength);
int zedc_inflateGetDictionary(zedc_streamp strm,
			      uint8_t *dictionary,
			      unsigned int *dictLength);

int zedc_inflatePrime(zedc_streamp strm, int bits, int value);
int zedc_inflateSync(zedc_streamp strm);

int zedc_inflate(zedc_streamp strm, int flush);
int zedc_inflateEnd(zedc_streamp strm);

int zedc_inflateGetHeader(zedc_streamp strm, gzedc_headerp head);

/** miscellaneous */
int zedc_inflateSaveBuffers(zedc_streamp strm, const char *prefix);
void zedc_lib_debug(int onoff);	/* debug outputs on/off */
void zedc_set_logfile(FILE *logfile);

int zedc_inflate_pending_output(struct zedc_stream_s *strm);
int zedc_read_pending_output(struct zedc_stream_s *strm,
			uint8_t *buf, unsigned int len);

/**
 * The application can compare zedc_Version and ZEDC_VERSION for
 * consistency. This check is automatically made by zedc_deflateInit
 * and zedc_inflateInit.
 */
const char *zedc_Version(void);

#ifdef __cplusplus
}
#endif

#endif	/* __LIBZEDC_H__ */
