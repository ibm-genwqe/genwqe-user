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

#ifndef __LIBCARD_H__
#define __LIBCARD_H__

/**
 * @file	libcard.h
 * @brief	application library for hardware access
 *
 * The GenWQE PCIe card provides the ability to speed up tasks by
 * offloading data processing. It provides a generic work queue engine
 * (GenWQE) which is used to pass the requests to the PCIe card. The
 * requests are to be passed in form of DDCB commands (Device Driver
 * Control Blocks). The device driver is allocating the next free DDCB
 * from the hardware queue and converts the DDCB-request defined in
 * this file into a DDCB. Once the request is passed to the card, the
 * process/thread will sleep and will be awoken once the request is
 * finished with our without success or a timeout condition occurred.
 *
 * IBM Accelerator Family 'GenWQE'
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>
#include <linux/genwqe/genwqe_card.h>

/*****************************************************************************/
/** Version Information and Error Codes					     */
/*****************************************************************************/

#define GENWQE_LIB_VERS_STRING		"3.0.23"

/**< library error codes */
#define GENWQE_OK			0
#define GENWQE_ERRNO			(-201)
#define GENWQE_ERR_CARD			(-202)
#define GENWQE_ERR_OPEN			(-203)
#define GENWQE_ERR_VERS_MISMATCH	(-204)
#define GENWQE_ERR_INVAL		(-205)
#define GENWQE_ERR_FLASH_VERIFY		(-206)
#define GENWQE_ERR_FLASH_READ		(-207)
#define GENWQE_ERR_FLASH_UPDATE		(-208)
#define GENWQE_ERR_GET_STATE		(-209)
#define GENWQE_ERR_SIM			(-210)
#define GENWQE_ERR_EXEC_DDCB		(-211)
#define GENWQE_ERR_PINNING		(-212)
#define GENWQE_ERR_TESTMODE		(-213)
#define GENWQE_ERR_APPID		(-214)

/*****************************************************************************/
/** Useful macros in case they are not defined somewhere else		     */
/*****************************************************************************/

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(a)  (sizeof((a)) / sizeof((a)[0]))
#endif

#ifndef ABS
#  define ABS(a)	 (((a) < 0) ? -(a) : (a))
#endif

#ifndef MAX
#  define MAX(a,b)	({ __typeof__ (a) _a = (a); \
			   __typeof__ (b) _b = (b); \
			_a > _b ? _a : _b; })
#endif
#ifndef MIN
#  define MIN(a,b)	({ __typeof__ (a) _a = (a); \
			   __typeof__ (b) _b = (b); \
			_a < _b ? _a : _b; })
#endif

/*****************************************************************************/
/** Type definitions							     */
/*****************************************************************************/

#define CARD_DEVICE	("/dev/" GENWQE_DEVNAME "%u_card")

typedef struct card_dev_t *card_handle_t;

/**
 * @brief In case of RETC 0x110 and ATTN 0xE007 the DMA engine reports back
 * its detailed status in the ASV of the DDCB.
 */
struct asv_runtime_dma_error {
	uint64_t raddr_be64;			/* 0x80 */
	uint32_t rfmt_chan_disccnt_be32;	/* 0x88 */
	uint16_t rdmae_be16;			/* 0x8C */
	uint16_t rsge_be16;			/* 0x8E */

	uint64_t res0;				/* 0x90 */
	uint64_t res1;				/* 0x98 */

	uint64_t waddr_be64;			/* 0xA0 */
	uint32_t wfmt_chan_disccnt_be32;	/* 0xA8 */
	uint16_t wdmae_be16;			/* 0xAC */
	uint16_t wsge_be16;			/* 0xAE */

	uint64_t res2;				/* 0xB0 */
	uint64_t res3;				/* 0xB8 */
} __attribute__((__packed__)) __attribute__((__may_alias__));

/*****************************************************************************/
/** Function Prototypes							     */
/*****************************************************************************/

/** Genwqe file operations */
#define GENWQE_CARD_REDUNDANT	-1 /* redundant card support */
#define GENWQE_CARD_SIMULATION	-2 /* use this for simulation */

#define GENWQE_CARD_TESTMODE	0x1000 /* tweak DDCB/sglists before exec */
#define GENWQE_TESTMODE_MASK	0xfff

/**
 * RDONLY: Only reading data from this handle
 * WRONLY: Only write to this handle is possible
 * RDRW:   Both reading and writing is possible
 * ASYNC:  Enable signal driven err notification: SIGIO is delivered
 *	   when the device needs recovery.
 *
 * @note Mode flags can be useful for code which is embedding the
 * card_handle_t within their own structures.
 */
#define GENWQE_MODE_RDONLY	0x0001
#define GENWQE_MODE_WRONLY	0x0002
#define GENWQE_MODE_RDWR	0x0004
#define GENWQE_MODE_ASYNC	0x0008
#define GENWQE_MODE_NONBLOCK	0x0010  /* non blocking operation, -EBUSY */

#define GENWQE_APPL_ID_IGNORE	0x0000000000000000 /* Ignore appl id Bits */
#define GENWQE_APPL_ID_MASK	0x00000000ffffffff /* Valid bits in appid */

/**
 * @brief Get genwqe_card_handle
 *
 * @param [in] card_no   card number if positive
 *                       -1 GENWQE_CARD_REDUNDANT: Use multiple cards
 *                          if possible, recover problems automatically.
 *                       -2 GENWQE_CARD_SIMULATION: Simulation
 * @param [in] mode      For future extensions to influence handle behavior
 * @return	         GENWQE_LIB_OK on success or negative error code.
 */
card_handle_t genwqe_card_open(int card_no, int mode, int *err_code,
			       uint64_t appl_id, uint64_t appl_id_mask);
int  genwqe_card_close(card_handle_t card);

/* Error Handling and Information */
const char *card_strerror(int errnum);
const char *retc_strerror(int retc);
void genwqe_card_lib_debug(int onoff);

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
 * @return	         GENWQE_LIB_OK on success or negative error code.
 */
int genwqe_pin_memory(card_handle_t card, const void *addr, size_t size,
		      int dir);

/**
 * @brief Remove the pinning and free the dma-addresess within the driver.
 *
 * @param [in] card      card handle
 * @param [in] addr      user space address of memory buffer
 * @param [in] size      size of user space memory buffer or use 0 if you
 *                       don't know the size.
 * @return	         GENWQE_LIB_OK on success or negative error code.
 */
int genwqe_unpin_memory(card_handle_t card, const void *addr, size_t size);

static inline void genwqe_ddcb_cmd_init(struct genwqe_ddcb_cmd *cmd)
{
	__u64 tstamp;

	tstamp = cmd->disp_ts;
	memset(cmd, 0, sizeof(*cmd));
	cmd->disp_ts = tstamp;
}

/**
 * Super Child Block allocation/deallocation
 *
 * The SCB is build up as follows:
 *   ATS[n]  - Address Translation Specification
 *   DATA    - Data or pointers according to ATS[n] information
 *
 * Each 4 bit field in the ATS area of the SCB describes 8 bytes of
 * the SCB/data.
 *
 * Example: When using one 8 byte ATS entry (and the minimum is 8
 * bytes for ATS fields), we have 16 4-bit nibbles describing 16 * 8
 * bytes of the SCB. That results in an SCB of 128 bytes size, where 8
 * bytes are used as ATS bitfields. And the first nibble of the ATS
 * bitfield needs to be 0b0000 to reserve the space for the ATS 8 byte
 * entry itself. The remaining nibbles in the ATS area are up to the
 * application.
 *
 * As result the size of the SCB needs to be a multiple of 128 bytes.
 * And the usable data starts after the ATS area which is used to
 * describe the SCB itself.
 *
 * Our first implementation is limited such that we need to align the
 * memory for the SCB to a 4KiB boundary.
 */
void *genwqe_card_alloc_scb(card_handle_t card, size_t size);
int  genwqe_card_set_ats_flags(void *scb, size_t size, size_t offs, int type);
int  genwqe_card_free_scb(card_handle_t card, void *scb, size_t size);

/**
 * @brief Genwqe generic DDCB execution interface.
 * The execution request will block until finished or a timeout occurs.
 *
 * @param [in] card      card handle
 * @param [inout] req    DDCB execution request
 * @return	         GENWQE_LIB_OK on success or negative error code.
 *                       Please inspect the DDCB specific return code
 *                       in retc, attn and progress in case of error too.
 */
int genwqe_card_execute_ddcb(card_handle_t card, struct genwqe_ddcb_cmd *req);

/**
 * @brief	Execute a DDCB request with no DMA buffer translations.
 * @param [in] card	 card handle
 * @param [inout] req	 DDCB execution request
 * @return	         GENWQE_LIB_OK on success or negative error code.
 *                       Please inspect the DDCB specific return code
 *                       in retc, attn and progress in case of error too.
 */
int genwqe_card_execute_raw_ddcb(card_handle_t card,
				 struct genwqe_ddcb_cmd *req);

/** Genwqe register access */
uint64_t genwqe_card_read_reg64(card_handle_t card, uint32_t offs, int *rc);
uint32_t genwqe_card_read_reg32(card_handle_t card, uint32_t offs, int *rc);
int  genwqe_card_write_reg64(card_handle_t card, uint32_t offs, uint64_t v);
int  genwqe_card_write_reg32(card_handle_t card, uint32_t offs, uint32_t v);

int  genwqe_card_get_state(card_handle_t card, enum genwqe_card_state *state);
uint32_t genwqe_ddcb_crc32(uint8_t *buff, size_t len, uint32_t init);

/**
 * Service Layer Architecture (firmware) layer
 *  0x00: Development mode/Genwqe4-WFO (defunct)
 *  0x01: SLC1 (a5-wfo)
 *  0x02: SLC2 (sept2012), zcomp, zdb2, single DDCB,
 *  0x03: SLC2 (feb2013), zcomp, zdb2, generic driver, single DDCB
 *  0xFF: Bad Image.
 */
#define GENWQE_SLU_DEVEL   0x00
#define GENWQE_SLU_SLC1    0x01
#define GENWQE_SLU_SLC2_0  0x02
#define GENWQE_SLU_SLC2_1  0x03
#define GENWQE_SLU_BAD     0xff

/**
 * @brief Get filedescriptor associated with card.
 * @param [in] card      card handle
 * @return	         filedescriptor or -1 on error.
 */
int genwqe_card_fileno(card_handle_t card);

int genwqe_get_drv_rc(card_handle_t card);
int genwqe_get_drv_errno(card_handle_t card);

/**
 * @brief Debug support.
 */
void genwqe_card_lib_debug(int onoff); /* debug outputs on/off */
void genwqe_hexdump(FILE *fp, const void *buff, unsigned int size);

	/* Flags which information should be printed out */
#define GENWQE_DD_IDS			0x0001
#define GENWQE_DD_DDCB_BEFORE		0x0002
#define GENWQE_DD_DDCB_PREVIOUS		0x0004
#define GENWQE_DD_DDCB_PROCESSED	0x0008
#define GENWQE_DD_ALL			(GENWQE_DD_IDS		 |	\
					 GENWQE_DD_DDCB_BEFORE	 |	\
					 GENWQE_DD_DDCB_PREVIOUS |	\
					 GENWQE_DD_DDCB_PROCESSED)

void genwqe_print_debug_data(FILE *fp, struct genwqe_debug_data *debug_data,
			     int flags);

/*
 * Set of functions to alloc/free DMA capable buffers
 *
 * Allocating memory via the driver will always result in page alinged
 * memory. Since this is a feature, we use memalign to mimic the same
 * for simulation mode. Requesting too large chunks everything larger
 * than one page, might result in not getting the memory. Intel Linux
 * provides 4MiB largest. z Linux in the PCI support partition is
 * configured to return max 1MiB. If a large contignous memory block
 * is available depends on the systems amount of memory, but also on
 * the memory fragmentation state of the system. If larger regions are
 * needed, consider using sglists instead.
 *
 * Memory returned by this function is page aligned but not guaranteed
 * to be zeroed out.
 */
void *genwqe_card_malloc(card_handle_t card, size_t size);
int  genwqe_card_free(card_handle_t card, void *ptr, size_t size);

/*****************************************************************************/
/** Service related Functions						     */
/*****************************************************************************/

/*****************************************************************************/
/** move flash / update chip						     */
/*****************************************************************************/

struct card_upd_params {
	const char *fname;	/**< path and name of update file */
	uint32_t flength;	/**< length of update file	  */
	uint32_t crc;		/**< crc of this image		  */
	uint16_t flags;		/**< flags from MoveFlash tool	  */
	char partition;		/**< target partition in flash    */

	uint64_t slu_id;	/**< informational/sim: SluID     */
	uint64_t app_id;	/**< informational/sim: AppID     */

	uint16_t retc;
	uint16_t attn;		/**< attention code from processing */
	uint32_t progress;	/**< progress code from processing  */
};

/**
 * @brief Update chip code image. Note that the system must be rebooted
 *	  after using this function if you want to activate the changes.
 *
 * @param [in] card    card handle
 * @param [in] upd     struct containing all params for update process
 * @param [in] verify  verify content by reading it back and comparing
 * @return	       SLU_LIB_OK on success or error code.
 */
int genwqe_flash_update(card_handle_t card, struct card_upd_params *upd,
			int verify);

/**
 * @brief Read chip code image.
 *
 * @param [in] card    card handle
 * @param [in] upd     struct containing all params for read process
 * @return	       SLU_LIB_OK on success or error code.
 */
int genwqe_flash_read(card_handle_t card, struct card_upd_params *upd);

/**
 * Original VPD layout by Nallatech. This is normally stored in the
 * cards CPLD chip.
 */
typedef struct genwqe_vpd {
	uint8_t csv_vpd_data[512];	/* New defined by CSV file */
} __attribute__((__packed__)) __attribute__((__may_alias__)) genwqe_vpd;

int genwqe_read_vpd(card_handle_t card, genwqe_vpd *vpd);
int genwqe_write_vpd(card_handle_t card, const genwqe_vpd *vpd);

void card_overwrite_slu_id(card_handle_t card, uint64_t slu_id);
void card_overwrite_app_id(card_handle_t card, uint64_t app_id);
uint64_t card_get_app_id(card_handle_t card);

int genwqe_dump_statistics(FILE *fp);

#ifdef __cplusplus
}
#endif

#endif	/* __LIBCARD_H__ */
