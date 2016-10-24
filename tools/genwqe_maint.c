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
 * Genwqe Capi Card Master Maintenance tool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <endian.h>
#include <sys/stat.h>

#include <libddcb.h>
#include <libcxl.h>
#include "afu_regs.h"

/*
 * This needs to be in sync with the max number of cards supported in
 * ddcb_capi.c.
 */
#define NUM_CARDS 4 /* max number of CAPI cards in system */

static const char *version = GIT_VERSION;
static int verbose = 0;
static FILE *fd_out;

#define VERBOSE0(fmt, ...) do {					\
		fprintf(fd_out, fmt, ## __VA_ARGS__);		\
	} while (0)

#define VERBOSE1(fmt, ...) do {					\
		if (verbose > 0)				\
			fprintf(fd_out, fmt, ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE2(fmt, ...) do {					\
		if (verbose > 1)				\
			fprintf(fd_out, fmt, ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE3(fmt, ...) do {					\
		if (verbose > 2)				\
			fprintf(fd_out, fmt, ## __VA_ARGS__);	\
	} while (0)

struct mdev_ctx {
	int loop;		/* Loop Counter */
	int card;		/* Card no (0,1,2,3 */
	struct cxl_afu_h *afu_h;/* The AFU handle */
	int dt;			/* Delay time in sec (1 sec default) */
	int count;		/* Number of loops to do, (-1) = forever */
	bool daemon;		/* TRUE if forked */
	uint64_t wed;		/* This is a dummy only for attach */
	bool quiet;		/* False or true -q option */
	pid_t pid;
	pid_t my_sid;		/* for sid */
	int mode;		/* See below */
	size_t errinfo_size;
	char *errinfo;

	uint64_t fir[MMIO_FIR_REGS_NUM];
};

struct cgzip_afu_fir {
	__be32 fir_val;
	__be32 fir_addr;
};

/*	Expect min this Release or higher */
#define	MIN_REL_VERSION	0x0601

/* Mode Bits for Master Loop */
#define	CHECK_FIRS_MODE		0x0001	/* Mode 1 */
#define	CHECK_TIME_MODE		0x0002	/* Mode 2 */

static struct mdev_ctx	master_ctx;

#if 0
static int mmio_write(struct cxl_afu_h *afu_h, int ctx, uint32_t offset,
		      uint64_t data)
{
	int rc = -1;
	uint32_t offs = (ctx * MMIO_CTX_OFFSET) + offset;

	VERBOSE3("[%s] Enter, Offset: 0x%x data: 0x%016llx\n",
		__func__, offs, (long long)data);
	rc = cxl_mmio_write64(afu_h, offs, data);
	VERBOSE3("[%s] Exit, rc = %d\n", __func__, rc);
	return rc;
}
#endif

static int mmio_read(struct cxl_afu_h *afu_h, int ctx, uint32_t offset,
		     uint64_t *data)
{
	int rc = -1;
	uint32_t offs = (ctx * MMIO_CTX_OFFSET) + offset;

	VERBOSE3("[%s] Enter, CTX: %d Offset: 0x%x\n", __func__, ctx, offs);
	rc = cxl_mmio_read64(afu_h, offs, data);
	VERBOSE3("[%s] Exit, rc = %d data: 0x%016llx\n",
		__func__, rc, (long long)*data);
	return rc;
}

/*
 * Open AFU Master Device
 */
static int afu_m_open(struct mdev_ctx *mctx)
{
	int rc = 0;
	char device[64];
	long api_version, cr_device, cr_vendor;

	sprintf(device, "/dev/cxl/afu%d.0m", mctx->card);
	VERBOSE3("[%s] Enter, Open Device: %s\n", __func__, device);
	mctx->afu_h = cxl_afu_open_dev(device);
	if (NULL == mctx->afu_h) {
		VERBOSE0("[%s] Exit, Card Open error rc: %d\n", __func__, rc);
		return -1;
	}

	/* Check if the compiled in API version is compatible with the
	   one reported by the kernel driver */
	rc = cxl_get_api_version_compatible(mctx->afu_h, &api_version);
	if ((rc != 0) || (api_version != CXL_KERNEL_API_VERSION)) {
		VERBOSE0(" [%s] ERR: incompatible API version: %ld/%d rc=%d\n",
			 __func__, api_version, CXL_KERNEL_API_VERSION, rc);
		rc = -2;
		goto err_afu_free;
	}

	/* Check vendor id */
	rc = cxl_get_cr_vendor(mctx->afu_h, 0, &cr_vendor);
	if ((rc != 0) || (cr_vendor != CGZIP_CR_VENDOR)) {
		VERBOSE0(" [%s] ERR: vendor_id: %ld/%d rc=%d\n",
			 __func__, (unsigned long)cr_vendor,
			 CGZIP_CR_VENDOR, rc);
		rc = -3;
		goto err_afu_free;
	}

	/* Check device id */
	rc = cxl_get_cr_device(mctx->afu_h, 0, &cr_device);
	if ((rc != 0) || (cr_device != CGZIP_CR_DEVICE)) {
		VERBOSE0(" [%s] ERR: device_id: %ld/%d rc=%d\n",
			 __func__, (unsigned long)cr_device,
			 CGZIP_CR_VENDOR, rc);
		rc = -4;
		goto err_afu_free;
	}

	/* If we cannot get it, continue with warning ... */
	mctx->errinfo = NULL;
	rc = cxl_errinfo_size(mctx->afu_h, &mctx->errinfo_size);
	if (0 == rc) {
		mctx->errinfo = malloc(mctx->errinfo_size);
		if (mctx->errinfo == NULL) {
			rc = -5;
			goto err_afu_free;
		}
	} else
		VERBOSE0(" [%s] WARN: Cannot retrieve errinfo size rc=%d\n",
			 __func__, rc);

	rc = cxl_afu_attach(mctx->afu_h, (__u64)(unsigned long)
			    (void *)mctx->wed);
	if (0 != rc) {
		rc = -6;
		goto err_free_errinfo;
	}

	rc = cxl_mmio_map(mctx->afu_h, CXL_MMIO_BIG_ENDIAN);
	if (rc != 0) {
		rc = -7;
		goto err_free_errinfo;
	}

	return 0;

 err_free_errinfo:
	if (mctx->errinfo)
		free(mctx->errinfo);
	mctx->errinfo = NULL;
 err_afu_free:
	cxl_afu_free(mctx->afu_h);
	mctx->afu_h = NULL;
	VERBOSE3("[%s] Exit rc=%d\n", __func__, rc);
	return rc;
}

static int afu_m_close(struct mdev_ctx *mctx)
{
	VERBOSE3("[%s] Enter\n", __func__);
	if (NULL == mctx->afu_h)
		return -1;

	cxl_mmio_unmap(mctx->afu_h);
	cxl_afu_free(mctx->afu_h);
	mctx->afu_h = NULL;

	if (mctx->errinfo)
		free(mctx->errinfo);
	mctx->errinfo = NULL;
	VERBOSE3("[%s] Exit\n", __func__);
	return 0;
}

static int afu_check_stime(struct mdev_ctx *mctx)
{
	int	gsel, bsel = 0, ctx = 0;
	uint64_t gmask = 0, qstat_reg, err_reg, mstat_reg;
	uint64_t wtime;
	uint64_t cid_reg;
	int	n_act = 0;
	uint64_t s_time = 0;
	char s[32];

	for (gsel = 0; gsel < MMIO_CASV_REG_NUM; gsel++) {
		mmio_read(mctx->afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_CASV_REG + (gsel*8), &gmask);
		if (0 == gmask)
			continue;	/* No bit set, Skip */

		for (bsel = 0; bsel < MMIO_CASV_REG_CTX; bsel++) {
			if (0 == (gmask & (1ull << bsel)))
				continue;	/* Skip */

			ctx = (gsel * MMIO_CASV_REG_CTX) + bsel;	/* Active */

			mmio_read(mctx->afu_h, ctx+1, MMIO_DDCBQ_STATUS_REG, &qstat_reg);
			if (0 == (qstat_reg & 0xffffffff00000000ull)) {
				VERBOSE3("AFU[%d:%03d] master skip\n",
					mctx->card, ctx);
				continue;	/* Skip Master */
			}
			mmio_read(mctx->afu_h, ctx+1, MMIO_DDCBQ_WT_REG, &wtime);
			wtime = wtime / 250; /* makes time in usec */

			mmio_read(mctx->afu_h, ctx+1, MMIO_DDCBQ_CID_REG, &cid_reg);
			uint16_t cur_cid = (uint16_t)(cid_reg >> 16);	/* Currect Context id */
			uint16_t my_cid = (uint16_t)(cid_reg & 0xffff);	/* My Context id */

			mmio_read(mctx->afu_h, ctx+1, MMIO_DDCBQ_DMAE_REG, &err_reg);

			uint16_t cseq = (uint16_t)(qstat_reg >> 48ull);	/* Currect sequence */
			uint16_t lseq = (uint16_t)(qstat_reg >> 32ull);	/* Last sequence */
			uint8_t qidx = (uint8_t)(qstat_reg >> 24);	/* Q Index */
			uint16_t qnfe = (uint16_t)(qstat_reg >> 8);	/* Context Non Fatal Error Bits */
			uint8_t qstat = (uint8_t)(qstat_reg & 0xff);	/* Context Status */

			/* Generate W for Waiting, I for Idle and R for Running */
			char flag = 'W';		/* Default Context is Waiting to get executed */
			if ((lseq + 1 ) == cseq)
				flag = 'I';		/* Context is Idle, nothing to do */
			else if (0x30 == qstat)		/* if Bits 4 + 5 on ? */
					flag = 'R';	/* Context is Running */

			if (qnfe) {
				VERBOSE0("AFU[%d:%03d] ERR: CurrentCtx: %03d MyCtx: %03d CS: %04X LS: %04X ",
					mctx->card, ctx, cur_cid, my_cid, cseq, lseq);
				VERBOSE0("[%c] IDX: %02d QNFE: %04x QSTAT: %02x Time: %lld usec",
					flag, qidx, qnfe, qstat, (long long)wtime);
				if (0 != err_reg)
					VERBOSE0("DMA Err: 0x%016llx", (long long)err_reg);
				VERBOSE0("\n");
			} else {
				VERBOSE0("AFU[%d:%03d] CurrentCtx: %03d MyCtx: %03d CS: %04X LS: %04X ",
					mctx->card, ctx, cur_cid, my_cid, cseq, lseq);
				VERBOSE0("[%c] IDX: %02d QNFE: %04x QSTAT: %02x Time: %lld usec",
					flag, qidx, qnfe, qstat, (long long)wtime);
				if (0 != err_reg)
					VERBOSE0("DMA Err: 0x%016llx", (long long)err_reg);
				VERBOSE0("\n");
			}
			n_act++;
			s_time += wtime;
		}
	}
	if (n_act) {
		time_t result = time(NULL);
		struct tm * p = localtime(&result);
		strftime(s, 32, "%T", p);

		VERBOSE0("AFU[%d:XXX] at %s Running %d Active Contexts total %lld msec",
			 mctx->card,  s, n_act, (long long)s_time/1000);
		mmio_read(mctx->afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_AFU_STATUS_REG, &mstat_reg);
		if (0 != mstat_reg)
			VERBOSE0(" Status: 0x%016llx", (long long)mstat_reg);
		VERBOSE0("\n");
	}
	return mctx->dt;
}

static void afu_dump_mfirs(struct mdev_ctx *mctx)
{
	unsigned int i;
	struct cgzip_afu_fir *fir;

	if (verbose > 3) {
		ddcb_hexdump(fd_out, mctx->errinfo, mctx->errinfo_size);
		return;
	}

	for (i = 0, fir = (struct cgzip_afu_fir *)mctx->errinfo;
	     i < MMIO_FIR_REGS_NUM; i++) {

		VERBOSE0("  AFU[%d] FIR: %d: 0x%08x addr: 0x%08x "
			 "mmio: 0x%016llx\n",
			 mctx->card, i,
			 be32toh(fir[i].fir_val),
			 be32toh(fir[i].fir_addr),
			 (long long)mctx->fir[i]);
	}
}

/*
 * Print FIRs only if they have changed. Always collect them.
 */
static int afu_check_mfirs(struct mdev_ctx *mctx)
{
	int i;
	uint64_t data;
	uint32_t offs;
	bool changed = false;
	bool dead = false;
	long cr_device = 0;
	time_t t;
	int rc;

	for (i = 0; i < MMIO_FIR_REGS_NUM; i++) {
		offs = MMIO_FIR_REGS_BASE + i * 8;
		mmio_read(mctx->afu_h, MMIO_MASTER_CTX_NUMBER, offs, &data);
		if (data != mctx->fir[i])
			changed = true;
		if (data == -1ull)
			dead = true;

		mctx->fir[i] = data;
	}
	if (changed) {
		t = time(NULL);
		VERBOSE0("%s", ctime(&t));

		/* Always print this ... */
		cxl_get_cr_device(mctx->afu_h, 0, &cr_device);
		VERBOSE0("  cr_device: 0x%04lx\n", (unsigned long)cr_device);

		if (mctx->errinfo) {
			rc = cxl_errinfo_read(mctx->afu_h, mctx->errinfo, 0,
					      mctx->errinfo_size);
			if (rc != (int)mctx->errinfo_size) {
				VERBOSE0("  cxl_err_info_read returned %d!\n",
					 rc);
			}
			afu_dump_mfirs(mctx);
		}

		if (dead) {
			t = time(NULL);
			VERBOSE0("%s  AFU[%d] card is dead.\n",
				 ctime(&t), mctx->card);
		}
	}

	return mctx->dt;
}

/* Return true if card Software Release is OK */
static bool check_app(struct mdev_ctx *mctx, uint16_t min_rel)
{
	int	rc;
	uint64_t data;

	/* Get MMIO_APP_VERSION_REG */
	rc = mmio_read(mctx->afu_h, MMIO_MASTER_CTX_NUMBER,
		       MMIO_APP_VERSION_REG, &data);
	if (0 != rc)
		return false;

	/* Check Application Version for Version higher than 0403 */
	/* Register 8 does have following layout for the 64 bits */
	/* RRRRFFIINNNNNNNN */
	/* RRRR     == 16 bit Software Release (0404) */
	/* FF       ==  8 bit Software Fix Level on card (01) */
	/* II       ==  8 bit Software Interface ID (03) */
	/* NNNNNNNN == 32 Bit Function (475a4950) = (GZIP) */

	if (0x475a4950 != (data & 0xffffffff))
		return false;

	data = data >> 32;		/* RRRRFFII */
	if (0x03 == (data & 0xff)) {	/* Check II */
		data = data >> 16;	/* Check RRRR */
		if ((uint16_t)data >= min_rel)	/* need >= min_rel */
			return true;
	}
	return false;
}

static int do_master(struct mdev_ctx *mctx)
{
	int dt = mctx->dt;

	mctx->loop++;
	VERBOSE2("AFU[%d:XXX] Loop: %d Delay: %d sec mode: 0x%x left: %d\n",
		mctx->card, mctx->loop,
		mctx->dt, mctx->mode, mctx->count);

	if (CHECK_FIRS_MODE == (CHECK_FIRS_MODE & mctx->mode))
		dt = afu_check_mfirs(mctx);

	if (CHECK_TIME_MODE == (CHECK_TIME_MODE & mctx->mode))
		dt = afu_check_stime(mctx);

	return dt;
}

static void sig_handler(int sig)
{
	struct mdev_ctx *mctx = &master_ctx;

	VERBOSE0("Sig Handler Signal: %d SID: %d\n", sig, mctx->my_sid);
	afu_m_close(mctx);
	fflush(fd_out);
	fclose(fd_out);

	exit(EXIT_SUCCESS);
}

static void help(char *prog)
{
	printf("Usage: %s [-CvhVd] [-f file] [-c count] [-i delay]\n"
	       "\t-C, --card <num>	Card to use (default 0)\n"
	       "\t-V, --version	\tPrint Version number\n"
	       "\t-h, --help		This help message\n"
	       "\t-q, --quiet		No output at all\n"
	       "\t-v, --verbose	\tverbose mode, up to -vvv\n"
	       "\t-c, --count <num>	Loops to run (-1 = forever)\n"
	       "\t-i, --interval <num>	Interval time in sec (default 1 sec)\n"
	       "\t-d, --daemon		Start in Daemon process (background)\n"
	       "\t-m, --mode		Mode:\n"
	       "\t	1 = Check Master Firs\n"
	       "\t	2 = Report Context Details\n"
	       "\t-f, --log-file <file> Log File name when running in -d "
	       "(daemon)\n"
	       "\n"
	       "Figure out how many card resets are allowed within an hour:\n"
	       "    sudo cat /sys/kernel/debug/powerpc/eeh_max_freezes\n"
	       "\n"
	       "Set this to a higher value with:\n"
	       "    sudo sh -c 'echo 10000 > /sys/kernel/debug/powerpc/eeh_max_freezes'\n"
	       "\n"
	       "Manually resetting a card:\n"
	       "    sudo sh -c 'echo 1 > /sys/class/cxl/card0/reset'\n"
	       "\n", prog);
}

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
	int rc = EXIT_SUCCESS;
	int ch;
	unsigned int i;
	char *log_file = NULL;
	struct mdev_ctx *mctx = &master_ctx;
	int	dt;
	int	mode;

	fd_out = stdout;	/* Default */

	mctx->afu_h = NULL;	/* No handle */
	mctx->loop = 0;		/* Start Loop Counter */
	mctx->quiet = false;	/* Default */
	mctx->dt = 1;		/* Default, 1 sec delay time */
	mctx->count = -1;	/* Default, run forever */
	mctx->card = 0;		/* Default, Card 0 */
	mctx->mode = 0;		/* Default, nothing to watch */
	mctx->daemon = false;	/* Not in Daemon mode */

	for (i = 0; i < MMIO_FIR_REGS_NUM; i++)
		mctx->fir[i] = -1;

	rc = EXIT_SUCCESS;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "card",	required_argument, NULL, 'C' },
			{ "version",	no_argument,	   NULL, 'V' },
			{ "quiet",	no_argument,	   NULL, 'q' },
			{ "help",	no_argument,	   NULL, 'h' },
			{ "verbose",	no_argument,	   NULL, 'v' },
			{ "count",	required_argument, NULL, 'c' },
			{ "interval",	required_argument, NULL, 'i' },
			{ "daemon",	no_argument,	   NULL, 'd' },
			{ "log-file",	required_argument, NULL, 'f' },
			{ "mode",	required_argument, NULL, 'm' },
			{ 0,		0,		   NULL,  0  }
		};
		ch = getopt_long(argc, argv, "C:f:c:i:m:Vqhvd",
			long_options, &option_index);
		if (-1 == ch)
			break;
		switch (ch) {
		case 'C':	/* --card */
			mctx->card = strtol(optarg, (char **)NULL, 0);
			break;
		case 'V':	/* --version */
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'q':	/* --quiet */
			mctx->quiet = true;
			break;
		case 'h':	/* --help */
			help(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'v':	/* --verbose */
			verbose++;
			break;
		case 'c':	/* --count */
			mctx->count = strtoul(optarg, NULL, 0);
			if (0 == mctx->count)
				mctx->count = 1;
			break;
		case 'i':	/* --interval */
			mctx->dt = strtoul(optarg, NULL, 0);
			break;
		case 'd':	/* --daemon */
			mctx->daemon = true;
			break;
		case 'm':	/* --mode */
			mode = strtoul(optarg, NULL, 0);
			switch (mode) {
			case 1: mctx->mode |= CHECK_FIRS_MODE; break;
			case 2: mctx->mode |= CHECK_TIME_MODE; break;
			default:
				fprintf(stderr, "Please provide correct "
					"Mode Option (1..2)\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'f':	/* --log-file */
			log_file = optarg;
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if ((mctx->card < 0) || (mctx->card >= NUM_CARDS)) {
		fprintf(stderr, "Err: %d for option -C is invalid, please provide "
			"0..%d!\n", mctx->card, NUM_CARDS-1);
		exit(EXIT_FAILURE);
	}

	if (mctx->daemon) {
		if (NULL == log_file) {
			fprintf(stderr, "Please Provide log file name (-f) "
				"if running in daemon mode !\n");
			exit(EXIT_FAILURE);
		}
	}
	if (log_file) {
		fd_out = fopen(log_file, "w+");
		if (NULL == fd_out) {
			fprintf(stderr, "Can not create/append to file %s\n",
				log_file);
			exit(EXIT_FAILURE);
		}
	}
	signal(SIGCHLD,SIG_IGN);	/* ignore child */
	signal(SIGTSTP,SIG_IGN);	/* ignore tty signals */
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);
	signal(SIGHUP,sig_handler);	/* catch -1 hangup signal */
	signal(SIGINT, sig_handler);	/* Catch -2 */
	signal(SIGTERM,sig_handler);	/* catch -15 kill signal */

	if (mctx->daemon) {
		mctx->pid = fork();
		if (mctx->pid < 0) {
			printf("Fork() failed\n");
			exit(EXIT_FAILURE);
		}
		if (mctx->pid > 0) {
			printf("Child Pid is %d Parent exit here\n",
			       mctx->pid);
			exit(EXIT_SUCCESS);
		}
		if (chdir("/")) {
			fprintf(stderr, "Can not chdir to / !!!\n");
			exit(EXIT_FAILURE);
		}
		umask(0);
		/* set new session */
		mctx->my_sid = setsid();
		printf("Child sid: %d from pid: %d\n",
		       mctx->my_sid, mctx->pid);

		if(mctx->my_sid < 0)
			exit(EXIT_FAILURE);

		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	rc = cxl_mmio_install_sigbus_handler();
	if (rc != 0) {
		VERBOSE0("Err: Install cxl sigbus_handler rc=%d\n", rc);
		exit(EXIT_FAILURE);
	}

	if (0 != afu_m_open(mctx)) {
		VERBOSE0("Err: failed to open Master Context for "
			 "CAPI Card: %u\n"
			 "\tCheck existence/permissions of /dev/cxl/* or see "
			 "kernel logfile.\n"
			 "\terrno=%d %s\n",
			 mctx->card, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (false == check_app(mctx, MIN_REL_VERSION)) {
		VERBOSE0("Err: Wrong Card Release. Need >= 0x%02x\n",
			MIN_REL_VERSION);
		afu_m_close(mctx);
		exit(EXIT_FAILURE);
	}

	while (1) {
		dt = do_master(mctx);		/* Process */
		if (dt)
			sleep(dt);		/* Sleep Remaining time */
		if (-1 == mctx->count)
			continue;		/* Run Forever */
		mctx->count--;			/* Decrement Runs */
		if (0 == mctx->count)
			break;			/* Exit */
	}

	if (!mctx->quiet && verbose)
		VERBOSE0("[%s] AFU[%d] after %d loops\n",
			 __func__, mctx->card, mctx->loop);

	afu_m_close(mctx);
	fflush(fd_out);
	fclose(fd_out);

	exit(rc);
}
