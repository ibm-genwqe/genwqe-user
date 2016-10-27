/*
 * Copyright 2015, 2016, International Business Machines
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
 * Specialized DDCB execution implementation.
 *
 * ToDo: Create version which can transparently support multiple cards
 *       - Make sure that the appid is the same ...
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <asm/byteorder.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dirent.h>
#include <unistd.h>

#include <libddcb.h>
#include <ddcb.h>
#include <libcxl.h>
#include "afu_regs.h"

#define CONFIG_DDCB_TIMEOUT	5  /* max time for a DDCB to be executed */
#define	NUM_DDCBS		4  /* DDCB queue length */

extern int libddcb_verbose;
extern FILE *libddcb_fd_out;

#include <sys/syscall.h>   /* For SYS_xxx definitions */

static inline pid_t gettid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

#define VERBOSE0(fmt, ...) do {						\
		if (libddcb_fd_out)					\
			fprintf(libddcb_fd_out, "%08x.%08x: " fmt,	\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE1(fmt, ...) do {						\
		if (libddcb_fd_out && (libddcb_verbose > 0))		\
			fprintf(libddcb_fd_out, "%08x.%08x: " fmt,	\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE2(fmt, ...) do {						\
		if (libddcb_fd_out && (libddcb_verbose > 1))		\
			fprintf(libddcb_fd_out, "%08x.%08x: " fmt,	\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE3(fmt, ...) do {						\
		if (libddcb_fd_out && (libddcb_verbose > 3))		\
			fprintf(libddcb_fd_out, "%08x.%08x: " fmt,	\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

#define __free(ptr) free((ptr))

static void *__ddcb_done_thread(void *card_data);

/**
 * Each CAPI compression card has one AFU, which provides one ddcb
 * queue per process. Multiple threads within one process share the
 * ddcb queue. Locking is needed to ensure that this works race free.
 */
struct ttxs {
	struct	dev_ctx	*ctx;	/* Pointer to Card Context */
	int	compl_code;	/* Completion Code */
	sem_t	wait_sem;
	int	seqnum;		/* Seq Number when done */
	int	card_no;	/* Card number from Open */
	int     card_next;	/* Next card to try in redundant mode */
	unsigned int mode;
	uint64_t app_id;	/* a copy of MMIO_APP_VERSION_REG */
	uint64_t app_id_mask;	/* used when opening the handle */
	struct	ttxs	*verify;
};

/* Thread wait Queue, allocate one entry per ddcb */
enum waitq_status { DDCB_FREE, DDCB_IN, DDCB_OUT, DDCB_ERR };
struct  tx_waitq {
	enum	waitq_status	status;
	struct	ddcb_cmd	*cmd;
	struct	ttxs	*ttx;	/* back Pointer to active ttx */
	int	seqnum;		/* a copy of ddcb_seqnum at start time */
	bool	thread_wait;	/* A thread is waiting to */
	uint64_t q_in_time;	/* Time in msec when i added this ddcb */
};

/**
 * A a device context is normally bound to a card which provides a
 * ddcb queue. Whenever a new context is created a queue is attached
 * to it. Whenever it is removed the queue is removed too. There can
 * be multiple contexts using just one card.
 */
struct dev_ctx {
	ddcb_t *ddcb;			/* ddcb queue */
	struct tx_waitq waitq[NUM_DDCBS];
	unsigned int completed_tasks[NUM_DDCBS+1]; /* used for DDCB_DEBUG=1 */
	unsigned int completed_ddcbs;	/* used for DDCB_DEBUG=1 */
	unsigned int process_irqs;	/* used for DDCB_DEBUG=1 */
	int card_no;			/* Same card number as in ttx */
	unsigned int mode;
	pthread_mutex_t	lock;
	int		clients;	/* Thread open counter */
	struct cxl_afu_h *afu_h;	/* afu_h != NULL device is open */
	int		afu_fd;		/* fd from cxl_afu_fd() */
	int		afu_rc;		/* rc from __afu_open() */
	long cr_device;			/* config record device id */
	long cr_vendor;			/* config record vendor id */
	long api_version_compatible;
	uint16_t	ddcb_seqnum;
	uint16_t	ddcb_free1;	/* Not used */
	unsigned int	ddcb_num;	/* How deep is my ddcb queue */
	int		ddcb_out;	/* ddcb Output (done) index */
	int		ddcb_in;	/* ddcb Input index */
	struct		cxl_event	event;	/* last AFU event */
	int		tout;		/* Timeout Value for compeltion */
	pthread_t	ddcb_done_tid;
	sem_t		open_done_sem;	/* open done */
	uint64_t	app_id;		/* a copy of MMIO_APP_VERSION_REG */
	int		cid_id;		/* cid id from MMIO_DDCBQ_CID_REG */
	sem_t		free_sem;	/* Sem to wait for free ddcb */
	struct		dev_ctx		*verify;	/* Verify field */
};

#define NUM_CARDS 4 /* max number of CAPI cards in system */

static ddcb_t my_ddcbs[NUM_CARDS][NUM_DDCBS] __attribute__((aligned(64*1024)));
static struct dev_ctx my_ctx[NUM_CARDS];

static inline uint64_t get_msec(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	return t.tv_sec * 1000 + t.tv_usec/1000;
}

/*	Add trace function by setting RT_TRACE */
//#define RT_TRACE
#ifdef RT_TRACE
#define	RT_TRACE_SIZE 1000
struct trc_stru {
	uint32_t	tok;
	uint32_t	tid;
	uint32_t	n1;
	uint32_t	n2;
	void	*p;
};

static int trc_idx = 0, trc_wrap = 0;
static struct trc_stru trc_buff[RT_TRACE_SIZE];
static pthread_mutex_t	trc_lock;

static void rt_trace_init(void)
{
	pthread_mutex_init(&trc_lock, NULL);
}

static void rt_trace(uint32_t tok, uint32_t n1, uint32_t n2, void *p)
{
	int	i;

	pthread_mutex_lock(&trc_lock);
	i = trc_idx;
	trc_buff[i].tid = (uint32_t)get_msec();
	trc_buff[i].tok = tok;
	trc_buff[i].n1 = n1;
	trc_buff[i].n2= n2;
	trc_buff[i].p = p;
	i++;
	if (i == RT_TRACE_SIZE) {
		i = 0;
		trc_wrap++;
	}
	trc_idx = i;
	pthread_mutex_unlock(&trc_lock);
}
static void rt_trace_dump(void)
{
	int i;

	pthread_mutex_lock(&trc_lock);
	VERBOSE0("Index: %d Warp: %d\n", trc_idx, trc_wrap);
	for (i = 0; i < RT_TRACE_SIZE; i++) {
		if (0 == trc_buff[i].tok) break;
		VERBOSE0("%03d: %04d : %04x - %04x - %04x - %p\n",
			 i, trc_buff[i].tid, trc_buff[i].tok,
			 trc_buff[i].n1, trc_buff[i].n2, trc_buff[i].p);
	}
	trc_idx = 0;
	pthread_mutex_unlock(&trc_lock);
}
#else
static void rt_trace_init(void) {}
static void rt_trace(uint32_t tok __attribute__((unused)),
			uint32_t n1 __attribute__((unused)),
			uint32_t n2 __attribute__((unused)),
			void *p __attribute__((unused))) {}
static void rt_trace_dump(void) {}

#endif

/*	Command to ddcb */
static inline void cmd_2_ddcb(ddcb_t *pddcb, struct ddcb_cmd *cmd,
			      uint16_t seqnum, bool use_irq)
{
	pddcb->pre = DDCB_PRESET_PRE;
	pddcb->cmdopts_16 = __cpu_to_be16(cmd->cmdopts);
	pddcb->cmd = cmd->cmd;
	pddcb->acfunc = cmd->acfunc;	/* functional unit */
	pddcb->psp = (((cmd->asiv_length / 8) << 4) | ((cmd->asv_length / 8)));
	pddcb->n.ats_64 = __cpu_to_be64(cmd->ats);
	memcpy(&pddcb->n.asiv[0], &cmd->asiv[0], DDCB_ASIV_LENGTH_ATS);
	pddcb->icrc_hsi_shi_32 = __cpu_to_be32(0x00000000); /* for crc */
	/* Write seqnum into reserved area, check for this seqnum is done in ddcb_2_cmd() */
	pddcb->rsvd_0e = __cpu_to_be16(seqnum);

	/* DDCB completion irq */
	if (use_irq)
		pddcb->icrc_hsi_shi_32 |= DDCB_INTR_BE32;

	pddcb->seqnum = __cpu_to_be16(seqnum);
	pddcb->retc_16 = 0;
	if (libddcb_verbose > 3) {
		VERBOSE0("DDCB [%016llx] Seqnum 0x%x before execution:\n",
			(long long)(unsigned long)(void *)pddcb, seqnum);
		ddcb_hexdump(libddcb_fd_out, pddcb, sizeof(ddcb_t));
	}
}

/**
 * Copy DDCB ASV to request struct. There is no endian conversion
 * made, since data structure in ASV is still unknown here
 * return true if the receiced ddcb is good.
 */
static bool ddcb_2_cmd(ddcb_t *ddcb, struct ddcb_cmd *cmd)
{
	memcpy(&cmd->asv[0], (void *) &ddcb->asv[0], cmd->asv_length);

	/* copy status flags of the variant part */
	cmd->vcrc = __be16_to_cpu(ddcb->vcrc_16);
	cmd->deque_ts = __be64_to_cpu(ddcb->deque_ts_64);
	cmd->cmplt_ts = __be64_to_cpu(ddcb->cmplt_ts_64);
	cmd->attn = __be16_to_cpu(ddcb->attn_16);
	cmd->progress = __be32_to_cpu(ddcb->progress_32);
	cmd->retc = __be16_to_cpu(ddcb->retc_16);
	/* Check received seqnum here (this will become a copy from rsvd_0e field) */
	if (ddcb->rsvd_0e != ddcb->rsvd_c0)
		return false;
	return true;
}

static void afu_print_status(struct cxl_afu_h *afu_h, FILE *fp)
{
	int i;
	uint64_t addr, reg;
	long cr_device = -1, cr_vendor = -1, cr_class = -1;

	if (fp == NULL)
		return;

	cxl_get_cr_device(afu_h, 0, &cr_device);
	cxl_get_cr_vendor(afu_h, 0, &cr_vendor);
	cxl_get_cr_class(afu_h, 0, &cr_class);
	fprintf(fp,
		" cr_device:          0x%016lx\n"
		" cr_vendor:          0x%016lx\n"
		" cr_class:           0x%016lx\n",
		cr_device, cr_vendor, cr_class);

	cxl_mmio_read64(afu_h, MMIO_IMP_VERSION_REG, &reg);
	fprintf(fp, " Version Reg:        0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_APP_VERSION_REG, &reg);
	fprintf(fp, " Appl. Reg:          0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_AFU_CONFIG_REG, &reg);
	fprintf(fp, " Afu Config Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_AFU_STATUS_REG, &reg);
	fprintf(fp, " Afu Status Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_AFU_COMMAND_REG, &reg);
	fprintf(fp, " Afu Cmd Reg:        0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_FRT_REG, &reg);
	fprintf(fp, " Free Run Timer:     0x%016llx\n", (long long)reg);

	cxl_mmio_read64(afu_h, MMIO_DDCBQ_START_REG, &reg);
	fprintf(fp, " DDCBQ Start Reg:    0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_CONFIG_REG, &reg);
	fprintf(fp, " DDCBQ Conf Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_COMMAND_REG, &reg);
	fprintf(fp, " DDCBQ Cmd Reg:      0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_STATUS_REG, &reg);
	fprintf(fp, " DDCBQ Stat Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_CID_REG, &reg);
	fprintf(fp, " DDCBQ Context ID:   0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_WT_REG, &reg);
	fprintf(fp, " DDCBQ WT Reg:       0x%016llx\n", (long long)reg);

	for (i = 0; i < MMIO_FIR_REGS_NUM; i++) {
		addr = MMIO_FIR_REGS_BASE + (uint64_t)(i * 8);
		cxl_mmio_read64(afu_h, addr, &reg);
		fprintf(fp, " FIR Reg [%08llx]: 0x%016llx\n",
			(long long)addr, (long long)reg);
	}
}

/* Init Thread Wait Queue */
static void __setup_waitq(struct dev_ctx *ctx)
{
	unsigned int i;
	struct tx_waitq *q;

	for (i = 0, q = &ctx->waitq[0]; i < ctx->ddcb_num; i++, q++) {
		q->status = DDCB_FREE;
		q->cmd = NULL;
		q->ttx = NULL;
		q->thread_wait = false;
	}
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 *  o Open afu device
 *  o Map MMIO registers
 *  o Allocate and setup ddcb queue
 *  o Initialize queue hardware to become operational
 */
static int __afu_open(struct dev_ctx *ctx)
{
	int	rc = DDCB_OK;
	char	device[64];
	uint64_t mmio_dat;
	int rc0;

	/* Do not do anything if afu should have already been opened */
	if (ctx->afu_h)
		return DDCB_OK;

	if (DDCB_MODE_MASTER & ctx->mode)
		sprintf(device, "/dev/cxl/afu%d.0m", ctx->card_no);
	else	sprintf(device, "/dev/cxl/afu%d.0s", ctx->card_no);
	VERBOSE1("       [%s] AFU[%d] Enter Open: %s DDCBs @ %p\n", __func__,
		ctx->card_no, device, &ctx->ddcb[0]);

	ctx->ddcb_num = NUM_DDCBS;
	ctx->ddcb_seqnum = 0xf00d;	/* Starting Seq */
	ctx->ddcb_in = 0;		/* ddcb Input Index */
	ctx->ddcb_out = 0;		/* ddcb Output Index */

	rc0 = sem_init(&ctx->free_sem, 0, ctx->ddcb_num);
	if (rc0 != 0) {
		VERBOSE0("    [%s] initializing free_sem failed %d %s ...\n",
			 __func__, rc0, strerror(errno));
		return DDCB_ERRNO;
	}

	if (!(DDCB_MODE_MASTER & ctx->mode))
		__setup_waitq(ctx);

	ctx->afu_h = cxl_afu_open_dev(device);
	if (NULL == ctx->afu_h) {
		rc = DDCB_ERR_CARD;
		goto err_exit;
	}


	/* Check if the compiled in API version is compatible with the
	   one reported by the kernel driver */
	rc = cxl_get_api_version_compatible(ctx->afu_h,
					    &ctx->api_version_compatible);
	if ((rc != 0) ||
	    (ctx->api_version_compatible != CXL_KERNEL_API_VERSION)) {
		VERBOSE0(" [%s] ERR: incompatible API version: %ld/%d rc=%d\n",
			 __func__, ctx->api_version_compatible,
			 CXL_KERNEL_API_VERSION, rc);
		rc = DDCB_ERR_VERS_MISMATCH;
		goto err_afu_free;
	}

	/* FIXME This is still keeping the backwards compatibility */
	/* Check vendor id */
	rc = cxl_get_cr_vendor(ctx->afu_h, 0, &ctx->cr_vendor);
	if (rc == 0) {
		if (ctx->cr_vendor != CGZIP_CR_VENDOR) {
			VERBOSE0(" [%s] ERR: vendor_id: %ld/%d\n",
				 __func__, (unsigned long)ctx->cr_vendor,
				 CGZIP_CR_VENDOR);
			rc = DDCB_ERR_VERS_MISMATCH;
			goto err_afu_free;
		}
	} else
		VERBOSE0("    [%s] WARNING: checking vendor id: %08lx/%d\n",
			 __func__, ctx->cr_vendor, rc);

	/* Check device id */
	rc = cxl_get_cr_device(ctx->afu_h, 0, &ctx->cr_device);
	if (rc == 0) {
		if (ctx->cr_device != CGZIP_CR_DEVICE) {
			VERBOSE0(" [%s] ERR: device_id: %ld/%d\n",
				 __func__, (unsigned long)ctx->cr_device,
				 CGZIP_CR_VENDOR);
			rc = DDCB_ERR_CARD;
			goto err_afu_free;
		}
	} else
		VERBOSE0("    [%s] WARNING: checking device id: %08lx/%d\n",
			 __func__, ctx->cr_device, rc);


	ctx->afu_fd = cxl_afu_fd(ctx->afu_h);

	rc = cxl_afu_attach(ctx->afu_h,
			    (__u64)(unsigned long)(void *)ctx->ddcb);
	if (0 != rc) {
		rc = DDCB_ERR_CARD;
		goto err_afu_free;
	}

	if (cxl_mmio_map(ctx->afu_h, CXL_MMIO_BIG_ENDIAN) == -1) {
		rc = DDCB_ERR_CARD;
		goto err_afu_free;
	}

	if (!(DDCB_MODE_MASTER & ctx->mode)) {
		/* Only slaves can configure a Context for DMA */
		cxl_mmio_write64(ctx->afu_h, MMIO_DDCBQ_START_REG,
			(uint64_t)(void *)ctx->ddcb);

		/* | 63..48 | 47....32 | 31........24 | 23....16 | 15.....0 | */
		/* | Seqnum | Reserved | 1st ddcb num | max ddcb | Reserved | */
		mmio_dat = (((uint64_t)ctx->ddcb_seqnum << 48) |
		    	((uint64_t)ctx->ddcb_in  << 24)    |
		    	((uint64_t)(ctx->ddcb_num - 1) << 16));
		rc = cxl_mmio_write64(ctx->afu_h, MMIO_DDCBQ_CONFIG_REG, mmio_dat);
		if (rc != 0) {
			rc = DDCB_ERR_CARD;
			goto err_mmio_unmap;
		}
	}

	/* Get MMIO_APP_VERSION_REG */
	cxl_mmio_read64(ctx->afu_h, MMIO_APP_VERSION_REG, &mmio_dat);
	ctx->app_id = mmio_dat;		/* Save it */
	/* Get Context ID Register */
	cxl_mmio_read64(ctx->afu_h, MMIO_DDCBQ_CID_REG, &mmio_dat);
	ctx->cid_id = (int)mmio_dat & 0xffff;	/* only need my context */

	if (libddcb_verbose > 1)
		afu_print_status(ctx->afu_h, libddcb_fd_out);
	ctx->verify = ctx;
	VERBOSE1("       [%s] AFU[%d:%d] Exit rc: %d\n", __func__,
		ctx->card_no, ctx->cid_id, rc);
	return DDCB_OK;

 err_mmio_unmap:
	cxl_mmio_unmap(ctx->afu_h);
 err_afu_free:
	cxl_afu_free(ctx->afu_h);
	ctx->afu_h = NULL;
 err_exit:
	VERBOSE1("       [%s] AFU[%d] ERROR: rc: %d errno: %d %s\n", __func__,
		ctx->card_no, rc, errno, strerror(errno));
	return rc;
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 *       ctx->afu_h must be valid
 *       ctx->clients must be 0
 */
static inline int __afu_close(struct dev_ctx *ctx, bool force)
{
	struct cxl_afu_h *afu_h;
	uint64_t mmio_dat;
	int i = 0;
	int	rc = DDCB_OK;

	if (NULL == ctx)
		return DDCB_ERR_INVAL;

	if (ctx->verify != ctx)
		return DDCB_ERR_INVAL;

	afu_h = ctx->afu_h;
	if (NULL == afu_h) {
		VERBOSE0("[%s] WARNING: Trying to close inactive AFU!\n",
			__func__);
		return DDCB_ERR_INVAL;
	}

	if (0 != ctx->clients) {
		/*
		 * Enable this warning only in verbose mode. We have a
		 * testcase which does not close the afu handles
		 * properly, but just does exit(). This can cause the
		 * usage count still be != 0. Force is applied when
		 * the library destructor is being called. That should
		 * be fine.
		 */
		VERBOSE1("[%s] AFU[%d:%d] Error clients: %d\n",
			 __func__, ctx->card_no, ctx->cid_id, ctx->clients);
		if (!force)
			return DDCB_ERR_INVAL;
	}

	VERBOSE1("        [%s] AFU[%d:%d] Enter Open Clients: %d\n",
		__func__, ctx->card_no, ctx->cid_id, ctx->clients);
	while (1) {
		cxl_mmio_read64(afu_h, MMIO_DDCBQ_STATUS_REG, &mmio_dat);
		if (0x0ull == (mmio_dat & 0x10))
			break;

		usleep(100);
		i++;
		if (1000 == i) {
			VERBOSE0("[%s] AFU[%d:%d] Error Timeout wait_afu_stop "
				 "STATUS_REG: 0x%016llx\n", __func__,
				 ctx->card_no, ctx->cid_id, (long long)mmio_dat);
			rc = DDCB_ERR_CARD;
			break;
		}
	}
	if (libddcb_verbose > 1)
		afu_print_status(ctx->afu_h, libddcb_fd_out);

	cxl_mmio_unmap(afu_h);
	cxl_afu_free(afu_h);
	ctx->afu_h = NULL;

	VERBOSE1("        [%s] AFU[%d:%d] Exit rc: %d\n", __func__,
		ctx->card_no, ctx->cid_id, rc);
	return rc;
}

static void afu_dump_queue(struct dev_ctx *ctx)
{
	unsigned int i;
	ddcb_t *ddcb;

	for (i = 0, ddcb = &ctx->ddcb[0]; i < ctx->ddcb_num; i++, ddcb++) {
		VERBOSE0("DDCB %d [%016llx]\n", i, (long long)ddcb);
		ddcb_hexdump(libddcb_fd_out, ddcb, sizeof(ddcb_t));
	}
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 *
 * This needs to be executed only if the device is not
 * yet open. The Card (AFU) will be attaced in the done thread.
 */
static int card_dev_open(struct dev_ctx *ctx)
{
	int rc = DDCB_OK;
	void *res = NULL;

	VERBOSE1("    [%s] AFU[%d] Enter clients: %d open_done_sem: %p\n",
		 __func__, ctx->card_no, ctx->clients, &ctx->open_done_sem);

	if (ctx->ddcb_done_tid != 0)  /* already in use!! */
		return DDCB_OK;

	/* Create a semaphore to wait until afu Open is done */
	rc = sem_init(&ctx->open_done_sem, 0, 0);
	if (0 != rc) {
		VERBOSE0("ERROR: initializing open_done_sem %p %d %s!\n",
			 &ctx->open_done_sem, rc, strerror(errno));
		return DDCB_ERRNO;
	}

	/* Now create the worker thread which opens the afu */
	rc = pthread_create(&ctx->ddcb_done_tid, NULL, &__ddcb_done_thread,
			    ctx);
	if (0 != rc) {
		VERBOSE1("    [%s] ERROR: pthread_create rc: %d\n",
			__func__, rc);
		return DDCB_ERR_ENOMEM;
	}

	TEMP_FAILURE_RETRY(sem_wait(&ctx->open_done_sem));
	rc = ctx->afu_rc;	/* Get RC */
	if (DDCB_OK != rc) {
		/* The thread was not able to open or init tha AFU */
		VERBOSE1("    [%s] AFU[%d] ERROR: rc: %d\n",
			__func__, ctx->card_no, rc);
		/* Wait for done thread to join */
		pthread_join(ctx->ddcb_done_tid, &res);
		ctx->ddcb_done_tid = 0;
	}

	VERBOSE1("    [%s] AFU[%d:%d] Exit rc: %d\n", __func__,
		ctx->card_no, ctx->cid_id, rc);
	return rc;
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 */
static int card_dev_close(struct dev_ctx *ctx)
{
	int rc = DDCB_OK;
	void *res = NULL;

	VERBOSE1("    [%s] AFU[%d:%d] Enter clients: %d\n", __func__,
		ctx->card_no, ctx->cid_id, ctx->clients);

	if (ctx->ddcb_done_tid) {
		rc = pthread_cancel(ctx->ddcb_done_tid);
		VERBOSE1("    [%s] AFU[%d:%d] Wait done_thread to join rc: %d\n",
			__func__, ctx->card_no, ctx->cid_id, rc);
		rc = pthread_join(ctx->ddcb_done_tid, &res);
		VERBOSE1("    [%s] AFU[%d:%d] clients: %d rc: %d\n", __func__,
			ctx->card_no, ctx->cid_id, ctx->clients, rc);
		ctx->ddcb_done_tid = 0;
	}
	VERBOSE1("    [%s] AFU[%d:%d] Exit clients: %d\n", __func__,
		ctx->card_no, ctx->cid_id, ctx->clients);
	return rc;
}

static int __client_inc(struct dev_ctx *ctx, unsigned int mode)
{
	int rc = DDCB_OK;

	pthread_mutex_lock(&ctx->lock);

	VERBOSE1("  [%s] AFU[%d:%d] Enter clients: %d\n", __func__,
		ctx->card_no, ctx->cid_id, ctx->clients);

	if (ctx->clients == 0) {
		ctx->mode = mode;
		rc = card_dev_open(ctx);
		if (DDCB_OK == rc)
			ctx->clients++;	/* increment clients only if good */
	} else {
		if (mode != ctx->mode)
			rc = DDCB_ERRNO;
		else ctx->clients++;		/* increment clients */
	}

	VERBOSE1("  [%s] AFU[%d:%d] Exit clients: %d rc: %d\n", __func__,
		ctx->card_no, ctx->cid_id, ctx->clients, rc);
	pthread_mutex_unlock(&ctx->lock);

	return rc;
}

static void __client_dec(struct dev_ctx *ctx)
{
	pthread_mutex_lock(&ctx->lock);

	VERBOSE1("  [%s] AFU[%d:%d] Enter Clients: %d\n", __func__,
		 ctx->card_no, ctx->cid_id, ctx->clients);

	ctx->clients--;

	/*
	 * Since closing the AFU is so expensive, we keep the afu
	 * handle and the allocating thread alive until the
	 * application exits.
	 *
	 * if (0 == ctx->clients)
	 *         card_dev_close(ctx);
	 */
	VERBOSE1("  [%s] AFU[%d:%d] Exit Clients: %d\n", __func__,
		 ctx->card_no, ctx->cid_id, ctx->clients);

	pthread_mutex_unlock(&ctx->lock);
}

static void *card_open(int card_no, unsigned int mode, int *card_rc,
		       uint64_t appl_id __attribute__((unused)),
		       uint64_t appl_id_mask __attribute__((unused)))
{
	unsigned int i;
	int rc = DDCB_OK;
	struct ttxs *ttx = NULL;

	VERBOSE1("[%s] AFU[%d] Enter mode: 0x%x\n", __func__,
		card_no, mode);

	if ((card_no != ACCEL_REDUNDANT) &&
	    ((card_no < 0) || (card_no >= NUM_CARDS))) {
		rc = DDCB_ERR_INVAL;
		goto card_open_exit;
	}

	/* Allocate Thread Context */
	ttx = calloc(1, sizeof(*ttx));
	if (!ttx) {
		rc = DDCB_ERR_ENOMEM;
		goto card_open_exit;
	}

	/* Inc use count and initialize AFU on first open */
	sem_init(&ttx->wait_sem, 0, 0);
	ttx->card_no = card_no;	/* Save only right now */
	ttx->app_id = appl_id;
	ttx->app_id_mask = appl_id_mask;
	ttx->card_next = rand() % NUM_CARDS;  /* start always random */
	ttx->mode = mode;
	ttx->verify = ttx;

	/*
	 * We bind the client to the card in open for single card mode
	 * and to any card in redundant mode.
	 */
	if (ttx->card_no != ACCEL_REDUNDANT) {
		ttx->ctx = &my_ctx[card_no];  /* select card context */
		rc = __client_inc(ttx->ctx, mode);
		if (rc != DDCB_OK) {
			free(ttx);
			ttx = NULL;
		}
	} else {
		/* open all possible cards */
		for (i = 0; i < NUM_CARDS; i++) {
			rc = __client_inc(&my_ctx[ttx->card_next], mode);
			if (rc == DDCB_OK)  /* remember last one which is ok */
				ttx->ctx = &my_ctx[ttx->card_next];
			ttx->card_next = (ttx->card_next + 1) %	NUM_CARDS;
		}
	}

 card_open_exit:
	if (card_rc)
		*card_rc = rc;
	VERBOSE1("[%s] AFU[%d] Exit ttx: %p\n", __func__,
		card_no, ttx);
	return ttx;
}

static int card_close(void *card_data)
{
	unsigned int i;
	struct ttxs *ttx = (struct ttxs*)card_data;

	VERBOSE1("[%s] Enter ttx: %p\n", __func__, ttx);
	if (NULL == ttx)
		return DDCB_ERR_INVAL;

	if (ttx->verify != ttx)
		return DDCB_ERR_INVAL;

	rt_trace(0xdeaf, 0, 0, ttx);

	if (ttx->card_no != ACCEL_REDUNDANT)
		__client_dec(ttx->ctx);
	else
		for (i = 0; i < NUM_CARDS; i++)
			__client_dec(&my_ctx[i]);

	ttx->verify = NULL;
	free(ttx);

	rt_trace_dump();
	VERBOSE1("[%s] Exit ttx: %p\n", __func__, ttx);
	return DDCB_OK;
}

static void start_ddcb(struct	cxl_afu_h *afu_h, int seq)
{
	uint64_t	reg;

	reg = (uint64_t)seq << 48 | 1;	/* Set Seq. Number + Start Bit */
	cxl_mmio_write64(afu_h, MMIO_DDCBQ_COMMAND_REG, reg);
}

/**
 * Set command into next DDCB Slot
 */
static int __ddcb_execute_multi(void *card_data, struct ddcb_cmd *cmd)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx	*ctx = NULL;
	struct	tx_waitq *txq = NULL;
	ddcb_t	*ddcb;
	int	idx = 0;
	int	seq, val;
	struct	ddcb_cmd *my_cmd;

	if (NULL == ttx)
		return DDCB_ERR_INVAL;
	if (ttx->verify != ttx)
		return DDCB_ERR_INVAL;
	if (NULL == cmd)
		return DDCB_ERR_INVAL;
	ctx = ttx->ctx;					/* get card Context */
	if (DDCB_MODE_MASTER & ctx->mode)	/* no DMA in Master Mode */
		return DDCB_ERR_INVAL;
	my_cmd = cmd;

	while (my_cmd) {
		sem_getvalue(&ctx->free_sem, &val);
		TEMP_FAILURE_RETRY(sem_wait(&ctx->free_sem));

		pthread_mutex_lock(&ctx->lock);

		idx = ctx->ddcb_in;
		ddcb = &ctx->ddcb[idx];
		txq = &ctx->waitq[idx];
		txq->ttx = ttx;			/* set ttx pointer into txq */
		txq->status = DDCB_IN;
		seq = (int)ctx->ddcb_seqnum;	/* Get seq */
		txq->cmd = my_cmd;		/* my command to txq */
		txq->seqnum = ctx->ddcb_seqnum;	/* Save seq Number */
		txq->q_in_time = get_msec();	/* Save now time in msec */
		ctx->ddcb_seqnum++;		/* Next seq */
		rt_trace(0x00a0, seq, idx, ttx);
		VERBOSE1("[%s] AFU[%d:%d] seq: 0x%x slot: %d cmd: %p\n", __func__,
			ctx->card_no, ctx->cid_id, seq, idx, my_cmd);
		/* Increment ddcb_in and warp back to 0 */
		ctx->ddcb_in = (ctx->ddcb_in + 1) % ctx->ddcb_num;

		cmd_2_ddcb(ddcb, my_cmd, seq,
			   (ctx->mode & DDCB_MODE_POLLING) ? false : true);

		start_ddcb(ctx->afu_h, seq);
		/* Get  Next cmd and continue if there is one */
		my_cmd = (struct ddcb_cmd *)my_cmd->next_addr;
		if (NULL == my_cmd)
			txq->thread_wait = true;

		pthread_mutex_unlock(&ctx->lock);
	}

	/* Block Caller */
	VERBOSE2("[%s] Wait ttx: %p\n", __func__, ttx);
	TEMP_FAILURE_RETRY(sem_wait(&ttx->wait_sem));
	rt_trace(0x00af, ttx->seqnum, idx, ttx);
	VERBOSE2("[%s] return ttx: %p\n", __func__, ttx);
	return ttx->compl_code;	/* Give Completion code back to caller */
}

static int ddcb_execute(void *card_data, struct ddcb_cmd *cmd)
{
	int rc;
	unsigned int i;
	struct ttxs *ttx = (struct ttxs*)card_data;

	if (ttx->card_no == ACCEL_REDUNDANT) {
		for (i = 0; i < NUM_CARDS; i++) {
			ttx->card_next = (ttx->card_next + 1) % NUM_CARDS;

			if (my_ctx[ttx->card_next].afu_h != NULL) {
				ttx->ctx = &my_ctx[ttx->card_next];
				break;
			}
		}
	}

	rc = __ddcb_execute_multi(card_data, cmd);
	if (DDCB_OK != rc)
		errno = EBADF;	/* Return Invalid exchange */

	return rc;
}

static bool __ddcb_done_post(struct dev_ctx *ctx, int compl_code)
{
	int	idx, elapsed_time;
	ddcb_t	*ddcb;
	struct	tx_waitq	*txq;
	struct	ttxs		*ttx = NULL;

	pthread_mutex_lock(&ctx->lock);
	idx = ctx->ddcb_out;
	ddcb = &ctx->ddcb[idx];
	txq = &ctx->waitq[idx];

	/* Check if Nothing to do, goto exit and wait again */
	if (DDCB_IN != txq->status)
		goto post_exit_stop;

	elapsed_time = (int)(get_msec() - txq->q_in_time);

	if (DDCB_ERR_IRQTIMEOUT == compl_code) {
		if (ddcb->retc_16)
			VERBOSE2("\t[%s] AFU[%d:%d] seq: 0x%x slot: %d compl_code: %d"
				" retc: %4.4x after %d msec. wait 4 IRQ\n",
				__func__, ctx->card_no, ctx->cid_id, txq->seqnum,
				idx, compl_code, ddcb->retc_16, elapsed_time);
		/* Select Timeout and no data received */
		if (elapsed_time < (ctx->tout * 1000))
			goto post_exit_cont;	/* Continue until timeout */

		VERBOSE2("\t[%s] AFU[%d:%d] seq: 0x%x slot: %d timeout "
			"after %d msec\n", __func__,
			ctx->card_no, ctx->cid_id, txq->seqnum,
			idx, elapsed_time);
	}

	if ((DDCB_OK == compl_code) && (0 == ddcb->retc_16)) {
		/* Still waiting for retc to be set */
		rt_trace(0x001a, ddcb->retc_16, idx, 0);
		VERBOSE2("\t[%s] AFU[%d:%d] seq: 0x%x slot: %d "
			 "retc: 0 wait\n", __func__,
			 ctx->card_no, ctx->cid_id, txq->seqnum, idx);
		goto post_exit_stop;
	}

	if (libddcb_verbose > 3) {
		/* For debug only */
		VERBOSE0("AFU[%d:%d] DDCB %d [%016llx] after execution "
			 "compl_code: %d retc16: %4.4x\n",
			ctx->card_no, ctx->cid_id, idx, (long long)ddcb, compl_code,
			ddcb->retc_16);
		ddcb_hexdump(libddcb_fd_out, ddcb, sizeof(ddcb_t));
	}

	/* Copy the ddcb back to cmd, and check for error */
	if (false == ddcb_2_cmd(ddcb, txq->cmd)) {
		/* Overwrite compl_code only if not set before */
		if (DDCB_OK != compl_code)
			compl_code = DDCB_ERR_EXEC_DDCB;
	}

	if (DDCB_OK != compl_code)
		VERBOSE0("\t[%s] AFU[%d:%d] seq: 0x%x slot: %d compl_code: %d"
			" retc: %x after: %d msec\n", __func__,
			ctx->card_no, ctx->cid_id, txq->seqnum, idx,
			compl_code, ddcb->retc_16, elapsed_time);
	else
		VERBOSE1("\t[%s] AFU[%d:%d] seq: 0x%x slot: %d compl_code: %d"
			" retc: %x after: %d msec\n", __func__,
			ctx->card_no, ctx->cid_id, txq->seqnum, idx,
			compl_code, ddcb->retc_16, elapsed_time);

	ttx = txq->ttx;
	ttx->compl_code = compl_code;
	rt_trace(0x0011, txq->seqnum, idx, ttx);
	sem_post(&ctx->free_sem);
	if (txq->thread_wait) {
		rt_trace(0x0012, txq->seqnum, idx, ttx);
		VERBOSE1("\t[%s] AFU[%d:%d] Post: %p\n", __func__,
			ctx->card_no, ctx->cid_id, ttx);
		sem_post(&ttx->wait_sem);
		txq->thread_wait = false;
	}

	/* Increment and wrap back to start */
	ctx->ddcb_out = (ctx->ddcb_out + 1) % ctx->ddcb_num;
	txq->status = DDCB_FREE;

  post_exit_cont:
	pthread_mutex_unlock(&ctx->lock);
	return true;		/* Continue Loop */
  post_exit_stop:
	pthread_mutex_unlock(&ctx->lock);
	return false;		/* Stop Loop */
}

/**
 * The cleanup function gets invoked after the thread was canceld by
 * sending card_dev_close(). This function was intended to close the
 * AFU. But it turned out that closing it has sigificant performance
 * impact. So we decided to keep the afu resource opened until the
 * application terminates. This will absorb one file descriptor plus
 * the memory associated to the afu handle.
 *
 * Note: Open and Close the AFU must handled by the same thread id.
 *       ctx->lock must be held when entering this function.
 */
static void __ddcb_done_thread_cleanup(void *arg __attribute__((unused)))
{
	struct dev_ctx *ctx = (struct dev_ctx *)arg;

	VERBOSE1("\t[%s]\n", __func__);
	__afu_close(ctx, true);
}

/**
 * Process DDCB queue results using polling for completion. This
 * implementation might not yet be perfect from an error isolation
 * standpoint. E.g. how to handle error interrupt conditions without
 * impacting performance? We still do it to figure possible
 * performance differentces between interrupt and polling driven
 * operation.
 */
static int __ddcb_process_polling(struct dev_ctx *ctx)
{
	int tasks;

	VERBOSE1("[%s] AFU[%d:%d] Enter polling work loop\n", __func__,
		ctx->card_no, ctx->cid_id);
	while (1) {
		/*
		 * Using trylock in combination with testcancel to
		 * avoid deadlock situations when competing for the
		 * ctx->lock on shutdown ...
		 */
		/* while (pthread_mutex_trylock(&ctx->lock) != 0)
		   pthread_testcancel(); */

		pthread_testcancel();

		tasks = 0;
		while (__ddcb_done_post(ctx, DDCB_OK))
			tasks++;
		ctx->completed_ddcbs += tasks;
		if (tasks < NUM_DDCBS)
			ctx->completed_tasks[tasks]++;
		else
			ctx->completed_tasks[NUM_DDCBS]++;

		/* pthread_mutex_unlock(&ctx->lock); */

	}
	VERBOSE1("[%s] AFU[%d:%d] Exit polling work loop\n", __func__,
		ctx->card_no, ctx->cid_id);

	return 0;
}

/**
 * Process DDCB queue results using completion processing with
 * interrupt.
 */
static int __ddcb_process_irqs(struct dev_ctx *ctx)
{
	int rc;

	VERBOSE1("[%s] AFU[%d:%d] Enter interrupt work loop\n", __func__,
		ctx->card_no, ctx->cid_id);
	while (1) {
		fd_set	set;
		struct	timeval timeout;

		FD_ZERO(&set);
		FD_SET(ctx->afu_fd, &set);

		/* Set timeout to "tout" seconds */
		timeout.tv_sec = 0; // ctx->tout;
		timeout.tv_usec = 100 * 1000;	/* 100 msec */

		rc = select(ctx->afu_fd + 1, &set, NULL, NULL, &timeout);
		if (0 == rc) {
			/* Timeout will Post error code only if context is active */
			__ddcb_done_post(ctx, DDCB_ERR_IRQTIMEOUT);
			continue;
		}
		if ((rc == -1) && (errno == EINTR)) {
			VERBOSE0("WARNING: select returned -1 "
				 "and errno was EINTR, retrying\n");
			continue;
		}
		rt_trace(0x0010, 0, 0, 0);

		/*
		 * FIXME I wonder if we must exit in this
		 * case. select() returning a negative value is
		 * clearly a critical issue. Only if errno == EINTR,
		 * we should rety.
		 *
		 * At least we should wakeup potential DDCB execution
		 * requestors, such that the error will be passed to
		 * the layers above and the application can be stopped
		 * if needed.
		 */
		if (rc < 0) {
			VERBOSE0("ERROR: waiting for interrupt! rc: %d\n", rc);
			afu_print_status(ctx->afu_h, libddcb_fd_out);
			while (__ddcb_done_post(ctx, DDCB_ERR_SELECTFAIL)) {
				/* empty */
			}
			continue;
		}

		ctx->process_irqs++;	/* Increment stat conuter */
		rc = cxl_read_event(ctx->afu_h, &ctx->event);
		if (0 != rc) {
			VERBOSE0("\tERROR: cxl_read_event rc: %d errno: %d\n",
				 rc, errno);
			continue;
		}
		VERBOSE2("\tcxl_read_event(...) = %d for context: %d "
			 "type: %d size: %d\n", rc, ctx->cid_id,
			 ctx->event.header.type, ctx->event.header.size);

		switch (ctx->event.header.type) {
		case CXL_EVENT_AFU_INTERRUPT: {
			unsigned int tasks = 0;

			/* Process all ddcb's */
			VERBOSE2("\tCXL_EVENT_AFU_INTERRUPT: flags: 0x%x "
				 "irq: 0x%x\n",
				ctx->event.irq.flags,
				ctx->event.irq.irq);

			while (__ddcb_done_post(ctx, DDCB_OK))
				tasks++;
			ctx->completed_ddcbs += tasks;
			if (tasks < NUM_DDCBS)
				ctx->completed_tasks[tasks]++;
			else	ctx->completed_tasks[NUM_DDCBS]++;
			break;
		}
		case CXL_EVENT_DATA_STORAGE:
			rt_trace(0xbbbb, ctx->ddcb_out, ctx->ddcb_in, NULL);
			VERBOSE0("\tCXL_EVENT_DATA_STORAGE: flags: 0x%x "
				 "addr: 0x%016llx dsisr: 0x%016llx\n",
				ctx->event.fault.flags,
				(long long)ctx->event.fault.addr,
				(long long)ctx->event.fault.dsisr);
			afu_print_status(ctx->afu_h, libddcb_fd_out);
			afu_dump_queue(ctx);
			rt_trace_dump();
			while (__ddcb_done_post(ctx, DDCB_ERR_EVENTFAIL)) {
				/* empty */
			}
			break;
		case CXL_EVENT_AFU_ERROR:
			VERBOSE0("\tCXL_EVENT_AFU_ERROR: flags: 0x%x "
				 "error: 0x%016llx\n",
				ctx->event.afu_error.flags,
				(long long)ctx->event.afu_error.error);
			afu_print_status(ctx->afu_h, libddcb_fd_out);
			while (__ddcb_done_post(ctx, DDCB_ERR_EVENTFAIL)) {
				/* empty */
			}
			break;
		default:
			VERBOSE0("\tcxl_read_event() %d unknown header type\n",
				ctx->event.header.type);
			__ddcb_done_post(ctx, DDCB_ERR_EVENTFAIL);
			break;
		}
	}

	return 0;
}

/**
 * DDCB completion and timeout handling. This function implements the
 * thread which looks out for completed DDCBs. Due to a CAPI
 * restriction it also needs to open and close the AFU handle used to
 * communicate to the CAPI card.
 */
static void *__ddcb_done_thread(void *card_data)
{
	int rc = 0, rc0;
	struct dev_ctx *ctx = (struct dev_ctx *)card_data;

	VERBOSE1("[%s] AFU[%d] Enter\n", __func__,
		ctx->card_no);
	rc = __afu_open(ctx);
	ctx->afu_rc = rc;		/* Save rc */

	rc0 = sem_post(&ctx->open_done_sem);	/* Post card_dev_open() */
	if (rc0 != 0) {
		VERBOSE0("[%s] AFU[%d] ERROR: %d %s\n", __func__,
			ctx->card_no, rc0, strerror(errno));
		__afu_close(ctx, false);
		ctx->afu_rc = -1;
		return NULL;
	}

	if (DDCB_OK != rc) {
		/* Error Exit here in case open the AFU failed */
		VERBOSE1("[%s] AFU[%d:%d] ERROR: %d Thread Exit\n", __func__,
			ctx->card_no, ctx->cid_id, rc);

		/* Join in card_dev_open() */
		return NULL;
	}

	/* Push the Cleanup Handler to close the AFU */
	pthread_cleanup_push(__ddcb_done_thread_cleanup, ctx);

	if (DDCB_MODE_MASTER & ctx->mode) {
		/* We do not have any code to execute when the master
		   was oppend */
		/* Master will be only used for peek and poke */
		while (1) {
			sleep(1);
		}
	}
	if (DDCB_MODE_POLLING & ctx->mode)
		__ddcb_process_polling(ctx);
	else
		__ddcb_process_irqs(ctx);

	pthread_cleanup_pop(1);
	return NULL;
}

static const char *_card_strerror(void *card_data __attribute__((unused)),
				  int card_rc __attribute__((unused)))
{
	return NULL;
}

static uint64_t card_read_reg64(void *card_data, uint32_t offs, int *card_rc)
{
	int rc = 0;
	uint64_t data = 0;
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx && (ttx->verify == ttx)) {
		ctx = ttx->ctx;
		if (ctx->afu_h) {
			rc = cxl_mmio_read64(ctx->afu_h, offs, &data);
			if (card_rc)
				*card_rc = rc;
			return data;
		}
	}
	if (card_rc)
		*card_rc = DDCB_ERR_INVAL;
	return 0;
}

static uint32_t card_read_reg32(void *card_data __attribute__((unused)),
				uint32_t offs __attribute__((unused)),
				int *card_rc __attribute__((unused)))
{
	int rc = 0;
	uint32_t data = 0;
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx && (ttx->verify == ttx)) {
		ctx = ttx->ctx;
		if (ctx->afu_h) {
			rc = cxl_mmio_read32(ctx->afu_h, offs, &data);
			if (card_rc)
				*card_rc = rc;
			return data;
		}
	}
	if (card_rc)
		*card_rc = DDCB_ERR_INVAL;
	return 0;
}

static int card_write_reg64(void *card_data, uint32_t offs, uint64_t data)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx && (ttx->verify == ttx)) {
		ctx = ttx->ctx;
		if (ctx->afu_h)
			return cxl_mmio_write64(ctx->afu_h, offs, data);
	}
	return DDCB_ERR_INVAL;
}

static int card_write_reg32(void *card_data, uint32_t offs, uint32_t data)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx && (ttx->verify == ttx)) {
		ctx = ttx->ctx;
		if (ctx->afu_h)
			return cxl_mmio_write32(ctx->afu_h, offs, data);
	}
	return DDCB_ERR_INVAL;
}

/**
 * The CAPI card implementation is always matching the zEDCv2
 * compressor implementation. It is complicated to return the right
 * version in case of multicard mode, since the DDCB execution is
 * altering through the cards. The right solution here is to enhance
 * the appl_id_mask, such that the version bits are considered and
 * only cards with the same id are being used.
 */
static uint64_t _card_get_app_id(void *card_data)
{
	struct ttxs *ttx = (struct ttxs *)card_data;

	if (ttx == NULL)
		return DDCB_ERR_INVAL;

	return ttx->app_id;
}

/**
 * The Queue worktimer increments every 4 cycles.
 */
static uint64_t _card_get_queue_work_time(void *card_data)
{
	int rc;
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;
	uint64_t data;

	if (ttx && (ttx->verify == ttx)) {
		ctx = ttx->ctx;
		if (!ctx)
			return 0;

		rc = cxl_mmio_read64(ctx->afu_h, MMIO_DDCBQ_WT_REG, &data);
		if (rc != 0)
			return 0;

		/* FIXME New versions do not need masking. */
		return data & 0x00ffffffffffffffull;
	}
	return 0;
}

/**
 * Our CAPI version runs with 250 MHz.
 */
static uint64_t _card_get_frequency(void *card_data __attribute__((unused)))
{
	/* FIXME Version register contains that info. */
	return 250 * 1000000;
}


static void card_dump_hardware_version(void *card_data, FILE *fp)
{
	struct ttxs *ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (!(ttx && (ttx->verify == ttx)))
		return;

	ctx = ttx->ctx;
	if (!ctx)
		return;

	afu_print_status(ctx->afu_h, fp);
}

static int card_pin_memory(void *card_data __attribute__((unused)),
			   const void *addr __attribute__((unused)),
			   size_t size __attribute__((unused)),
			   int dir __attribute__((unused)))
{
	return DDCB_OK;
}

static int card_unpin_memory(void *card_data __attribute__((unused)),
			     const void *addr __attribute__((unused)),
			     size_t size __attribute__((unused)))
{
	return DDCB_OK;
}

static void *card_malloc(void *card_data __attribute__((unused)),
			 size_t size)
{
	return memalign(sysconf(_SC_PAGESIZE), size);
}

static int card_free(void *card_data __attribute__((unused)),
		     void *ptr,
		     size_t size __attribute__((unused)))
{
	if (ptr == NULL)
		return DDCB_OK;

	free(ptr);
	return DDCB_OK;
}

static void __dev_dump(struct dev_ctx *ctx, FILE *fp)
{
	unsigned int i;
	bool work_done = false;

	if (fp == NULL)
		return;

	for (i = 0; i < NUM_DDCBS + 1; i++) {
		if (0 != ctx->completed_tasks[i]) {
			work_done = true;
			break;
		}
	}
	if (false == work_done)
		return;	/* Exit if not used */

	/* Keep this in a single print so we do not get mixed lines
	   from other process */
	fprintf(fp, "  AFU[%d:%d] irqs: %d] Completed DDCBs: %lld\n"
		    "  Stats: %d(wait), %d(x1), %d(x2), %d(x3), %d(x4 an more)\n",
		ctx->card_no, ctx->cid_id, ctx->process_irqs,
		(long long)ctx->completed_ddcbs,
		(int)ctx->completed_tasks[0],
		(int)ctx->completed_tasks[1],
		(int)ctx->completed_tasks[2],
		(int)ctx->completed_tasks[3],
		(int)ctx->completed_tasks[4]);
}

static int _accel_dump_statistics(FILE *fp)
{
	unsigned int card_no;

	for (card_no = 0; card_no < NUM_CARDS; card_no++)
		__dev_dump(&my_ctx[card_no], fp);

	return 0;
}

static struct ddcb_accel_funcs accel_funcs = {
	.card_type = DDCB_TYPE_CAPI,
	.card_name = "CAPI",

	/* functions */
	.card_open = card_open,
	.card_close = card_close,
	.ddcb_execute = ddcb_execute,
	.card_strerror = _card_strerror,
	.card_read_reg64 = card_read_reg64,
	.card_read_reg32 = card_read_reg32,
	.card_write_reg64 = card_write_reg64,
	.card_write_reg32 = card_write_reg32,
	.card_get_app_id = _card_get_app_id,
	.card_get_queue_work_time = _card_get_queue_work_time,
	.card_get_frequency = _card_get_frequency,
	.card_dump_hardware_version = card_dump_hardware_version,
	.card_pin_memory = card_pin_memory,
	.card_unpin_memory = card_unpin_memory,
	.card_malloc = card_malloc,
	.card_free = card_free,

	/* statistics */
	.dump_statistics = _accel_dump_statistics,
	.num_open = 0,
	.num_close = 0,
	.num_execute = 0,
	.time_open = 0,
	.time_execute = 0,
	.time_close = 0,

	.priv_data = NULL,
};

static void capi_card_init(void) __attribute__((constructor));
static void capi_card_init(void)
{
	int rc, tout = CONFIG_DDCB_TIMEOUT;
	unsigned int card_no;
	const char *ttt = getenv("DDCB_TIMEOUT");

	rt_trace_init();

	if (ttt)
		tout = strtoul(ttt, (char **) NULL, 0);

	for (card_no = 0; card_no < NUM_CARDS; card_no++) {
		struct dev_ctx *ctx = &my_ctx[card_no];

		ctx->card_no = card_no;
		ctx->app_id = 0x0;
		ctx->afu_h = NULL;
		ctx->ddcb_done_tid = 0;
		ctx->ddcb = my_ddcbs[card_no];
		ctx->ddcb_num = NUM_DDCBS;
		ctx->tout = tout;

		rc = pthread_mutex_init(&ctx->lock, NULL);
		if (0 != rc) {
			VERBOSE0("ERROR: initializing mutex failed!\n");
			return;
		}
	}
	ddcb_register_accelerator(&accel_funcs);
}

static void capi_card_exit(void) __attribute__((destructor));
static void capi_card_exit(void)
{
	unsigned int card_no;

	for (card_no = 0; card_no < NUM_CARDS; card_no++) {
		struct dev_ctx *ctx = &my_ctx[card_no];

		card_dev_close(ctx);
	}
}
