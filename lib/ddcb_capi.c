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

#define MMIO_IMP_VERSION_REG 0x3F00000ull
#define MMIO_APP_VERSION_REG 0x3F00008ull
#define MMIO_DDCBQ_START_REG 0x3F00010ull
#define MMIO_CONFIG_REG      0x3F00018ull
#define MMIO_CONTROL_REG     0x3F00020ull
#define MMIO_STATUS_REG      0x3F00028ull
#define MMIO_SCRATCH_REG     0x3FFFF98ull

#define	NUM_DDCBS	16

extern int libddcb_verbose;
#define VERBOSE0(...) {fprintf(stderr, __VA_ARGS__);}
#define VERBOSE1(...) {if (libddcb_verbose > 0) fprintf(stderr, __VA_ARGS__);}
#define VERBOSE2(...) {if (libddcb_verbose > 1) fprintf(stderr, __VA_ARGS__);}
#define VERBOSE3(...) {if (libddcb_verbose > 3) fprintf(stderr, __VA_ARGS__);}

static void *__ddcb_done_thread(void *card_data);

/**
 * Each CAPI compression card has one AFU, which provides one ddcb
 * queue per process. Multiple threads within one process share the
 * ddcb queue. Locking is needed to ensure that this works race free.
 */
struct ttxs {
	struct	dev_ctx	*ctx;	/* Pointer to Card Context */
	struct	ttxs	*verify;
};

enum waitq_status {DDCB_FREE, DDCB_IN, DDCB_OUT, DDCB_ERR};
/* Thread wait Queue, allocate one entry per ddcb */
struct  tx_waitq {
	int	compl_code;		/* Completion Code */
	enum	waitq_status status;
	int	seqnum;
	bool	wait_sem;
	sem_t	sem;
	struct	ddcb_cmd	*cmd;;
};

/**
 * A a device context is normally bound to a card which provides a
 * ddcb queue. Whenever a new context is created a queue is attached
 * to it. Whenever it is removed the queue is removed too. There can
 * be multiple contexts using just one card.
 */
struct dev_ctx {
	bool		dev_open;	/* Flag set to True if active */
	int		card_no;
	pthread_mutex_t	lock;
	int		clients;	/* Thread open counter */
	struct		cxl_afu_h	*afu_h;
	int		afu_fd;
	uint16_t	ddcb_seqnum;
	uint16_t	ddcb_free1;	/* Not used */
	int		ddcb_num;	/* How deep is my ddcb queue */
	int		ddcb_out;	/* ddcb Output (done) index */
	int		ddcb_in;	/* ddcb Input index */
	ddcb_t		*ddcb;		/* Pointer to the ddcb queue */
	struct		tx_waitq	*waitq;
	struct		cxl_event	event;	/* last AFU event */
	int		tout;		/* Timeout Value for compeltion */
	pthread_t	ddcb_done_tid;
	uint64_t	app_id;		/* a copy of MMIO_APP_VERSION_REG */
	int		busy_wait;	/* How many waiting threads in busy sem */
	sem_t		busy_sem;	/* bussy sem if ddcb q is full */
	struct		dev_ctx		*verify;	/* Verify field */
};

static struct dev_ctx my_ctx;	/* My Card */

/*	Command to ddcb */
static inline void cmd_2_ddcb(ddcb_t *pddcb, struct ddcb_cmd *cmd,
			      uint16_t seqnum)
{
	//uint16_t icrc;

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

	pddcb->retc_16 = 0;
	if (libddcb_verbose > 3) {
		VERBOSE0("DDCB [%016llx] Seqnum 0x%x before execution:\n",
			(long long)(unsigned long)(void *)pddcb, seqnum);
		ddcb_hexdump(stderr, pddcb, sizeof(ddcb_t));
	}
	/* Note: setup seqnum as last field */
	pddcb->seqnum = __cpu_to_be16(seqnum);
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
	if (0 != ddcb->deque_ts_64) {
		VERBOSE0("DDCB [%016llx] Seqnum 0x%x bad:\n",
			(long long)(unsigned long)(void *)ddcb, __be16_to_cpu(ddcb->seqnum));
		ddcb_hexdump(stderr, ddcb, sizeof(ddcb_t));
	}
}

static void afu_print_status(struct cxl_afu_h *afu_h)
{
	uint64_t	reg;

	cxl_mmio_read64(afu_h, MMIO_IMP_VERSION_REG, &reg);
	VERBOSE0(" Version Reg:    0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_APP_VERSION_REG, &reg);
	VERBOSE0(" Appl. Reg:      0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_DDCBQ_START_REG, &reg);
	VERBOSE0(" Ddcbq Start Reg:0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_CONFIG_REG, &reg);
	VERBOSE0(" Mmio Conf. Reg: 0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_CONTROL_REG, &reg);
	VERBOSE0(" Mmio Cont. Reg: 0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_STATUS_REG, &reg);
	VERBOSE0(" Status Reg:     0x%016llx\n", (long long)reg);
	cxl_mmio_read64(afu_h, MMIO_SCRATCH_REG, &reg);
	VERBOSE0(" Scratch Reg:    0x%016llx\n", (long long)reg);
}

static int __afu_open(struct dev_ctx *ctx)
{
	int	rc = DDCB_OK;
	char	device[64];
	uint64_t mmio_dat;

	sprintf(device, "/dev/cxl/afu%d.0d", ctx->card_no);
	ctx->afu_h = cxl_afu_open_dev(device);
	if (NULL == ctx->afu_h) {
		VERBOSE0("cxl_afu_open_dev: %s\n", device);
		return DDCB_ERR_CARD;
	}
	ctx->afu_fd = cxl_afu_fd(ctx->afu_h);
	ctx->ddcb = memalign(sysconf(_SC_PAGESIZE), ctx->ddcb_num * sizeof(ddcb_t));
	if (NULL == ctx->ddcb) {
		rc = DDCB_ERR_ENOMEM;
		goto err_afu_free;
	}
	memset(ctx->ddcb, 0, ctx->ddcb_num * sizeof(ddcb_t));

	rc = cxl_afu_attach(ctx->afu_h,
			    (__u64)(unsigned long)(void *)ctx->ddcb);
	if (0 != rc) {
		rc = DDCB_ERR_CARD;
		goto err_free_ddcb;
	}

	if (cxl_mmio_map(ctx->afu_h, CXL_MMIO_BIG_ENDIAN) == -1) {
		VERBOSE0("Unable to map problem state registers");
		rc = DDCB_ERR_CARD;
		goto err_free_ddcb;
	}

	mmio_dat = (((uint64_t)ctx->ddcb_seqnum << 48) |
		    ((uint64_t)(ctx->ddcb_num - 1) << 40) |
		    ((uint64_t)ctx->ddcb_in  << 32));
	rc = cxl_mmio_write64(ctx->afu_h, MMIO_CONFIG_REG, mmio_dat);
	if (rc != 0) {
		rc = DDCB_ERR_CARD;
		goto err_mmio_unmap;
	}

	/* Get MMIO_APP_VERSION_REG */
	cxl_mmio_read64(ctx->afu_h, MMIO_APP_VERSION_REG, &mmio_dat);
	ctx->app_id = mmio_dat;		/* Save it */

	mmio_dat = 0x1ull;
	rc = cxl_mmio_write64(ctx->afu_h, MMIO_CONTROL_REG, mmio_dat);
	if (rc != 0) {
		rc = DDCB_ERR_CARD;
		goto err_mmio_unmap;
	}
	if (libddcb_verbose > 1)
		afu_print_status(ctx->afu_h);

	return DDCB_OK;

 err_mmio_unmap:
	cxl_mmio_unmap(ctx->afu_h);
 err_free_ddcb:
	free(ctx->ddcb);
 err_afu_free:
	cxl_afu_free(ctx->afu_h);
	return rc;
}

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
	if (NULL == afu_h)
		return DDCB_OK;
	if (false == ctx->dev_open)
		perror("Closing Afu without open\n");
	if (0 != ctx->clients)
		VERBOSE0("ERROR: Closing Afu while %d pending opens !\n", ctx->clients);
	VERBOSE2("__afu_close %p Afu %d pending opens\n", ctx, ctx->clients);
	mmio_dat = 0x2ull;
	cxl_mmio_write64(afu_h, MMIO_CONTROL_REG, mmio_dat);
	while (1) {
		cxl_mmio_read64(afu_h, MMIO_STATUS_REG, &mmio_dat);
		if (0x02ull == (mmio_dat & 0x3))
			break;
		usleep(100);
		i++;
		if (1000 == i) {
			VERBOSE0("ERROR: Timeout wait_afu_stop STATUS_REG: 0x%016llx\n",
				(long long)mmio_dat);
			rc = DDCB_ERR_CARD;
			break;
		}
	}
	if (libddcb_verbose > 1)
		afu_print_status(ctx->afu_h);
	cxl_mmio_unmap(afu_h);
	cxl_afu_free(afu_h);
	ctx->afu_h = NULL;
	ctx->dev_open = false;
	ctx->clients = 0;
	return rc;
}

/*	Init Thread Wait Queue */
static struct tx_waitq* __init_waitq(int num)
{
	struct tx_waitq *txq = malloc(num * sizeof(struct tx_waitq));
	struct tx_waitq *tx;
	int	i;

	if (txq) {
		tx =  &txq[0];
		for (i = 0; i < num; i++) {
			sem_init(&tx->sem, 0, 0);
			tx->status = DDCB_FREE;
			tx->seqnum = 0;
			tx->cmd = NULL;
			tx->wait_sem = false;
			tx->compl_code = 0;
			tx++;
		}
	}
	return txq;
}


/* Open a Card. This will execute one time only  */
static int card_dev_open(int card_no)
{
	int rc = DDCB_OK;
	struct dev_ctx *ctx = &my_ctx;
	pthread_t tid;

	pthread_mutex_lock(&ctx->lock);
	if (ctx->dev_open) {
		pthread_mutex_unlock(&ctx->lock);
		return rc;
	}
	ctx->card_no = card_no;
	ctx->tout = 5;			/* Set timeout to 5 sec on HW, and 5 min on SIM */
	const char *ttt = getenv("DDCB_TIMEOUT");
	if (ttt)
		ctx->tout = strtoul(ttt, (char **) NULL, 0);
	ctx->ddcb_num = NUM_DDCBS;
	ctx->ddcb_seqnum = 0xf00d;	/* Starting Seq */
	ctx->ddcb_in = 0;		/* ddcb Input Index */
	ctx->ddcb_out = 0;		/* ddcb Output Index */
	ctx->busy_wait = 0,
	sem_init(&ctx->busy_sem, 0, 0);
	/* Make the Wait queue for each ddcb */
	ctx->waitq = __init_waitq(ctx->ddcb_num);
	if (ctx->waitq) {
		if (DDCB_OK == __afu_open(ctx)) {
			ctx->verify = ctx;		/* Set Verify field */
			ctx->dev_open = true;		/* Open done */
			rc = pthread_create(&tid, NULL, &__ddcb_done_thread, ctx);
			if (0 == rc) {
				ctx->ddcb_done_tid = tid;
				ctx->dev_open = true;	/* Set to done */
				ctx->clients = 0;
				rc = DDCB_OK;
			} else {
				__afu_close(ctx);
				free(ctx->waitq);
				rc =  DDCB_ERR_ENOMEM;
			}
		} else {
			free(ctx->waitq);
			rc = DDCB_ERR_CARD;
		}
	} else rc = DDCB_ERR_ENOMEM;
	pthread_mutex_unlock(&ctx->lock);
	return rc;
}

static void __client_inc(struct dev_ctx *ctx)
{
	pthread_mutex_lock(&ctx->lock);
	ctx->clients++;
	pthread_mutex_unlock(&ctx->lock);
}

static void __client_dec(struct dev_ctx *ctx)
{
	pthread_mutex_lock(&ctx->lock);
	ctx->clients--;
	pthread_mutex_unlock(&ctx->lock);
}

static void *card_open(int card_no,
		       unsigned int mode __attribute__((unused)),
		       int *card_rc,
		       uint64_t appl_id __attribute__((unused)),
		       uint64_t appl_id_mask __attribute__((unused)))
{
	int rc = DDCB_OK;
	struct ttxs *ttx;

	/* Check if Card is open or Open Card */
	rc = card_dev_open(card_no);
	if (DDCB_OK != rc) {
		if (card_rc)
			*card_rc = rc;
		return NULL;
	}
	/* Allocate Thread Context */
	ttx = calloc(1, sizeof(*ttx));
	if (ttx) {
		ttx->ctx = &my_ctx;
		ttx->verify = ttx;
		__client_inc(ttx->ctx);
	} else rc = DDCB_ERR_ENOMEM;
	if (card_rc)
		*card_rc = rc;
	return ttx;
}

static int card_close(void *card_data)
{
	struct ttxs *ttx = (struct ttxs*)card_data;
	struct dev_ctx *ctx;
	void *res = NULL;

	if (NULL == ttx)
		return DDCB_ERR_INVAL;
	if (ttx->verify != ttx)
		return DDCB_ERR_INVAL;
	ctx = ttx->ctx;
	__client_dec(ctx);
	free(ttx);

	VERBOSE2("card_close ctx: %p Clients: %d\n", ctx, ctx->clients);

	if (0 == ctx->clients) {
		pthread_cancel(ctx->ddcb_done_tid);
		pthread_join(ctx->ddcb_done_tid, &res);
		__afu_close(ctx);
	}
	VERBOSE2("card_close EXIT\n");
	return DDCB_OK;
}

/*	set command into next DDCB Slot */
static int __ddcb_execute_multi(void *card_data, struct ddcb_cmd *cmd)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx	*ctx = NULL;
	struct	tx_waitq *txq = NULL;
	ddcb_t	*ddcb;
	int	idx = 0;
	int	seq;
	struct ddcb_cmd *my_cmd = cmd;

	if (NULL == ttx)
		return DDCB_ERR_INVAL;
	if (ttx->verify != ttx)
		return DDCB_ERR_INVAL;
	if (NULL == cmd)
		return DDCB_ERR_INVAL;
	ctx = ttx->ctx;	/* get card Context */
	while (my_cmd) {
		pthread_mutex_lock(&ctx->lock);
		idx = ctx->ddcb_in;
		ddcb = &ctx->ddcb[idx];
		txq = &ctx->waitq[idx];
		if (DDCB_FREE == txq->status) {
			txq->status = DDCB_IN;
			seq = (int)ctx->ddcb_seqnum;	/* Get seq */
			txq->seqnum = seq;		/* Set seq into thread ctx */
			txq->cmd = my_cmd;
			ctx->ddcb_seqnum++;		/* Next seq */
			VERBOSE2("__ddcb_execute seq: 0x%x slot: %d cmd: %p\n",
				seq, idx, my_cmd);
			/* Increment ddcb_in and warp back to 0 */
			ctx->ddcb_in = (ctx->ddcb_in + 1) % ctx->ddcb_num;
			/* Check for Next cmd */
			my_cmd = (struct ddcb_cmd *)my_cmd->next_addr;
			if (NULL == my_cmd)
				txq->wait_sem = true;
			pthread_mutex_unlock(&ctx->lock);
			cmd_2_ddcb(ddcb, cmd, seq);
		} else {
			ctx->busy_wait++;
			pthread_mutex_unlock(&ctx->lock);
			sem_wait(&ctx->busy_sem);
		}
	}
	sem_wait(&txq->sem);		/* And Block */
	return txq->compl_code;		/* Give Completion code back to caller */
}

static int ddcb_execute(void *card_data, struct ddcb_cmd *cmd)
{
	int rc;

	rc = __ddcb_execute_multi(card_data, cmd);
	return rc;
}

static bool __ddcb_done_post(struct dev_ctx *ctx, int compl_code)
{
	int	idx;
	ddcb_t	*ddcb;
	struct	tx_waitq	*txq;

	idx = ctx->ddcb_out;
	ddcb = &ctx->ddcb[idx];
	txq = &ctx->waitq[idx];
	if (libddcb_verbose > 3) {
		VERBOSE0("DDCB %d [%016llx] after execution compl_code: %d\n",
			idx, (long long)ddcb, compl_code);
		ddcb_hexdump(stderr, ddcb, sizeof(ddcb_t));
	}

	if (DDCB_OK == compl_code) {
		if (0 == ddcb->retc_16) {
			VERBOSE2("\t__ddcb_done_thread seq: 0x%x slot: %d retc d wait\n",
				txq->seqnum, idx);
			return false;		/* do not continue */
		}
	}
	if (DDCB_IN == txq->status) {
		VERBOSE2("\t__ddcb_done_thread seq: 0x%x slot: %d cmd: %p\n",
			txq->seqnum, idx, txq->cmd);
		pthread_mutex_lock(&ctx->lock);
		ddcb_2_cmd(ddcb, txq->cmd);
		ddcb->retc_16 = 0;
		if (txq->wait_sem) {
			txq->compl_code = compl_code;
			VERBOSE2("\t__ddcb_done_thread seq: 0x%x slot: %d cmd: %p POST rc: %d\n",
				txq->seqnum, idx, txq->cmd, compl_code);
			sem_post(&txq->sem);
			txq->wait_sem = false;
		}
		/* Increment and wrap back to start */
		ctx->ddcb_out = (ctx->ddcb_out + 1) % ctx->ddcb_num;
		txq->status = DDCB_FREE;
		while (ctx->busy_wait) {
			VERBOSE2("\t__ddcb_done_thread post busy_wait %d\n",
				ctx->busy_wait);
			sem_post(&ctx->busy_sem);
			ctx->busy_wait--;
		}
		pthread_mutex_unlock(&ctx->lock);
		return true;		/* Continue */
	}
	VERBOSE0("\t__ddcb_done_thread FIXME\n");
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
			VERBOSE0("WARNING: %d sec timeout while waiting for interrupt! "
				"rc: %d --> %d\n", ctx->tout, rc, DDCB_ERR_IRQTIMEOUT);
			__ddcb_done_post(ctx, DDCB_ERR_IRQTIMEOUT);
			continue;
		}
		if (rc < 0) {
			VERBOSE0("ERROR: waiting for interrupt! rc: %d\n", rc);
			afu_print_status(ctx->afu_h);
			continue;
		}

		rc = cxl_read_expected_event(ctx->afu_h, &ctx->event, CXL_EVENT_AFU_INTERRUPT, 1);
		if (0 != rc)
			VERBOSE0("failed reading expected event! rc: %d\n", rc);
		VERBOSE2("\tcxl_read_expected_event(...) = %d\n"
			"  event.header.type = %d event.header.size = %d\n",
			rc, ctx->event.header.type, ctx->event.header.size);

		/* Process all ddcb's */
		while (__ddcb_done_post(ctx, DDCB_OK)) {};
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

	if (ttx) {
		if (ttx->verify == ttx) {
			ctx = ttx->ctx;
			if (ctx->afu_h) {
				rc = cxl_mmio_read64(ctx->afu_h, offs, &data);
				if (card_rc)
					*card_rc = rc;
				return data;
			}
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

	if (ttx) {
		if (ttx->verify == ttx) {
			ctx = ttx->ctx;
			if (ctx->afu_h) {
				rc = cxl_mmio_read32(ctx->afu_h, offs, &data);
				if (card_rc)
					*card_rc = rc;
				return data;
			}
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

	if (ttx) {
		if (ttx->verify == ttx) {
			ctx = ttx->ctx;
			if (ctx->afu_h)
				return cxl_mmio_write64(ctx->afu_h, offs, data);
		}
	}
	return DDCB_ERR_INVAL;
}

static int card_write_reg32(void *card_data, uint32_t offs, uint32_t data)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx) {
		if (ttx->verify == ttx) {
			ctx = ttx->ctx;
			if (ctx->afu_h)
				return cxl_mmio_write32(ctx->afu_h, offs, data);
		}
	}
	return DDCB_ERR_INVAL;
}

static uint64_t _card_get_app_id(void *card_data)
{
	struct	ttxs	*ttx = (struct ttxs*)card_data;
	struct	dev_ctx *ctx;

	if (ttx) {
		if (ttx->verify == ttx) {
			ctx = ttx->ctx;
			if (ctx)
				return ctx->app_id;
		}
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
	struct	dev_ctx *ctx = &my_ctx;
	int	rc;

	rc = pthread_mutex_init(&ctx->lock, NULL);
	if (0 != rc) {
		VERBOSE0("Error: initializing mutex failed!\n");
		return;
	}
	ctx->dev_open = false;
	ddcb_register_accelerator(&accel_funcs);
}

static void capi_card_exit(void)
{
	struct dev_ctx *ctx = &my_ctx;

	__afu_close(ctx);
}
