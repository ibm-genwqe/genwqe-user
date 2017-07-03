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

/**
 * @file	genwqe_echo.c
 * @brief	FPGA accelerator SW utility.
 *
 * This utility sends ECHO-DDCBs to the Service Layer Unit (SLU) or an
 * chip application unit (or AFU), waits for completion and checks if
 * the teststring is correctly returned.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <asm/byteorder.h>

#include <sched.h>

#include "genwqe_tools.h"
#include "force_cpu.h"
#include "libddcb.h"

#define timediff_usec(t0, t1)						\
	((double)(((t0)->tv_sec * 1000000 + (t0)->tv_usec) -		\
		  ((t1)->tv_sec * 1000000 + (t1)->tv_usec)))

int verbose_flag = 0;

static const char *version = GIT_VERSION;
const char *tstring_default = "ABCDEF_echo test [123456789abcde]";

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s [OPTIONS]\n"
	       "  -h, --help\n"
	       "  -v, --verbose\n"
	       "  -C, --card=CARDNO|RED  Note: RED is for Card Redundant mode\n"
	       "  -A, --accelerator-type=GENWQE|CAPI CAPI is only available "
	       "for System p\n"
	       "  -q, --quiet            quiece output\n"
	       "  -V, --version\n"
	       "  -H, --hardware-version\n"
	       "  -c, --count=COUNT\n"
	       "  -X, --cpu=CPU          only run on this CPU number\n"
	       "  -D, --debug            create extended debug data on failure\n"
#if defined (CONFIG_BUILD_4TEST)
	       "  -u, --unitid=0:service layer|1:APP\n"
#endif
	       "  -e, --exit-on-err      exit program when seeing an error\n"
	       "  -f, --flood\n"
	       "  -l, --preload=1..N     N <= 64\n"
	       "  -i, --interval=INTERVAL_USEC\n"
	       "  -s, --string=TESTSTRING\n"
	       "  -p, --polling          use DDCB polling mode.\n"
	       "\n"
	       "This utility sends echo DDCBs either to the service layer\n"
	       "or other chip units. It can be used to check the cards\n"
	       "health and/or to produce stress on the card to verify its\n"
	       "correct function.\n\n", prog);
}

static void INT_handler(int sig);
static int stop_echoing = 0;

static void INT_handler(int sig)
{
	signal(sig, SIG_IGN);
	stop_echoing = 1;
	/* signal(SIGINT, INT_handler); *//* Try again */
}

/**
 * @brief		prepare data to be send via ECHO-DDCB.
 *			clear receive part.
 *			SLU allows 64 bytes to be echoed.
 *
 * @param tstring	test string to be send to SLU
 * @param acfunc	Unit-ID in HW: 0=SLU, 1=APP
 * @param cmd		command definitions and return values
 * @param count		how often shall the command be issued
 *
 */
static void preset_echo_cmd(char *tstring, uint8_t acfunc,
			    struct ddcb_cmd *cmd, int count)
{
	int i, j;
	int len;

	/* preset tx values */
	len = (int)strlen(tstring);
	len = ((len + 7) / 8) * 8;	/* round up to multiples of 8 */
	if (len > DDCB_ASV_LENGTH) {
		pr_info("test string too long (%u)\n",
			(unsigned int)strlen(tstring));

		len = DDCB_ASV_LENGTH;		/* limit to 64 chars */
	}

	for (i = 0; i < count; i++) {
		cmd->acfunc = acfunc;  /* to which func is echo going? */
		cmd->ddata_addr = 0ull;	 /* FIXME */
		cmd->cmd     = DDCB_CMD_ECHO_SYNC;
		cmd->cmdopts = _DDCB_OPT_ECHO_COPY_ALL;
		cmd->ats     = 0ULL;

		strncpy((char *)cmd->asiv, tstring, len);
		cmd->asiv_length = 64;

		/* clear rx values */
		for (j=0; j < DDCB_ASV_LENGTH; j++)
			cmd->asv[j] = 0;

		cmd->asv_length = 64;
		cmd->retc = DDCB_RETC_IDLE; /* still unprocessed */
		if (i < (count-1))  /* chaining */
			cmd->next_addr = (unsigned long)(cmd + 1);
		else
			cmd->next_addr = 0x0;
		cmd++;
	}
	pr_info("%u ECHO DDCBs prepared (%u bytes to send)\n", count, len);
}

static int do_echo(accel_t card, int preload, uint8_t unit,
		   char *teststring)
{
	int rc, j, xerrno;
	unsigned int i;
	struct ddcb_cmd *cmd, *pcmd;

	/* uint64_t reg64; */

	/* FIXME mallocs eat performance. Consider to allocate largest
	   size on stack. */
	cmd = (struct ddcb_cmd *)malloc(preload * sizeof(*cmd));
	if (cmd == NULL) {
		fprintf(stderr, "err: failed to alloc cmd memory\n");
		return -ENOMEM;
	}
	memset(cmd, 0, preload * sizeof(*cmd));

	/* preset all cmd structures */
	preset_echo_cmd(teststring, unit, cmd, preload);
	pcmd = cmd;

	/* issue ECHO commands */
	rc = accel_ddcb_execute(card, pcmd, NULL, &xerrno);
	if (rc != DDCB_OK) {
		fprintf(stderr,
			"err: Echo DDCB failed: %s (%d)\n"
			"     errno=%d %s\n"
			"     RETC: %03x %s ATTN: %02x PROGR: %x\n",
			ddcb_strerror(rc), rc, xerrno, strerror(xerrno),
			pcmd->retc, ddcb_retc_strerror(pcmd->retc),
			pcmd->attn, pcmd->progress);

		goto err;
	}

	/* now check all results */
	pcmd = cmd;
	rc = EXIT_SUCCESS;
	for (j = 0; j < preload; j++) {
		if (strncmp((char *)pcmd->asv, teststring,
			    strlen(teststring)) != 0) {

			printf("\nDDCB echo compare failed\n"
			       "    retc=%x %s:\n",
			       pcmd->retc, ddcb_retc_strerror(pcmd->retc));

			printf("  original: ");
			for (i = 0; i < strlen(teststring); i++) {
				printf(" %02x", teststring[i]);
				if ((i & 0x0f) == 0x0f)
					printf("\n            ");
			}

			printf("\n  received: ");
			for (i = 0; i < pcmd->asv_length; i++) {
				printf(" %02x", pcmd->asv[i]);
				if ((i & 0x0f) == 0x0f)
					printf("\n            ");
			}
			printf("\n");

			rc = EX_ERR_DATA;
			break;
		} else {
			pr_info("Echo OK (retc=%x %s)\n", pcmd->retc,
				ddcb_retc_strerror(pcmd->retc));
		}
		pcmd++;
	}
 err:
	free(cmd);
	return rc;
}

/**
 * @brief	the utility itself
 */
int main(int argc, char *argv[])
{
	int ch, rc = DDCB_OK;
	int card_no = 0;
	int card_type = DDCB_TYPE_GENWQE;
	int preload = 1;
	int flood = 0;
	bool print_hardware_version = false;
	int quiet = 0;
	int exit_on_err = 1;
	unsigned long count = 0;
	int run_infinite = 1;
	unsigned long interval = 1000000; /* 1sec is default */
	uint8_t unit = DDCB_ACFUNC_APP;	/* 0=Servicelayer/1=ZCOMP/GZIP/... */
	accel_t card;
	char *teststring =(char *)tstring_default;
	unsigned long packets_send = 0, packets_received = 0;
	int cpu = -1;
	int err_code = 0;
	unsigned long long frequency, wtime_usec = 0, wtime_s = 0, wtime_e = 0;
	unsigned int mode = (DDCB_MODE_RDWR | DDCB_MODE_ASYNC);

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			/* functions */

			/* options */
			{ "card",	required_argument, NULL, 'C' },
			{ "accelerator-type", required_argument, NULL, 'A' },
			{ "cpu",	required_argument, NULL, 'X' },

			{ "count",	required_argument, NULL, 'c' },
			{ "preload",	required_argument, NULL, 'l' },
			{ "interval",	required_argument, NULL, 'i' },
			{ "string",	required_argument, NULL, 's' },
#if defined (CONFIG_BUILD_4TEST)
			{ "unit",	required_argument, NULL, 'u' },
#endif
			{ "exit-on-err", required_argument, NULL, 'e' },
			{ "flood",	no_argument,       NULL, 'f' },

			/* misc/support */
			{ "version",	no_argument,       NULL, 'V' },
			{ "hardware-version", no_argument, NULL, 'H' },
			{ "debug",	no_argument,	   NULL, 'D' },
			{ "polling",	no_argument,	   NULL, 'p' },
			{ "quiet",	no_argument,	   NULL, 'q' },
			{ "verbose",	no_argument,       NULL, 'v' },
			{ "help",	no_argument,	   NULL, 'h' },
			{ 0,		no_argument,	   NULL,  0  },
		};

#if defined (CONFIG_BUILD_4TEST)
		ch = getopt_long(argc, argv, "pDC:A:c:fhl:i:s:qvX:HVu:e:",
				long_options, &option_index);
#else
		ch = getopt_long(argc, argv, "pDC:A:c:fhl:i:s:qvX:HVe:",
				long_options, &option_index);
#endif
		if (ch == -1)	/* all params processed ? */
			break;

		switch (ch) {
		case 'C':
			if (strcmp(optarg, "RED") == 0) {
				card_no = ACCEL_REDUNDANT;
				break;
			}
			card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 'A':		/* set card number */
			if (strcmp(optarg, "GENWQE") == 0) {
				card_type = DDCB_TYPE_GENWQE;
				break;
			}
			if (strcmp(optarg, "CAPI") == 0) {
				card_type = DDCB_TYPE_CAPI;
				break;
			}
			card_type = strtol(optarg, (char **)NULL, 0);
			break;
		case 'X':
			cpu = strtoul(optarg, NULL, 0);
			break;

		case 'c':		/* loop count */
			count = strtol(optarg, (char **)NULL, 0);
			run_infinite = 0;
			break;
		case 'l':		/* preload */
			preload = strtol(optarg, (char **)NULL, 0);
			break;
		case 'i':		/* interval */
			interval = strtol(optarg, (char **)NULL, 0);
			break;
		case 'f':
			flood = 1;
			interval = 0;
			break;

		case 's':		/* string */
			teststring = optarg;
			if (strlen(teststring) > DDCB_ASV_LENGTH) {
			    printf("WARNING: Limited string to %d bytes\n",
				   DDCB_ASV_LENGTH);
			    teststring[DDCB_ASV_LENGTH] = 0;
			}
			break;
		case 'e':
			exit_on_err = strtol(optarg, (char **)NULL, 0);
			break;
#if defined (CONFIG_BUILD_4TEST)
		case 'u':		/* unit */
			unit = strtol(optarg, (char **)NULL, 0);
			break;
#endif
		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
		case 'D':
			/* debug_flag++; *//* FIXME disabled */
			break;
		case 'p':
			mode |= DDCB_MODE_POLLING;
			break;
		case 'q':
			quiet++;
			break;
		case 'v':
			verbose_flag++;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'H':
			print_hardware_version = true;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (optind != argc) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	switch_cpu(cpu, verbose_flag);
	ddcb_debug(verbose_flag);

	/* open card access (for DDCB) */
	card = accel_open(card_no, card_type, mode, &err_code, 0,
			  DDCB_APPL_ID_IGNORE);
	if (card == NULL) {
		fprintf(stderr, "err: failed to open card %u type %u "
			"(%d/%s)\n", card_no, card_type, err_code,
			accel_strerror(card, err_code));
		rc = err_code;
		goto err_out;
	}
	if (print_hardware_version) {
		accel_dump_hardware_version(card, stderr);
		goto close_card;
	}


	/* Note: I want to be able to send to an illegal unit as a
	   testcase */
	pr_info("Start DDCB Echo '%s' for unit #%x\n", teststring, unit);
	if (preload < 1)
		preload = 1;

	signal(SIGINT, INT_handler);
	wtime_s = accel_get_queue_work_time(card);

	while (!stop_echoing) {
		struct timeval t0, t1;

		if (!run_infinite && !count)
			break;

		gettimeofday(&t0, NULL);
		rc = do_echo(card, preload, unit, teststring);
		gettimeofday(&t1, NULL);

		packets_send++;
		if (rc == 0) {
			if (!flood && !quiet) {
				printf("%d x %u bytes from UNIT #%x: "
				       "echo_req time=%2.1f usec\n",
				       preload, (int)strlen(teststring),
				       unit, timediff_usec(&t1, &t0));
			}
			packets_received++;
		}

		count--;
		if (!run_infinite && !count)
			break;	/* stop without waiting, if count is 0 */

		if (interval)
			usleep(interval);

		if (exit_on_err && (rc != DDCB_OK))
			break;
	}

	wtime_e = accel_get_queue_work_time(card);
	frequency = accel_get_frequency(card);
	wtime_usec = frequency ? (wtime_e - wtime_s) / (frequency/1000000) : 0;

 close_card:
	accel_close(card);

	if (!flood && !quiet)
		printf("\n");

 err_out:
	if (!quiet) {
		printf("--- UNIT #%x echo statistics ---\n"
		       "%ld packets transmitted, %ld received, %ld lost, "
		       "%ld%% packet loss, queue %lld usec\n",
		       unit, packets_send,
		       packets_received, (packets_send - packets_received),
		       !packets_send ? 100 :
		       100 * (packets_send - packets_received)/packets_send,
		       wtime_usec);
	}

	if (rc != DDCB_OK)
		exit(EXIT_FAILURE);

	exit(EXIT_SUCCESS);
}
