/*
 * Copyright 2014,2015 International Business Machines
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

#ifndef __MEMCOPY_DDCB_H__
#define __MEMCOPY_DDCB_H__

#include <stdint.h>
#include <linux/genwqe/genwqe_card.h>

#ifdef __cplusplus
	extern "C" {
#endif

 /**< DDCB commands */
#define ZCOMP_CMD_ZEDC_MEMCOPY		0x03

/* memcopy is available for different APPS/AFUs */
#ifndef GENWQE_APPL_ID_GZIP
#  define GENWQE_APPL_ID_GZIP	0x00000000475a4950 /* The GZIP APPL id */
#endif
#ifndef GENWQE_APPL_ID_GZIP2
#  define GENWQE_APPL_ID_GZIP2	0x00000002475a4950 /* The GZIP 2 APPL id */
#endif

/**
 * application specific invariant part of the DDCB (104 bytes: 0x20...0x7f)
 * see ZCOMP Data Compression HLD spec 0.96: 5.3.3 Memcopy CMD
 */
struct asiv_memcpy {
	uint64_t inp_buff;	/**< 0x20 input buffer address */
	uint32_t inp_buff_len;	/**< 0x28 */
	uint32_t in_crc32;	/**< 0x2c only used for zEDC */

	uint64_t outp_buff;	/**< 0x30 input buffer address */
	uint32_t outp_buff_len;	/**< 0x38 */
	uint32_t in_adler32;	/**< 0x3c only used for zEDC */

	uint64_t res0[4];	/**< 0x40 0x48 0x50 0x58 */
	uint16_t res1;		/**< 0x60 */
	uint16_t input_lists;	/**< 0x62 */
	uint32_t res2;		/**< 0x64 */

	uint64_t res3[3];	/**< 0x68 ... 0x7f */
} __attribute__((__packed__)) __attribute__((__may_alias__));

/**
 * application specific variant part of the DDCB (56 bytes: 0x80...0xb7)
 * see ZCOMP Data Compression HLD spec 0.96: 5.3.3 Memcopy CMD
 */
struct asv_memcpy {
	uint64_t res0[2];	/**< 0x80 ... 0x8f */
	uint32_t out_crc32;	/**< 0x90 only used for zEDC */
	uint32_t out_adler32;	/**< 0x94 only used for zEDC */
	uint32_t inp_processed;	/**< 0x98 */
	uint32_t outp_returned;	/**< 0x9c */
	uint64_t  res1[4];	/**< 0xa0 ... 0xbf */
} __attribute__((__packed__)) __attribute__((__may_alias__));

#ifdef __cplusplus
}
#endif

#endif	/* __MEMCOPY_DDCB_H__ */
