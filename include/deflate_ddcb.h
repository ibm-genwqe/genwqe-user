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

#ifndef __ZEDC_DDCB_H__
#define __ZEDC_DDCB_H__

/*
 * Description of the zEDC DDCB format for inflate and deflate.  Each
 * DDCB references DMA memory for input, output and workspace.  The
 * Driver takes care to replace the original user-space addresses in
 * to DMA addresses to raw memory or to create scatter-gather-lists to
 * describe the referenced memory.
 */

#include <stdint.h>
#include <asm/ioctl.h>
#include <stddef.h>
#include <linux/genwqe/genwqe_card.h>

#ifdef __cplusplus
extern "C" {
#endif

 /**< DDCB commands */
#define ZEDC_CMD_INFLATE		0x01
#define ZEDC_CMD_DEFLATE		0x02

/**< DEFLATE command options */
#define DDCB_OPT_DEFL_IBUF_INDIR	(1 <<  0)
#define DDCB_OPT_DEFL_OBUF_INDIR	(1 <<  1)
#define DDCB_OPT_DEFL_SAVE_DICT		(1 <<  2)
#define DDCB_OPT_DEFL_STATE_PROVIDED	(1 <<  3)
#define DDCB_OPT_DEFL_SAVE_STATE	(1 <<  4)
#define DDCB_OPT_DEFL_START_BLOCK	(1 <<  5)
#define DDCB_OPT_DEFL_END_BLOCK		(1 <<  6)
#define DDCB_OPT_DEFL_RAS_CHECK		(1 << 15)

/**< INFLATE command options */
#define DDCB_OPT_INFL_IBUF_INDIR	(1 <<  0)
#define DDCB_OPT_INFL_OBUF_INDIR	(1 <<  1)
#define DDCB_OPT_INFL_SAVE_DICT		(1 <<  2)
#define DDCB_OPT_INFL_STATE_PROVIDED	(1 <<  3)
#define DDCB_OPT_INFL_SAVE_STATE	(1 <<  4)
#define DDCB_OPT_INFL_STOP_BLOCK	(1 <<  5)
#define DDCB_OPT_INFL_STOP_TREE		(1 <<  6)
#define DDCB_OPT_INFL_RAS_CHECK		(1 << 15)

/**
 * Workspace for deflate:
 * +-----------------------++-----------------------++
 * |       32KiB dict      ||       32KiB dict      ||
 * | + 16 bytes shift area || + 16 bytes shift area ||
 * +-----------------------++-----------------------++
 * |     wspace_page 0      |     wspace_page 1      |
 *
 * Workspace for inflate:   FIXME Currently no padding FIXME 480 and not 496
 * +-----------------------++-----------------------++----------+----------+
 * |       32KiB dict      ||       32KiB dict      || 496 byte | 512 byte |
 * | + 16 bytes shift area || + 16 bytes shift area || padding  |    hdr   |
 * +-----------------------++-----------------------++----------+----------+
 * |     wspace_page 0      |     wspace_page 1      |
 */

/*
 * FIXME Are the definitions OK? Is the 0x8000 + 0x8000 OK? Are the
 * 496 padding bytes ok?
 */

/**< Additional Parameters */
#define	ZEDC_DICT_LEN			(0x8000 + 16)	/* 32kb + 16 */
#define	ZEDC_TREE_LEN			(0x0200)	/* real: <= 288(dec) */
#define ZEDC_DEFL_WORKSPACE_SIZE	(2 * ZEDC_DICT_LEN)

#define ZEDC_INFL_TREE_START		(0x8000 + 0x8000 + ZEDC_TREE_LEN)
#define ZEDC_INFL_WORKSPACE_SIZE	(ZEDC_INFL_TREE_START + ZEDC_TREE_LEN)

/*
 * Worksspace definition for inflate and deflate.
 */
struct zedc_wsp {
	uint8_t dict[2][ZEDC_DICT_LEN];	/* two dicts + extra bytes */
	uint8_t tree[ZEDC_TREE_LEN];	/* FIXME should be 512 byte aligned */
};

#define ZEDC_ONUMBYTES_v0		(23) /* 0xa0 ... 0xb6 */
#define ZEDC_ONUMBYTES_v1		(24) /* 0xa0 ... 0xb7 */
#define ZEDC_ONUMBYTES_EXTRA		(7)  /* 0xb9 ... 0xbf  */
#define ZEDC_INFL_AVAIL_IN_MAX		(0xffffffff - 1023) /* 4GiB - 1KiB */

/**
 * Application specific invariant part of the DDCB (104 bytes: 0x18...0x7f)
 * see ZEDC Data Compression HLD spec 0.90: 5.3 Application DDCB Fields.
 */

/* ASIV specific part for compression (deflate) */
/* DDCB range: 0x18 ... 0x7f */
struct zedc_asiv_infl {
	uint64_t in_buff;			/**< 0x20 inp buff DMA addr */
	uint32_t in_buff_len;			/**< 0x28 inp buff length */
	uint32_t in_crc32;			/**> 0x2C inp buff CRC32 */

	uint64_t out_buff;			/**< 0x30 outp buf DMA addr */
	uint32_t out_buff_len;			/**< 0x38 outp buf length */
	uint32_t in_adler32;			/**> 0x3C inp buff ADLER32 */

	uint64_t in_dict;			/**< 0x40 inp dict DMA addr. */
	uint32_t in_dict_len;			/**< 0x48 inp dict length */
	uint32_t rsvd_0;			/**< 0x4C reserved */

	uint64_t inp_scratch;			/**< 0x50 inp hdr/scr DMA addr */
	uint32_t in_scratch_len;		/**< 0x58 total used */
	uint16_t in_hdr_bits;			/**< 0x5C */
	uint8_t  hdr_ib;			/**< 0x5E */
	uint8_t  scratch_ib;			/**< 0x5F */

	uint64_t out_dict;			/**< 0x60 outp dict DMA addr */
	uint32_t out_dict_len;			/**< 0x68 outp dict length */
	uint32_t rsvd_1;			/**< 0x6C reserved */

	uint64_t rsvd_2;			/**< 0x70 reserved */
	uint64_t rsvd_3;			/**< 0x78 reserved */
} __attribute__((__packed__)) __attribute__((__may_alias__));

#define INFL_STAT_PASSED_EOB	0x01
#define INFL_STAT_RESERVED1	0x02
#define INFL_STAT_FINAL_EOB	0x04
#define INFL_STAT_REACHED_EOB	0x08
#define INFL_STAT_RESERVED2	0x10
#define INFL_STAT_HDR_TYPE1	0x20	/* Bit 5, see spec */
#define INFL_STAT_HDR_TYPE2	0x40	/* Bit 6, see spec */
#define INFL_STAT_HDR_TYPE	(INFL_STAT_HDR_TYPE1 | INFL_STAT_HDR_TYPE2)
#define INFL_STAT_HDR_BFINAL	0x80


/* DDCB range: 0x80 ... 0xbf */
struct zedc_asv_infl {
	uint16_t out_dict_used;			/**> 0x80  */
	uint16_t copyblock_len;			/**> 0x82  */
	uint8_t  rsvd_84;			/**> 0x84  */
	uint8_t  infl_stat;			/**> 0x85  */
	uint8_t  rsvd_86;			/**> 0x86  */
	uint8_t  proc_bits;			/**> 0x87  */

	uint32_t hdr_start;			/**> 0x88  */
	uint8_t  rsvd_8c;			/**> 0x8c  */
	uint8_t  hdr_start_bits;		/**> 0x8d  */
	uint16_t out_hdr_bits;			/**> 0x8e  */

	uint32_t out_crc32;			/**< 0x90 */
	uint32_t out_adler32;			/**< 0x94 */

	uint32_t inp_processed;			/**< 0x98 */
	uint32_t outp_returned;			/**< 0x9c */

	uint64_t rsvd_a0[3];			/**> 0xa0, 0xa8, 0xb0 */

	uint8_t  out_dict_offs;			/**> 0xb8  */
	uint8_t  rsvd_b9;			/**> 0xb9  */
	uint16_t obytes_in_dict;		/**< 0xba  */
	uint16_t rsvd_bc;			/**< 0xbc  */
	uint16_t rsvd_be;			/**< 0xbe ... 0xbf */
}__attribute__((__packed__)) __attribute__((__may_alias__));

/* ASIV specific part for compression (deflate) */
/* DDCB range: 0x20 ... 0x7f */
struct zedc_asiv_defl {
	uint64_t in_buff;			/**< 0x20 inp buff DMA addr */
	uint32_t in_buff_len;			/**< 0x28 inp buff length */
	uint32_t in_crc32;			/**< 0x2C inp buff CRC32 */

	uint64_t out_buff;			/**< 0x30 outp buff DMA addr */
	uint32_t out_buff_len;			/**< 0x38 outp buff length */
	uint32_t in_adler32;			/**< 0x3C inp buff ADLER32 */

	uint64_t in_dict;			/**< 0x40 inp dict DMA addr */
	uint32_t in_dict_len;			/**< 0x48 inp dict length */
	uint32_t rsvd_0;			/**< 0x4C reserved */

	uint64_t rsvd_1;			/**< 0x50 reserved */
	uint64_t rsvd_2;			/**< 0x58 reserved */

	uint64_t out_dict;			/**< 0x60 outp dict DMA addr */
	uint32_t out_dict_len;			/**< 0x68 outp dict length */
	uint32_t rsvd_3;			/**< 0x6C reserved */

	uint64_t rsvd_4;			/**< 0x70 reserved */

	uint8_t ibits[7];			/**< 0x78 partial symbol */
	uint8_t inumbits;			/**< 0x7f valid bits (ibits) */
} __attribute__((__packed__)) __attribute__((__may_alias__));

/* ASV DDCB range: 0x80 ... 0xbf */
struct zedc_asv_defl {				/* for deflate */
	uint16_t out_dict_used;			/**< 0x80 */
	uint8_t  resrv_1[5];			/**< 0x82 */
	uint8_t  onumbits;			/**< 0x87 */
	uint64_t resrv_2;			/**< 0x88 */

	uint32_t out_crc32;			/**< 0x90 */
	uint32_t out_adler32;			/**< 0x94 */
	uint32_t inp_processed;			/**< 0x98 */
	uint32_t outp_returned;			/**< 0x9c */

	uint8_t	 obits[ZEDC_ONUMBYTES_v1];	/**< 0xa0 ... 0xb7 */
	uint8_t	 out_dict_offs;			/**< 0xb8 */
	uint8_t  obits_extra[ZEDC_ONUMBYTES_EXTRA]; /**< 0xb9 ... 0xbf */
} __attribute__((__packed__)) __attribute__((__may_alias__));

#ifdef __cplusplus
}
#endif

#endif /* __ZEDC_DDCB_H__ */
