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

/*
 * Specialized DDCB execution implementation.
 *
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
#define _GNU_SOURCE
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

#include <libddcb.h>
#include <ddcb.h>
#include <libcxl.h>
#include <memcopy_ddcb.h>

#define MMIO_IMP_VERSION_REG	0x0000000ull
#define MMIO_APP_VERSION_REG	0x0000008ull
#define MMIO_AFU_CONFIG_REG	0x0000010ull
#define MMIO_AFU_STATUS_REG	0x0000018ull
#define MMIO_AFU_COMMAND_REG	0x0000020ull
#define MMIO_FRT_REG		0x0000080ull

#define MMIO_DDCBQ_START_REG	0x0000100ull
#define MMIO_DDCBQ_CONFIG_REG	0x0000108ull
#define MMIO_DDCBQ_COMMAND_REG	0x0000110ull
#define MMIO_DDCBQ_STATUS_REG	0x0000118ull
#define MMIO_DDCBQ_WT_REG	0x0000180ull

#define MMIO_FIR_REGS_BASE	0x0001000ull	/* FIR: 1000...1028 */
#define MMIO_FIR_REGS_NUM	6

#define MMIO_ERRINJ_MMIO_REG	0x0001800ull
#define MMIO_ERRINJ_GZIP_REG	0x0001808ull

#define	MMIO_AGRV_REGS_BASE	0x0002000ull
#define	MMIO_AGRV_REGS_NUM	16

#define	MMIO_GZIP_REGS_BASE	0x0002100ull
#define	MMIO_GZIP_REGS_NUM	16

#define MMIO_DEBUG_REG		0x000FF00ull

#define	NUM_DDCBS	4

extern int libddcb_verbose;

#include <sys/syscall.h>   /* For SYS_xxx definitions */

static inline pid_t gettid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

#define VERBOSE0(fmt, ...) do {						\
		fprintf(stderr, "%08x.%08x: " fmt,			\
			getpid(), gettid(), ## __VA_ARGS__);		\
	} while (0)

#define VERBOSE1(fmt, ...) do {						\
		if (libddcb_verbose > 0)				\
			fprintf(stderr, "%08x.%08x: " fmt,		\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE2(fmt, ...) do {						\
		if (libddcb_verbose > 1)				\
			fprintf(stderr, "%08x.%08x: " fmt,		\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE3(fmt, ...) do {						\
		if (libddcb_verbose > 3)				\
			fprintf(stderr, "%08x.%08x: " fmt,		\
				getpid(), gettid(), ## __VA_ARGS__);	\
	} while (0)

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
};

/**
 * A a device context is normally bound to a card which provides a
 * ddcb queue. Whenever a new context is created a queue is attached
 * to it. Whenever it is removed the queue is removed too. There can
 * be multiple contexts using just one card.
 */
struct dev_ctx {
	int		card_no;
	pthread_mutex_t	lock;
	int		clients;	/* Thread open counter */
	struct cxl_afu_h *afu_h;	/* afu_h != NULL device is open */
	int		afu_fd;
	uint16_t	ddcb_seqnum;
	uint16_t	ddcb_free1;	/* Not used */
	unsigned int	ddcb_num;	/* How deep is my ddcb queue */
	int		ddcb_out;	/* ddcb Output (done) index */
	int		ddcb_in;	/* ddcb Input index */
	ddcb_t		*ddcb;		/* Pointer to the ddcb queue */
	struct		tx_waitq	*waitq;
	struct		cxl_event	event;	/* last AFU event */
	int		tout;		/* Timeout Value for compeltion */
	pthread_t	ddcb_done_tid;
	uint64_t	app_id;		/* a copy of MMIO_APP_VERSION_REG */
	sem_t		free_sem;	/* Sem to wait for free ddcb */
	struct		dev_ctx		*verify;	/* Verify field */
};

static struct dev_ctx my_ctx;	/* My Card */


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
	trc_buff[i].tid = (uint32_t)pthread_self();
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

	if (0 == trc_idx) return;
	fprintf(stderr, "Index: %d Warp: %d\n", trc_idx, trc_wrap);
	for (i = 0; i < RT_TRACE_SIZE; i++) {
		if (0 == trc_buff[i].tok) break;
		fprintf(stderr, "%03d: %04x - %04x - %04x - %04x - %p\n",
			i, trc_buff[i].tid, trc_buff[i].tok,
			trc_buff[i].n1, trc_buff[i].n2, trc_buff[i].p);
	}
	trc_idx = 0;
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
			      uint16_t seqnum)
{
	pddcb->pre = DDCB_PRESET_PRE;
	pddcb->cmdopts_16 = __cpu_to_be16(cmd->cmdopts);
	pddcb->cmd = cmd->cmd;
	pddcb->acfunc = cmd->acfunc;	/* functional unit */
	pddcb->psp = (((cmd->asiv_length / 8) << 4) | ((cmd->asv_length / 8)));
	pddcb->n.ats_64 = __cpu_to_be64(cmd->ats);
	memcpy(&pddcb->n.asiv[0], &cmd->asiv[0], DDCB_ASIV_LENGTH_ATS);
	pddcb->icrc_hsi_shi_32 = __cpu_to_be32(0x00000000); /* for crc */

	pddcb->deque_ts_64 = __cpu_to_be64(0xffdd0000 | seqnum);

	/* DDCB completion irq */
	pddcb->icrc_hsi_shi_32 |= DDCB_INTR_BE32;
	pddcb->seqnum = __cpu_to_be16(seqnum);
	pddcb->retc_16 = 0;
	if (libddcb_verbose > 3) {
		VERBOSE0("DDCB [%016llx] Seqnum 0x%x before execution:\n",
			(long long)(unsigned long)(void *)pddcb, seqnum);
		ddcb_hexdump(stderr, pddcb, sizeof(ddcb_t));
	}
}

/**
 * Copy DDCB ASV to request struct. There is no endian conversion
 * made, since data structure in ASV is still unknown here
 */
static void ddcb_2_cmd(ddcb_t *ddcb, struct ddcb_cmd *cmd)
{
	memcpy(&cmd->asv[0], (void *) &ddcb->asv[0], cmd->asv_length);

	/* copy status flags of the variant part */
	cmd->vcrc = __be16_to_cpu(ddcb->vcrc_16);
	cmd->deque_ts = __be64_to_cpu(ddcb->deque_ts_64);
	cmd->cmplt_ts = __be64_to_cpu(ddcb->cmplt_ts_64);
	cmd->attn = __be16_to_cpu(ddcb->attn_16);
	cmd->progress = __be32_to_cpu(ddcb->progress_32);
	cmd->retc = __be16_to_cpu(ddcb->retc_16);
}

static void afu_print_status(struct cxl_afu_h *afu_h)
{
	int i;
	uint64_t addr, reg;

	cxl_mmio_read64(afu_h, MMIO_IMP_VERSION_REG, &reg);
	VERBOSE0(" Version Reg:        0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_APP_VERSION_REG, &reg);
	VERBOSE0(" Appl. Reg:          0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_AFU_CONFIG_REG, &reg);
	VERBOSE0(" Afu Config Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_AFU_STATUS_REG, &reg);
	VERBOSE0(" Afu Status Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_AFU_COMMAND_REG, &reg);
	VERBOSE0(" Afu Cmd Reg:        0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_FRT_REG, &reg);
	VERBOSE0(" Free Run Timer:     0x%016llx\n", (long long)reg);

	cxl_mmio_read64(afu_h, MMIO_DDCBQ_START_REG, &reg);
	VERBOSE0(" DDCBQ Reg:          0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_CONFIG_REG, &reg);
	VERBOSE0(" DDCBQ Conf Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_COMMAND_REG, &reg);
	VERBOSE0(" DDCBQ Cmd Reg:      0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_STATUS_REG, &reg);
	VERBOSE0(" DDCBQ Stat Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_WT_REG, &reg);
	VERBOSE0(" DDCBQ WT Reg:       0x%016llx\n", (long long)reg);

	for (i = 0; i < MMIO_FIR_REGS_NUM; i++) {
		addr = MMIO_FIR_REGS_BASE + (uint64_t)(i * 8);
		cxl_mmio_read64(afu_h, addr, &reg);
		VERBOSE0(" FIR Reg [%08llx]: 0x%016llx\n",
			 (long long)addr, (long long)reg);
	}
}

static bool afu_clear_firs(struct cxl_afu_h *afu_h)
{
	uint64_t	addr;
	uint64_t	reg;
	int	i;

	for (i = 0; i < MMIO_FIR_REGS_NUM; i++) {
		addr = MMIO_FIR_REGS_BASE + (uint64_t)(i * 8);
		cxl_mmio_read64(afu_h, addr, &reg);
		if (reg != 0ull) {
			/* Pending Firs from prev execution */
			VERBOSE0(" [%08llx]:     0x%016llx\n",
				 (long long)addr, (long long)reg);
			cxl_mmio_write64(afu_h, addr, 0xffffffffffffffffull);
			/* Read again, this time it must be 0 */
			cxl_mmio_read64(afu_h, addr, &reg);
			if (reg != 0ull) {
				VERBOSE0(" [%08llx]:     0x%016llx cannot be cleared!\n",
					(long long)addr, (long long)reg);
				return false;
			}
		}
	}
	return true;
}

/* Init Thread Wait Queue */
static struct tx_waitq* __alloc_waitq(int num)
{
	struct	tx_waitq *txq;
	struct	tx_waitq *q;
	int	i;

	txq = malloc(num * sizeof(struct tx_waitq));
	if (NULL == txq)
		return NULL;

	for (i = 0, q = &txq[0]; i < num; i++, q++) {
		q->status = DDCB_FREE;
		q->cmd = NULL;
		q->ttx = NULL;
		q->thread_wait = false;
	}
	return txq;
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
	uint64_t	mmio_dat;

	ctx->ddcb_num = NUM_DDCBS;
	ctx->ddcb_seqnum = 0xf00d;	/* Starting Seq */
	ctx->ddcb_in = 0;		/* ddcb Input Index */
	ctx->ddcb_out = 0;		/* ddcb Output Index */
	sem_init(&ctx->free_sem, 0, ctx->ddcb_num);

	/* Make the Wait queue for each ddcb */
	ctx->waitq = __alloc_waitq(ctx->ddcb_num);
	if (!ctx->waitq)
		return DDCB_ERR_ENOMEM;

	sprintf(device, "/dev/cxl/afu%d.0d", ctx->card_no);
	ctx->afu_h = cxl_afu_open_dev(device);
	if (NULL == ctx->afu_h) {
		VERBOSE0("Error: cxl_afu_open_dev: %s\n", device);
		rc = DDCB_ERR_CARD;
		goto err_waitq_free;
	}
	ctx->afu_fd = cxl_afu_fd(ctx->afu_h);
	ctx->ddcb = memalign(sysconf(_SC_PAGESIZE),
			     ctx->ddcb_num * sizeof(ddcb_t));
	if (NULL == ctx->ddcb) {
		rc = DDCB_ERR_ENOMEM;
		goto err_afu_free;
	}
	memset(ctx->ddcb, 0, ctx->ddcb_num * sizeof(ddcb_t));

	rc = cxl_afu_attach(ctx->afu_h,
			    (__u64)(unsigned long)(void *)ctx->ddcb);
	if (0 != rc) {
		rc = DDCB_ERR_CARD;
		goto err_ddcb_free;
	}

	if (cxl_mmio_map(ctx->afu_h, CXL_MMIO_BIG_ENDIAN) == -1) {
		VERBOSE0("Error: Unable to map problem state registers");
		rc = DDCB_ERR_CARD;
		goto err_ddcb_free;
	}

	if (afu_clear_firs(ctx->afu_h) == false) {
		VERBOSE0("Error: Unable clear Pending FIRs!\n");
		rc = DDCB_ERR_CARD;
		goto err_mmio_unmap;
	}

	/* | 63..48 | 47....32 | 31........24 | 23....16 | 15.....0 | */
	/* | Seqnum | Reserved | 1st ddcb num | max ddcb | Reserved | */
	mmio_dat = (((uint64_t)ctx->ddcb_seqnum << 48) |
		    ((uint64_t)ctx->ddcb_in  << 24)    |
		    ((uint64_t)(ctx->ddcb_num - 1) << 16));
	rc = cxl_mmio_write64(ctx->afu_h, MMIO_DDCBQ_CONFIG_REG, mmio_dat);
	if (rc != 0) {
		VERBOSE0("Error: Unable to write Config Register");
		rc = DDCB_ERR_CARD;
		goto err_mmio_unmap;
	}

	/* Get MMIO_APP_VERSION_REG */
	cxl_mmio_read64(ctx->afu_h, MMIO_APP_VERSION_REG, &mmio_dat);
	ctx->app_id = mmio_dat;		/* Save it */

	if (libddcb_verbose > 1)
		afu_print_status(ctx->afu_h);

	return DDCB_OK;

 err_mmio_unmap:
	cxl_mmio_unmap(ctx->afu_h);
 err_ddcb_free:
	free(ctx->ddcb);
 err_afu_free:
	cxl_afu_free(ctx->afu_h);
 err_waitq_free:
	free(ctx->waitq);
	return rc;
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 *       ctx->afu_h must be valid
 *       ctx->clients must be 0
 */
static int __afu_close(struct dev_ctx *ctx)
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
		VERBOSE1("WARNING: Trying to close inactive AFU!\n");
		return DDCB_ERR_INVAL;
	}

	if (0 != ctx->clients) {
		VERBOSE0("ERROR: Closing AFU with %d pending clients!\n",
			 ctx->clients);
		return DDCB_ERR_INVAL;
	}

	VERBOSE2("__afu_close %p Afu %d pending opens\n", ctx, ctx->clients);
	mmio_dat = 0x2ull;	/* Stop !! */
	cxl_mmio_write64(afu_h, MMIO_DDCBQ_COMMAND_REG, mmio_dat);
	while (1) {
		cxl_mmio_read64(afu_h, MMIO_DDCBQ_STATUS_REG, &mmio_dat);
		if (0x0ull == (mmio_dat & 0x4))
			break;
		usleep(100);
		i++;
		if (1000 == i) {
			VERBOSE0("ERROR: Timeout wait_afu_stop STATUS_REG: "
				 "0x%016llx\n",	(long long)mmio_dat);
			rc = DDCB_ERR_CARD;
			break;
		}
	}
	if (libddcb_verbose > 1)
		afu_print_status(ctx->afu_h);

	cxl_mmio_unmap(afu_h);
	cxl_afu_free(afu_h);
	ctx->afu_h = NULL;

	/* Free wait queues */
	if (ctx->waitq) {
		free(ctx->waitq);
		ctx->waitq = NULL;
	}
	/* Free DDCB queue */
	if (ctx->ddcb) {
		free(ctx->ddcb);
		ctx->ddcb = NULL;
	}
	return rc;
}

static void afu_dump_queue(struct dev_ctx *ctx)
{
	unsigned int i;
	ddcb_t *ddcb;

	for (i = 0, ddcb = &ctx->ddcb[0]; i < ctx->ddcb_num; i++, ddcb++) {
		VERBOSE0("DDCB %d [%016llx]\n", i, (long long)ddcb);
		ddcb_hexdump(stderr, ddcb, sizeof(ddcb_t));
	}
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 *
 * Open a Card. This needs to be executed only if the device is not
 * yet open.
 */
static int card_dev_open(struct dev_ctx *ctx)
{
	int rc = DDCB_OK;

	/* VERBOSE0("    [%s] clients=%d\n", __func__, ctx->clients); */
	if (ctx->afu_h) {
		VERBOSE0("[%s] ERROR: afu already opened afu=%p clients=%d!\n",
			 __func__, ctx->afu_h, ctx->clients);
		return DDCB_ERR_CARD;
	}

	if (DDCB_OK != __afu_open(ctx)) {
		free(ctx->waitq);
		return DDCB_ERR_CARD;
	}

	/* Now create the worker thread */
	rc = pthread_create(&ctx->ddcb_done_tid, NULL, &__ddcb_done_thread,
			    ctx);
	if (0 != rc) {
		VERBOSE0("[%s] calling __afu_close()\n", __func__);
		__afu_close(ctx);  /* frees waitq and ddcb queue too */
		return DDCB_ERR_ENOMEM;
	}

	ctx->verify = ctx;	/* Set Verify field */
	return DDCB_OK;
}

/**
 * NOTE: ctx->lock must be held when entering this function.
 */
static int card_dev_close(struct dev_ctx *ctx)
{
	void *res = NULL;

	/* VERBOSE0("    [%s] clients=%d\n", __func__, ctx->clients); */
	VERBOSE1("card_dev_close ctx: %p Clients: %d left\n",
		 ctx, ctx->clients);

	if (0 != ctx->clients) {
		VERBOSE0("ERROR: Trying to close device which is in use!\n");
		return DDCB_ERR_INVAL;
	}

	pthread_cancel(ctx->ddcb_done_tid);
	pthread_join(ctx->ddcb_done_tid, &res);

	/* VERBOSE0("[%s] calling __afu_close()\n", __func__); */
	__afu_close(ctx);

	return DDCB_OK;
}

static int __client_inc(struct dev_ctx *ctx)
{
	int rc = DDCB_OK;

	pthread_mutex_lock(&ctx->lock);
	/* VERBOSE0("  [%s] clients=%d\n", __func__, ctx->clients); */
	if (ctx->clients == 0)
		rc = card_dev_open(ctx);
	ctx->clients++;
	pthread_mutex_unlock(&ctx->lock);

	return rc;
}

static void __client_dec(struct dev_ctx *ctx)
{
	pthread_mutex_lock(&ctx->lock);
	/* VERBOSE0("  [%s] clients=%d\n", __func__, ctx->clients); */
	ctx->clients--;
	if (ctx->clients == 0)
		card_dev_close(ctx);
	pthread_mutex_unlock(&ctx->lock);
}

static void *card_open(int card_no,
		       unsigned int mode __attribute__((unused)),
		       int *card_rc,
		       uint64_t appl_id __attribute__((unused)),
		       uint64_t appl_id_mask __attribute__((unused)))
{
	int rc = DDCB_OK;
	struct ttxs *ttx = NULL;

	/* VERBOSE0("[%s]\n", __func__); */

	/* Allocate Thread Context */
	ttx = calloc(1, sizeof(*ttx));
	if (!ttx) {
		rc = DDCB_ERR_ENOMEM;
		goto card_open_exit;
	}

	/* Inc use count and initialize AFU on first open */
	my_ctx.card_no = card_no; /* remember card number */
	ttx->ctx = &my_ctx;
	ttx->verify = ttx;
	sem_init(&ttx->wait_sem, 0, 0);

	rc = __client_inc(ttx->ctx);
	if (rc != DDCB_OK) {
		free(ttx);
		ttx = NULL;
	}

 card_open_exit:
	if (card_rc)
		*card_rc = rc;
	return ttx;
}

static int card_close(void *card_data)
{
	struct ttxs *ttx = (struct ttxs*)card_data;
	struct dev_ctx *ctx;

	/* VERBOSE0("[%s]\n", __func__); */
	if (NULL == ttx)
		return DDCB_ERR_INVAL;

	if (ttx->verify != ttx)
		return DDCB_ERR_INVAL;

	ctx = ttx->ctx;
	__client_dec(ctx);

	ttx->verify = NULL;
	free(ttx);

	rt_trace_dump();
	return DDCB_OK;
}

static void start_ddcb(struct	cxl_afu_h *afu_h, int seq)
{
	uint64_t	reg;

	reg = (uint64_t)seq << 48 | 1;	/* Set Seq. Number + Start Bit */
	cxl_mmio_write64(afu_h, MMIO_DDCBQ_COMMAND_REG, reg);
}

/*	set command into next DDCB Slot */
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
	my_cmd = cmd;

	while (my_cmd) {
		sem_getvalue(&ctx->free_sem, &val);
		rt_trace(0x00a0, -1, val, ttx);
		sem_wait(&ctx->free_sem);

		pthread_mutex_lock(&ctx->lock);
		idx = ctx->ddcb_in;
		ddcb = &ctx->ddcb[idx];
		txq = &ctx->waitq[idx];
		txq->ttx = ttx;			/* set ttx pointer into txq */
		txq->status = DDCB_IN;
		seq = (int)ctx->ddcb_seqnum;	/* Get seq */
		txq->cmd = my_cmd;		/* my command to txq */
		txq->seqnum = ctx->ddcb_seqnum;	/* Save seq Number */
		ctx->ddcb_seqnum++;		/* Next seq */
		rt_trace(0x00a1, seq, idx, ttx);
		VERBOSE2("__ddcb_execute seq: 0x%x slot: %d cmd: %p\n",
			seq, idx, my_cmd);
		/* Increment ddcb_in and warp back to 0 */
		ctx->ddcb_in = (ctx->ddcb_in + 1) % ctx->ddcb_num;
		cmd_2_ddcb(ddcb, cmd, seq);
		start_ddcb(ctx->afu_h, seq);
		/* Get  Next cmd and continue if there is one */
		my_cmd = (struct ddcb_cmd *)my_cmd->next_addr;
		if (NULL == my_cmd)
			txq->thread_wait = true;
		pthread_mutex_unlock(&ctx->lock);
	}

	/* Block Caller */
	VERBOSE2("__ddcb_execute Wait ttx: %p\n", ttx);
	sem_wait(&ttx->wait_sem);
	rt_trace(0x00af, ttx->seqnum, idx, ttx);
	VERBOSE2("__ddcb_execute return ttx: %p\n", ttx);
	return ttx->compl_code;	/* Give Completion code back to caller */
}

static int ddcb_execute(void *card_data, struct ddcb_cmd *cmd)
{
	int rc;

	rc = __ddcb_execute_multi(card_data, cmd);
	if (DDCB_OK != rc)
		errno = EINTR;
	return rc;
}

static bool __ddcb_done_post(struct dev_ctx *ctx, int compl_code)
{
	int	idx;
	ddcb_t	*ddcb;
	struct	tx_waitq	*txq;
	struct	ttxs		*ttx;

	idx = ctx->ddcb_out;
	ddcb = &ctx->ddcb[idx];
	txq = &ctx->waitq[idx];
	if (libddcb_verbose > 3) {
		VERBOSE0("DDCB %d [%016llx] after execution "
			 "compl_code: %d ddcb->retc16: %4.4x\n",
			idx, (long long)ddcb, compl_code, ddcb->retc_16);
		ddcb_hexdump(stderr, ddcb, sizeof(ddcb_t));
	}

	if (DDCB_OK == compl_code) {
		if (0 == ddcb->retc_16) {
			VERBOSE2("\t__ddcb_done_thread seq: 0x%x slot: %d "
				 "retc: 0 wait\n", txq->seqnum, idx);
			return false; /* do not continue */
		}
	}

	if (DDCB_IN == txq->status) {
		ttx = txq->ttx;
		VERBOSE1("\t__ddcb_done_thread seq: 0x%x slot: %d ttx: %p\n",
			txq->seqnum, idx, ttx);
		ddcb_2_cmd(ddcb, txq->cmd);	/* Copy DDCB back to CMD */
		ttx->compl_code = compl_code;
		VERBOSE2("\t__ddcb_done_thread seq: 0x%x slot: %d "
			 "cmd: %p compl_code: %d\n",
			 txq->seqnum, idx, txq->cmd, compl_code);
		rt_trace(0x0011, txq->seqnum, idx, ttx);
		sem_post(&ctx->free_sem);
		if (txq->thread_wait) {
			rt_trace(0x0012, txq->seqnum, idx, ttx);
			VERBOSE2("\t__ddcb_done_thread Post: %p\n", ttx);
			sem_post(&ttx->wait_sem);
			txq->thread_wait = false;
		}

		/* Increment and wrap back to start */
		ctx->ddcb_out = (ctx->ddcb_out + 1) % ctx->ddcb_num;
		txq->status = DDCB_FREE;
		return true;		/* Continue */
	}
	return false;			/* do not continue */
}

static void *__ddcb_done_thread(void *card_data)
{
	struct dev_ctx *ctx = (struct dev_ctx *)card_data;
	fd_set	set;
	struct	timeval timeout;
	int	rc = 0;

	while (1) {
		FD_ZERO(&set);
		FD_SET(ctx->afu_fd, &set);

		/* Set timeout to "tout" seconds */
		timeout.tv_sec = ctx->tout;
		timeout.tv_usec = 0;
		rc = select(ctx->afu_fd + 1, &set, NULL, NULL, &timeout);

		if (0 == rc) {
			VERBOSE2("WARNING: %d sec timeout while waiting "
				 "for interrupt! rc: %d --> %d\n",
				 ctx->tout, rc, DDCB_ERR_IRQTIMEOUT);
			__ddcb_done_post(ctx, DDCB_ERR_IRQTIMEOUT);
			rt_trace_dump();
			continue;
		}
		if ((rc == -1) && (errno == EINTR)) {
			VERBOSE0("WARNING: select returned -1 "
				 "and errno was EINTR, retrying\n");
			continue;
		}

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
			afu_print_status(ctx->afu_h);
			__ddcb_done_post(ctx, DDCB_ERR_SELECTFAIL);
			continue;
		}

		rc = cxl_read_event(ctx->afu_h, &ctx->event);
		if (0 != rc) {
			VERBOSE0("\tERROR cxl_read_event() rc: %d errno: %d\n",
				 rc, errno);
			continue;
		}
		VERBOSE2("\tcxl_read_event(...) = %d\n"
			"\tevent.header.type = %d event.header.size = %d\n",
			rc, ctx->event.header.type, ctx->event.header.size);

		pthread_mutex_lock(&ctx->lock);
		switch (ctx->event.header.type) {
		case CXL_EVENT_AFU_INTERRUPT:
			/* Process all ddcb's */
			VERBOSE2("\tCXL_EVENT_AFU_INTERRUPT: flags: 0x%x "
				 "irq: 0x%x\n",
				ctx->event.irq.flags,
				ctx->event.irq.irq);
			while (__ddcb_done_post(ctx, DDCB_OK)) {};
			break;
		case CXL_EVENT_DATA_STORAGE:
			VERBOSE0("\tCXL_EVENT_DATA_STORAGE: flags: 0x%x "
				 "addr: 0x%016llx dsisr: 0x%016llx\n",
				ctx->event.fault.flags,
				(long long)ctx->event.fault.addr,
				(long long)ctx->event.fault.dsisr);
			afu_print_status(ctx->afu_h);
			afu_dump_queue(ctx);
			__ddcb_done_post(ctx, DDCB_ERR_EVENTFAIL);
			break;
		case CXL_EVENT_AFU_ERROR:
			VERBOSE0("\tCXL_EVENT_AFU_ERROR: flags: 0x%x "
				 "error: 0x%016llx\n",
				ctx->event.afu_error.flags,
				(long long)ctx->event.afu_error.error);
			afu_print_status(ctx->afu_h);
			__ddcb_done_post(ctx, DDCB_ERR_EVENTFAIL);
			break;
		default:
			VERBOSE0("\tcxl_read_event() %d unknown header type\n",
				ctx->event.header.type);
			__ddcb_done_post(ctx, DDCB_ERR_EVENTFAIL);
			break;
		}
		pthread_mutex_unlock(&ctx->lock);
	}
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

static uint64_t _card_get_app_id(void *card_data)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx && (ttx->verify == ttx)) {
		ctx = ttx->ctx;
		if (ctx)
			return ctx->app_id;
	}
	return DDCB_ERR_INVAL;
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

static struct ddcb_accel_funcs accel_funcs = {
	.card_type = DDCB_TYPE_CAPI,
	.card_open = card_open,
	.card_close = card_close,
	.ddcb_execute = ddcb_execute,
	.card_strerror = _card_strerror,
	.card_read_reg64 = card_read_reg64,
	.card_read_reg32 = card_read_reg32,
	.card_write_reg64 = card_write_reg64,
	.card_write_reg32 = card_write_reg32,
	.card_get_app_id = _card_get_app_id,
	.card_pin_memory = card_pin_memory,
	.card_unpin_memory = card_unpin_memory,
	.card_malloc = card_malloc,
	.card_free = card_free,
	.priv_data = NULL,
};

static void capi_card_init(void) __attribute__((constructor));
static void capi_card_exit(void) __attribute__((destructor));

static void capi_card_init(void)
{
	const char *ttt = getenv("DDCB_TIMEOUT");
	struct	dev_ctx *ctx = &my_ctx;
	int	rc;

	/* VERBOSE0("[%s]\n", __func__); */
	memset(ctx, 0, sizeof(*ctx));
	ctx->tout = 5;		/* Set timeout to 5 sec */
	if (ttt)
		ctx->tout = strtoul(ttt, (char **) NULL, 0);

	rt_trace_init();
	rc = pthread_mutex_init(&ctx->lock, NULL);
	if (0 != rc) {
		VERBOSE0("Error: initializing mutex failed!\n");
		return;
	}
	ddcb_register_accelerator(&accel_funcs);
}

static void capi_card_exit(void)
{
	/* struct dev_ctx *ctx = &my_ctx; */

	/*
	 *  We normally close the AFU when the refrence count drops to
	 *  zero. Trying to do __afu_close() is not helpful. Worker
	 *  thread will die with the process too.
	 */
	/* VERBOSE0("[%s] calling __afu_close()\n", __func__); */
	/* __afu_close(ctx); */
}
