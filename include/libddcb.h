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

#ifndef __LIBDDCB_H__
#define __LIBDDCB_H__

/**
 * IBM DDCB based Accelerator Family
 *
 * There will be two types of PCIe cards supporting DDCBs. The 1st one
 * is using the plain PCIe protocol and using the GenWQE Linux device
 * driver to communicate to user code. This works for Intel, z and p
 * and potentially for other architectures too.
 *
 * The 2nd type is using the CAPI protocol on top of PCIe and is only
 * available for IBM System p.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <linux/types.h>

/*****************************************************************************/
/** Version Information and Error Codes					     */
/*****************************************************************************/

#define DDCB_TYPE_GENWQE		0x0000
#define DDCB_TYPE_CAPI			0x0002

#define ACCEL_REDUNDANT			-1 /* special: redundant card */

#define DDCB_MODE_RD			0x0001 /* NOTE: Needs to match
						  GENWQE_MODE flags */
#define DDCB_MODE_WR			0x0002 /* ... */
#define DDCB_MODE_RDWR			0x0004 /* ... */
#define DDCB_MODE_ASYNC			0x0008 /* ... */
#define DDCB_MODE_NONBLOCK		0x0010 /* non blocking, -EBUSY */
#define DDCB_MODE_POLLING		0x0020 /* polling */
#define DDCB_MODE_MASTER		0x08000000
	/* Open Master Context, Slave is default, CAPI ony */

#define DDCB_APPL_ID_IGNORE		0x0000000000000000ull /* Ignore bits */
#define DDCB_APPL_ID_MASK		0x00000000ffffffffull /* Valid bits */
#define DDCB_APPL_ID_MASK_VER		0x000000ffffffffffull /* Valid bits */

#define DDCB_OK				0
#define DDCB_ERRNO			-401 /* libc call went wrong */
#define DDCB_ERR_CARD			-402 /* problems accessing accel. */
#define DDCB_ERR_OPEN			-403 /* cannot open accelerator */
#define DDCB_ERR_VERS_MISMATCH		-404 /* library version mismatch */
#define DDCB_ERR_INVAL			-405 /* illegal parameters */
#define DDCB_ERR_EXEC_DDCB		-411 /* ddcb execution failed */
#define DDCB_ERR_APPID			-414 /* application id wrong */
#define DDCB_ERR_NOTIMPL		-415 /* funct not implemented */
#define DDCB_ERR_ENOMEM			-416
#define DDCB_ERR_ENOENT			-417
#define DDCB_ERR_IRQTIMEOUT		-418
#define DDCB_ERR_EVENTFAIL		-419
#define DDCB_ERR_SELECTFAIL		-420  /* e.g. socket problems in sim */

/* Genwqe chip Units */
#define DDCB_ACFUNC_SLU			0x00  /* chip service layer unit */
#define DDCB_ACFUNC_APP			0x01  /* chip application */

/* DDCB return codes (RETC) */
#define DDCB_RETC_IDLE			0x0000 /* Unexecuted/DDCB created */
#define DDCB_RETC_PENDING		0x0101 /* Pending Execution */
#define DDCB_RETC_COMPLETE		0x0102 /* Cmd complete. No error */
#define DDCB_RETC_FAULT			0x0104 /* App Err, recoverable */
#define DDCB_RETC_ERROR			0x0108 /* App Err, non-recoverable */
#define DDCB_RETC_FORCED_ERROR		0x01ff /* overwritten by driver  */
#define DDCB_RETC_UNEXEC		0x0110 /* Unexe/Removed from queue */
#define DDCB_RETC_TERM			0x0120 /* Terminated */
#define DDCB_RETC_RES0			0x0140 /* Reserved */
#define DDCB_RETC_RES1			0x0180 /* Reserved */

/* Common DDCB Commands */
#define DDCB_CMD_ECHO_SYNC		0x00 /* PF/VF */

/* DDCB Command Options (CMDOPT) */
#define DDCB_OPT_ECHO_FORCE_NO		0x0000 /* ECHO DDCB */
#define DDCB_OPT_ECHO_FORCE_102		0x0001 /* force return code */
#define DDCB_OPT_ECHO_FORCE_104		0x0002
#define DDCB_OPT_ECHO_FORCE_108		0x0003
#define DDCB_OPT_ECHO_FORCE_110		0x0004
#define DDCB_OPT_ECHO_FORCE_120		0x0005
#define DDCB_OPT_ECHO_FORCE_140		0x0006
#define DDCB_OPT_ECHO_FORCE_180		0x0007

#define _DDCB_OPT_ECHO_COPY_NONE	0x00
#define _DDCB_OPT_ECHO_COPY_ALL		0x20

/* Issuing a specific DDCB command */
#define DDCB_LENGTH			256 /* Size of real DDCB */
#define DDCB_ASIV_LENGTH		104 /* Length of the DDCB ASIV array */
#define DDCB_ASIV_LENGTH_ATS		96  /* ASIV in ATS architecture */
#define DDCB_ASV_LENGTH			64  /* Len of the DDCB ASV array  */

/**
 * @brief In case of RETC 0x110 and ATTN 0xE007 the DMA engine reports
 * back its detailed status in the ASV of the DDCB. Fields are defined
 * in big endian byte ordering.
 */
struct _asv_runtime_dma_error {
	uint64_t raddr_be64;		/* 0x80 */

	uint32_t rfmt_chan_disccnt_be32;/* 0x88 */
	uint16_t rdmae_be16;		/* 0x8C */
	uint16_t rsge_be16;		/* 0x8E */

	uint64_t res0;			/* 0x90 */
	uint64_t res1;			/* 0x98 */
	uint64_t waddr_be64;		/* 0xA0 */

	uint32_t wfmt_chan_disccnt_be32;/* 0xA8 */
	uint16_t wdmae_be16;		/* 0xAC */
	uint16_t wsge_be16;		/* 0xAE */

	uint64_t res2;			/* 0xB0 */
	uint64_t res3;			/* 0xB8 */
} __attribute__((__packed__)) __attribute__((__may_alias__));

/**
 * struct genwqe_ddcb_cmd - User parameter for generic DDCB commands
 *
 * General fields are to be passed in host byte endian order. The
 * fields in asv and asiv depend on the accelerator functionality. The
 * compression/decompression accelerator uses e.g. big-endian.
 *
 * NOTE: This interface is matching the GenWQE device driver
 * interface. If it is changed, it needs to be reflected in the code
 * which prepares the request to the GenWQE device driver ioctl.
 *
 * And yes ... it is very close to the DDCB design ...
 */
typedef struct ddcb_cmd {
	__u64 next_addr;		/* chaining ddcb_cmd */
	__u64 flags;			/* reserved */

	__u8  acfunc;			/* accelerators functional unit */
	__u8  cmd;			/* command to execute */
	__u8  asiv_length;		/* used parameter length */
	__u8  asv_length;		/* length of valid return values  */
	__u16 cmdopts;			/* command options */
	__u16 retc;			/* return code from processing    */

	__u16 attn;			/* attention code from processing */
	__u16 vcrc;			/* variant crc16 */
	__u32 progress;			/* progress code from processing  */

	__u64 deque_ts;			/* dequeue time stamp */
	__u64 cmplt_ts;			/* completion time stamp */
	__u64 disp_ts;			/* SW processing start */

	__u64 ddata_addr;		/* collect debug data */

	__u8  asv[DDCB_ASV_LENGTH];	/* command specific values */

	union {
		/* 2nd version of DDCBs has ATS field */
		struct {
			__u64 ats;
			__u8  asiv[DDCB_ASIV_LENGTH_ATS];
		};
		/* 1st version has no ATS field */
		__u8 __asiv[DDCB_ASIV_LENGTH];
	};
} ddcb_cmd_t;

static inline void ddcb_cmd_init(struct ddcb_cmd *cmd)
{
	__u64 tstamp;

	tstamp = cmd->disp_ts;
	memset(cmd, 0, sizeof(*cmd));
	cmd->disp_ts = tstamp;
}

/* Opaque data type defined library internal */
typedef struct card_dev_t *accel_t;

/*****************************************************************************/
/** Function Prototypes							     */
/*****************************************************************************/

/* Error Handling and Information */
const char *ddcb_retc_strerror(int ddcb_retc);  /* DDCBs retc */
const char *ddcb_strerror(int accel_rc);
const char *accel_strerror(accel_t card, int card_rc); /* card errcode */

void ddcb_hexdump(FILE *fp, const void *buff, unsigned int size);
void ddcb_debug(int verbosity);
void ddcb_set_logfile(FILE *fd_out);

/**
 * @brief Get accel_handle
 *
 * @param [in] card_no   card number if positive
 *                       -1 ACCEL_REDUNDANT: Use multiple cards
 *                          if possible, recover problems automatically.
 * @param [in] mode      influence handle behavior
 * @return	         handle on success or NULL (see card_rc)
 */
accel_t accel_open(int card_no, unsigned int card_type, unsigned int mode,
		   int *rc, uint64_t appl_id, uint64_t appl_id_mask);

int accel_close(accel_t card);

/**
 * @brief Genwqe generic DDCB execution interface.
 * The execution request will block until finished or a timeout occurs.
 *
 * @param [in] card      card handle
 * @param [inout] req    DDCB execution request
 * @return	         DDCB_LIB_OK on success or negative error code.
 *                       Please inspect the DDCB specific return code
 *                       in retc, attn and progress in case of error too.
 */
int accel_ddcb_execute(accel_t card, struct ddcb_cmd *req, int *card_rc,
		       int *card_errno);

/* Register access */
uint64_t accel_read_reg64(accel_t card, uint32_t offs, int *card_rc);
uint32_t accel_read_reg32(accel_t card, uint32_t offs, int *card_rc);
int accel_write_reg64(accel_t card, uint32_t offs, uint64_t val);
int accel_write_reg32(accel_t card, uint32_t offs, uint32_t val);
uint64_t accel_get_app_id(accel_t card);

/**
 * @brief Get the queue work timer card ticks. This indicates how long
 * the hardware queue was in use. Comparing this value with the over
 * all runtime, helps to judge how much time was spend in software and
 * in hardware data processing.
 */
uint64_t accel_get_queue_work_time(accel_t card);
uint64_t accel_get_frequency(accel_t card);
void accel_dump_hardware_version(accel_t card, FILE *fp);

/**
 * @brief Prepare buffer to do DMA transactions. The driver will
 * create DMA mappings for this buffer and will allocate memory to
 * hold and sglist which describes the buffer. When executing DDCBs
 * the driver will use the cached entry before it tries to dynamically
 * allocate a new one. The intend is to speed up performance. The
 * resources are freed on device close or when calling the unpin
 * function.
 *
 * Note: Only needed if underlying architecture supports it.
 *
 * @param [in] card      card handle
 * @param [in] addr      user space address of memory buffer
 * @param [in] size      size of user space memory buffer
 * @param [in] direction 0: read/1: read and write
 * @return	         DDCB_LIB_OK on success or negative error code.
 */
int accel_pin_memory(accel_t card, const void *addr, size_t size, int dir);

/**
 * @brief Remove the pinning and free the dma-addresess within the driver.
 *
 * Note: Only needed if underlying architecture supports it.
 *
 * @param [in] card      card handle
 * @param [in] addr      user space address of memory buffer
 * @param [in] size      size of user space memory buffer or use 0 if you
 *                       don't know the size.
 * @return	         DDCB_LIB_OK on success or negative error code.
 */
int accel_unpin_memory(accel_t card, const void *addr, size_t size);

/*
 * Set of functions to alloc/free DMA capable buffers
 *
 * Allocating memory via the GenWQE Linux driver will result in page
 * alinged memory. Since this is a feature, we use memalign to mimic
 * the same for simulation mode. Requesting too large chunks
 * everything larger than one page, might result in not getting the
 * memory. Intel Linux provides 4MiB largest. z Linux in the PCI
 * support partition is configured to return max 1MiB. If a large
 * contignous memory block is available depends on the systems amount
 * of memory, but also on the memory fragmentation state of the
 * system. If larger regions are needed, consider using sglists
 * instead.
 *
 * Memory returned by this function is page aligned but not guaranteed
 * to be zeroed out.
 *
 * Note: Only needed if underlying architecture supports it.
 */
void *accel_malloc(accel_t card, size_t size);
int accel_free(accel_t card, void *ptr, size_t size);

/**
 * Since there are different types of DDCB accelerators out there,
 * e.g. GenWQE PCIe card and its simulation or the new CAPI PCIe
 * implementation with yet a different simulation approach underneath,
 * this interface offers to register functionality for the respective
 * types. The idea is to provide a constructor which registers the
 * interface atat libddcb and tools using it can specify the type of
 * DDCB accelerator they like to use.
 *
 * libddcb will use the registered functions to provide the requested
 * functionality.
 */
#define DDCB_FLAG_STATISTICS 0x0001 /* enable statistical data gathering */

struct ddcb_accel_funcs {
	int card_type;
	const char *card_name;

	/* must return void *card_data */
	void *(* card_open)(int card_no, unsigned int mode, int *card_rc,
			    uint64_t appl_id, uint64_t appl_id_mask);
	int (* card_close)(void *card_data);
	int (* ddcb_execute)(void *card_data, struct ddcb_cmd *req);

	const char * (* card_strerror)(void *card_data, int card_rc);

	/* The following functions we need for all implementation,
	   least for debugging purposes. */
	uint64_t (* card_read_reg64)(void *card_data, uint32_t offs,
				     int *card_rc);
	uint32_t (* card_read_reg32)(void *card_data, uint32_t offs,
				     int *card_rc);
	int (* card_write_reg64)(void *card_data, uint32_t offs,
				 uint64_t val);
	int (* card_write_reg32)(void *card_data, uint32_t offs,
				 uint32_t val);

	/* The application id is something we used for the GenWQE
	   implementation. It helps to ensure that the software can
	   check if it can operatate this accelerator
	   implementation. For CAPI we are searching a similar
	   mechanism still. */
	uint64_t (* card_get_app_id)(void *card_data);
	uint64_t (* card_get_queue_work_time)(void *card_data); /* ticks */
	uint64_t (* card_get_frequency)(void *card_data); /* Hz */
	void (* card_dump_hardware_version)(void *card_data, FILE *fp);

	/* Not all DDCB accelerators have this, GenWQE has it, but
	   CAPI does not. If not executed wrapper functions will
	   return DDCB_OK */
	int (* card_pin_memory)(void *card_data, const void *addr,
				size_t size, int dir);
	int (* card_unpin_memory)(void *card_data, const void *addr,
				  size_t size);
	void * (* card_malloc)(void *card_data, size_t size);
	int (* card_free)(void *card_data, void *ptr, size_t size);

	/* statistical information */
	int (* dump_statistics)(FILE *fp);

	pthread_mutex_t slock;
	unsigned long num_open;
	unsigned long num_execute;
	unsigned long num_close;

	unsigned long time_open;
	unsigned long time_execute;
	unsigned long time_close;

	/* private */
	void *priv_data;
};


/*
 * Dump card statistics for debugging and for performance analysis.
 *
 * @param [in] card      card handle
 * @param [out] fp       filehandle to write the text too
 */
int accel_dump_statistics(struct ddcb_accel_funcs *accel, FILE *fp);


/*
 * Register accelerator for later usage. This needs ideally be done in
 * a library constructor.
 *
 * @param [in] accel     accelerator function table
 */
int ddcb_register_accelerator(struct ddcb_accel_funcs *accel);

#ifdef __cplusplus
}
#endif

#endif	/* __LIBDDCB_H__ */
