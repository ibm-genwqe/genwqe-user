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
 * Accelerator Gzip Uptimea tool
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/timeb.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>

#include <libddcb.h>
#include <libcxl.h>
#include "afu_regs.h"

static const char *version = GIT_VERSION;
static int verbose = 0;

#define VERBOSE0(fmt, ...) do {					\
		fprintf(stderr, fmt, ## __VA_ARGS__);		\
	} while (0)

#define VERBOSE1(fmt, ...) do {					\
		if (verbose > 0)				\
			fprintf(stderr, fmt, ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE2(fmt, ...) do {					\
		if (verbose > 1)				\
			fprintf(stderr, fmt, ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE3(fmt, ...) do {					\
		if (verbose > 2)				\
			fprintf(stderr, fmt, ## __VA_ARGS__);	\
	} while (0)

typedef union ivr_u {
	uint64_t reg64;
	struct {
		uint8_t build_count:4;
		uint8_t res2:4;
		uint8_t day;
		uint8_t month;
		uint8_t year;
		uint16_t freq;
		uint16_t res1;
	} data;
} IVR;

typedef union avr_u {
	uint64_t reg64;
	struct {
		uint32_t aid;		/* fix 0x475a4950 */
		uint8_t aida;		/* fix 0x03 */
		uint8_t release2;
		uint16_t release1;
	} data;
} AVR;

/*	Expect min this Release or higher */
#define	MIN_REL_VERSION	0x0603

static int mmio_write(struct cxl_afu_h *afu_h,
		int ctx,
		uint32_t offset,
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

static int mmio_read(struct cxl_afu_h *afu_h,
		int ctx,
		uint32_t offset,
		uint64_t *data)
{
	int rc = -1;
	uint32_t offs = (ctx * MMIO_CTX_OFFSET) + offset;

	VERBOSE3("[%s] Enter CTX: %d Offset: 0x%x\n",
		__func__, ctx, offs);
	rc = cxl_mmio_read64(afu_h, offs, data);
	VERBOSE3("[%s] Exit rc: %d data: 0x%016llx\n",
		__func__, rc, (long long)*data);
	return rc;
}

/* Return true if card Software Release is OK */
static bool check_app(struct cxl_afu_h *afu_h, uint16_t min_rel)
{
	int	rc;
	AVR	avr;

	/* Get MMIO_APP_VERSION_REG */
	rc = mmio_read(afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_APP_VERSION_REG, &avr.reg64);
	if (0 != rc)
		return false;
	if (0x475a4950 != avr.data.aid)
		return false;
	if (0x03 != avr.data.aida) 		/* Check II */
		return false;
	if (avr.data.release1 >= min_rel)	/* need >= min_rel */
		return true;
	return false;
}

/*
 * Open AFU Master Device
 */
static struct cxl_afu_h *card_open(int card)
{
	int rc = 0;
	char device[64];
	long api_version, cr_device, cr_vendor;
	uint64_t wed = 0;	/* Dummy */
	struct cxl_afu_h *afu_h = NULL;

	sprintf(device, "/dev/cxl/afu%d.0m", card);
	VERBOSE1("[%s] Card: %d Open Device: %s\n",
		__func__, card, device);
	afu_h = cxl_afu_open_dev(device);
	if (NULL == afu_h) {
		perror("cxl_afu_open_dev()");
		VERBOSE0("[%s] Card: %d cxl_afu_open Error rc: %d\n",
			__func__, card, rc);
		rc = -1;
		goto card_open_exit;
	}

	/* Check if the compiled in API version is compatible with the
	   one reported by the kernel driver */
	rc = cxl_get_api_version_compatible(afu_h, &api_version);
	if ((rc != 0) || (api_version != CXL_KERNEL_API_VERSION)) {
		VERBOSE0(" [%s] Card: %d ERR: incompatible API version: %ld/%d rc=%d\n",
			 __func__, card, api_version, CXL_KERNEL_API_VERSION, rc);
		rc = -2;
		goto card_open_exit1;
	}

	/* Check vendor id */
	rc = cxl_get_cr_vendor(afu_h, 0, &cr_vendor);
	if (rc != 0) {
		perror("cxl_get_cr_vendor()");
		goto card_open_exit1;
	}
	if (cr_vendor != CGZIP_CR_VENDOR) {
		VERBOSE0(" [%s] Card: %d ERR: Vendor_id: 0x%lx Expect: 0x%x\n",
			 __func__, card, (unsigned long)cr_vendor, CGZIP_CR_VENDOR);
		rc = -3;
		goto card_open_exit1;
	}

	/* Check device id */
	rc = cxl_get_cr_device(afu_h, 0, &cr_device);
	if (rc != 0) {
		perror("cxl_get_cr_device()");
		goto card_open_exit1;
	}
	if (cr_device != CGZIP_CR_DEVICE) {
		VERBOSE0(" [%s] Card: %d ERR: Device_id: 0x%lx Expect: 0x%x\n",
			 __func__, card, (unsigned long)cr_device, CGZIP_CR_DEVICE);
		rc = -4;
		goto card_open_exit1;
	}

	if (0 != cxl_afu_attach(afu_h, (__u64)(unsigned long)(void *)&wed)) {
		perror("cxl_afu_attach()");
		rc = -6;
		goto card_open_exit1;
	}

	if (0 != cxl_mmio_map(afu_h, CXL_MMIO_BIG_ENDIAN)) {
		perror("cxl_mmio_map()");
		rc = -7;
 		goto card_open_exit1;
	}
	if (false == check_app(afu_h, MIN_REL_VERSION)) {
		VERBOSE0("[%s] Card: %d Err: Card Release Need >= 0x%02x\n",
			__func__, card, MIN_REL_VERSION);
		cxl_mmio_unmap(afu_h);
		rc = -8;
	}

 card_open_exit1:
	if (0 != rc) {
		if (afu_h) {
			cxl_afu_free(afu_h);
			afu_h = NULL;
		}
	}
 card_open_exit:
	VERBOSE1("[%s] Card: %d Exit rc: %d handle: %p\n",
		__func__, card, rc, afu_h);
	return afu_h;
}

static void card_close(int card, struct cxl_afu_h *afu_h)
{
	VERBOSE1("[%s] Card: %d Enter\n", __func__, card);
	if (NULL == afu_h)
		return;

	cxl_mmio_unmap(afu_h);
	cxl_afu_free(afu_h);
	VERBOSE1("[%s] Card: %d Exit\n", __func__, card);
	return;
}

#define	TICKS_TOMSEC	250000
#define MSEC_PER_SEC	1000
#define	SEC_PER_MIN	60
#define	SEC_PER_HOUR	(SEC_PER_MIN * 60)
#define SEC_PER_DAY	(SEC_PER_HOUR * 24)

/* Print day-h:min:sec.msec */
static void print_dhms(uint64_t msec)
{
	int sec = (int)(msec / MSEC_PER_SEC);
	int msecs = (int)(msec - (sec * MSEC_PER_SEC));
	int days = sec / SEC_PER_DAY;
	int hours = (sec % SEC_PER_DAY) / SEC_PER_HOUR;
	int mins = ((sec % SEC_PER_DAY) % SEC_PER_HOUR) / SEC_PER_MIN;
	int secs = ((sec % SEC_PER_DAY) % SEC_PER_HOUR) % SEC_PER_MIN; 
	VERBOSE0("%d-%02d:%02d:%02d.%d", days, hours, mins, secs, msecs);
	return;
}


static void reset_counters(struct cxl_afu_h *afu_h)
{
	uint64_t data = 0ull;

	/* Note: Registers are RO ! */
	mmio_write(afu_h, MMIO_MASTER_CTX_NUMBER,
		0x90, data);
	mmio_write(afu_h, MMIO_MASTER_CTX_NUMBER,
		MMIO_FRT_REG, data);
	return;
}

static void get_load(int card, struct cxl_afu_h *afu_h)
{
	uint64_t wload, frt;
	int load;

	mmio_read(afu_h, MMIO_MASTER_CTX_NUMBER,
		0x90, &wload);
	if (-1ull != wload) {
		mmio_read(afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_FRT_REG, &frt);
		if (-1ull != frt) {
			wload = wload / TICKS_TOMSEC;	/* Wload in msec */
			frt = frt / TICKS_TOMSEC;	/* FRT in msec */
			load = (int)(wload * 100 / frt);
			VERBOSE0("Capi-Gzip Card %d Up: ", card);
			print_dhms(frt);
			VERBOSE0(" Busy: ");
			print_dhms(wload);
			VERBOSE0(" (d-h:m:s.msec) Load AVG: %d%%\n", load);
		} else VERBOSE0("[%s] Can not read FRT from Card: %d\n",
			__func__, card);
	} else VERBOSE0("[%s] Can not read WLOAD from Card: %d\n",
			__func__, card);
	return;
}

static void sig_handler(int sig)
{
	VERBOSE0("Sig Handler Signal: %d\n", sig);
	exit(EXIT_SUCCESS);
}

static void help(char *prog)
{
	printf("Usage: %s [-vhV] [-C Card#]\n"
	       "\t-C, --card            CAPI Gzip Card to use\n"
	       "\t-V, --version         Print Version number\n"
	       "\t-h, --help            This help message\n"
	       "\t-r, --reset           Reset Counters before reading (future)\n"
	       "\t-v, --verbose         verbose mode, up to -vvv\n"
	       , prog);
}

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
	int rc = EXIT_SUCCESS;
	int ch;
	struct cxl_afu_h *afu_h;
	int card = 0;
	bool reset_flag = false;

	sigset_t new;

	rc = EXIT_SUCCESS;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "card",	required_argument, NULL, 'C' },
			{ "version",	no_argument,	   NULL, 'V' },
			{ "help",	no_argument,	   NULL, 'h' },
			{ "verbose",	no_argument,	   NULL, 'v' },
			{ "reset",	no_argument,	   NULL, 'r' },
			{ 0,		0,		   NULL,  0  }
		};
		ch = getopt_long(argc, argv, "C:Vvhr",
			long_options, &option_index);
		if (-1 == ch)
			break;
		switch (ch) {
		case 'C':	/* --card */
			card = strtoul(optarg, NULL, 0);
			break;
		case 'V':	/* --version */
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'h':	/* --help */
			help(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'v':	/* --verbose */
			verbose++;
			break;
		case 'r':	/* --reset */
			reset_flag = true;
			break;
		default:
			help(argv[0]);
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

	/* Open Card */
	afu_h = card_open(card);
	if (afu_h) {
		rc = cxl_mmio_install_sigbus_handler();
		if (rc != 0) {
			VERBOSE0("Err: Install cxl sigbus_handler rc=%d\n", rc);
			goto main_exit;
		}

		sigemptyset(&new);
        	sigaddset(&new, SIGPIPE);
        	if (pthread_sigmask(SIG_BLOCK, &new, NULL) != 0) {
			VERBOSE0("Unable to mask SIGPIPE");
			goto main_exit;
        	}

		if (reset_flag)
			reset_counters(afu_h);
		get_load(card, afu_h);
	} else rc = EINVAL;
  main_exit:
	if (afu_h)
		card_close(card, afu_h);

	VERBOSE1("Exit: rc: %d\n", rc);
	exit(rc);
}
