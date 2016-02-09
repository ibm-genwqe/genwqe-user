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

#ifndef __DDCB_H__
#define __DDCB_H__

#include <stdint.h>
#include <asm/byteorder.h>
#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SHI: Software to Hardware Interlock
 *   This 1 byte field is written by software to interlock the
 *   movement of one queue entry to another with the hardware in the
 *   chip.
 */
#define DDCB_SHI_INTR		0x04 /* Bit 2 */
#define DDCB_SHI_PURGE		0x02 /* Bit 1 */
#define DDCB_SHI_NEXT		0x01 /* Bit 0 */

/* HSI: Hardware to Software interlock
 * This 1 byte field is written by hardware to interlock the movement
 * of one queue entry to another with the software in the chip.
 */
#define DDCB_HSI_COMPLETED	0x40 /* Bit 6 */
#define DDCB_HSI_FETCHED	0x04 /* Bit 2 */

/**
 * Accessing HSI/SHI is done 32-bit wide
 *   Normally 16-bit access would work too, but on some platforms the
 *   16 compare and swap operation is not supported. Therefore
 *   switching to 32-bit such that those platforms will work too.
 *
 *                                         iCRC HSI/SHI
 */
#define DDCB_INTR_BE32		__cpu_to_be32(0x00000004)
#define DDCB_PURGE_BE32		__cpu_to_be32(0x00000002)
#define DDCB_NEXT_BE32		__cpu_to_be32(0x00000001)
#define DDCB_COMPLETED_BE32	__cpu_to_be32(0x00004000)
#define DDCB_FETCHED_BE32	__cpu_to_be32(0x00000400)

/* CRC polynomials for DDCB */
#define CRC16_POLYNOMIAL	0x1021

/* Definitions of DDCB presets */
#define DDCB_PRESET_PRE		0x80
#define ICRC_LENGTH(n)		((n) + 8 + 8 + 8)  /* used ASIV + hdr fields */
#define VCRC_LENGTH(n)		((n))		   /* used ASV */

#define ASIV_LENGTH		(0x80 - 0x18) /* 104 */
#define ASIV_LENGTH_ATS		(0x80 - 0x20) /* 96 */
#define ASV_LENGTH		(0xc0 - 0x80) /* 64 */

/* Interlock flags */
#define HSI_COMPLETED		0x40
#define HSI_FETCHED		0x04
#define SHI_NEXT		0x01
#define SHI_PURGE		0x02
#define SHI_INTR		0x04

/**
 * The fields are defined to be in big endian format.
 */
struct ddcb_t {
	union {
		__be32 icrc_hsi_shi_32; /**< CRC HW to SW/SW to HW Interlk */
		struct {
			__be16 icrc_16;
			uint8_t  hsi;
			uint8_t  shi;
		};
	};
	uint8_t pre;		/**< Preamble */
	uint8_t xdir;		/**< Execution Directives */
	__be16 seqnum;		/**< Sequence Number */

	uint8_t acfunc;		/**< Accelerator Function.. */
	uint8_t cmd;		/**< Command. */
	__be16 cmdopts_16;	/**< Command Options */
	uint8_t sur;		/**< Status Update Rate */
	uint8_t psp;		/**< Protection Section Pointer */
	__be16 rsvd_0e;		/**< Reserved invariant */

	__be64 fwiv;		/**< Firmware Invariant. */

	union {
		uint8_t __asiv[ASIV_LENGTH]; /**< Appl Spec Invariant */
		struct {
			__be64 ats_64;  /**< Address Translation Spec */
			uint8_t  asiv[ASIV_LENGTH_ATS]; /**< New ASIV */
		} n;
	};
	/* Note: 2nd Cache line starts here. */
	uint8_t	 asv[ASV_LENGTH];   /**< Appl Spec Variant */

	__be16 rsvd_c0;		/**< Reserved Variant */
	__be16 vcrc_16;		/**< Variant CRC */
	__be32 rsvd;		/**< Reserved unprotected */
	__be64 deque_ts_64;	/**< Deque Time Stamp. */
	__be16 retc_16;		/**< Return Code. Note Must be cleared by SW */
	__be16 attn_16;		/**< Attention/Extended Error Codes */
	__be32 progress_32;	/**< Progress indicator. */
	__be64 cmplt_ts_64;	/**< Completion Time Stamp. */
	__be32 ibdc;
	__be32 obdc;
	__be64 rsvd_SLH;	/**< Dispatch TimeStamp */
	uint8_t priv8[8];	/**< Driver usage */
	__be64 disp_ts_64;	/**< Dispatch TimeStamp */
} __attribute__((__packed__));

typedef struct ddcb_t ddcb_t;

#define DDCB_SIZE		sizeof(ddcb_t)

/**
 * @brief	Generate 16-bit crc as required for DDCBs
 *		polynomial = x^16 + x^12 + x^5 + 1   (0x1021)
 *		- example:
 *		  4 bytes 0x01 0x02 0x03 0x04 with init = 0xffff
 *		  should result in a crc16 of 0x89c3
 *
 * @param	buff	pointer to data buffer
 * @param	len	length of data for calculation
 * @param	init	initial crc (0xffff at start)
 *
 * @return	crc16 checksum in big endian format !
 *
 * Example: icrc = ddcb_crc16((const uint8_t *)pddcb,
 *                            ICRC_LENGTH(cmd->asiv_length), 0xffff);
 */
static inline __be16 ddcb_crc16(const uint8_t *buff, size_t len,
				uint16_t init)
{
	int i;
	uint16_t crc = init;

	while (len--) {
		crc = crc ^ (*buff++ << 8);
		for (i = 0; i < 8; i++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ CRC16_POLYNOMIAL;
			else
				crc = crc << 1;
		}
	}
	return crc;
}

#endif	/* __DDCB_H__ */
