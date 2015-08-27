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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "genwqe_tools.h"
#include "force_cpu.h"
#include <libddcb.h>

int verbose_flag = 0;

static const char *version = GIT_VERSION;

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s [-h] [-v,--verbose]\n"
	       "  -C,--card <cardno>\n"
	       "  -A, --accelerator-type=GENWQE|CAPI CAPI is only available "
	       "for System p\n"
	       "  -V, --version             print version.\n"
	       "  -q, --quiet               quiece output.\n"
	       "  -w, --width <32|64>       access width.\n"
	       "  -X, --cpu <id>            only run on this CPU.\n"
	       "  -i, --interval <intv>     interval in usec, 0: default.\n"
	       "  -c, --count <mum>         number of pokes.\n"
	       "  -r, --read-back           read back and verify.\n"
	       "  <addr> <val>\n"
	       "\n"
	       "Example (calling as root):\n"
	       "  genwqe_poke 0x0000000 0xdeadbeef\n"
	       "\n"
	       "Testcase to trigger error recovery code:\n"
	       "   Fatal GFIR:\n"
	       "     sudo ./tools/genwqe_poke -C0 0x00000008 0x001\n"
	       "   Info GFIR by writing to VF:\n"
	       "     sudo ./tools/genwqe_poke -C2 0x00020020 0x800\n"
	       "\n",
	       prog);
}

/**
 * @brief	tool to write to zEDC register
 *		must be called as root !
 *
 */
int main(int argc, char *argv[])
{
	int ch, rc, rbrc;
	int card_no = 0;
	int card_type = DDCB_TYPE_GENWQE;
	accel_t card;
	int err_code = 0;
	int cpu = -1;
	int width = 64;
	int rd_back = 0;
	uint32_t offs;
	uint64_t val, rbval;
	int quiet = 0;
	unsigned long i, count = 1;
	unsigned long interval = 0;
	int xerrno;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			/* functions */

			/* options */
			{ "card",	required_argument, NULL, 'C' },
			{ "accelerator-type", required_argument, NULL, 'A' },
			{ "cpu",	required_argument, NULL, 'X' },

			{ "width",	required_argument, NULL, 'w' },
			{ "interval",	required_argument, NULL, 'i' },
			{ "count",	required_argument, NULL, 'c' },
			{ "rd-back",	no_argument,       NULL, 'r' },

			/* misc/support */
			{ "version",	no_argument,	   NULL, 'V' },
			{ "quiet",	no_argument,	   NULL, 'q' },
			{ "verbose",	no_argument,	   NULL, 'v' },
			{ "help",	no_argument,	   NULL, 'h' },
			{ 0,		no_argument,	   NULL, 0   },
		};

		ch = getopt_long(argc, argv,
				 "C:A:X:w:i:c:Vqrvh",
				 long_options, &option_index);
		if (ch == -1)	/* all params processed ? */
			break;

		switch (ch) {
		/* which card to use */
		case 'C':
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
		case 'w':
			width = strtoul(optarg, NULL, 0);
			break;
		case 'i':		/* interval */
			interval = strtol(optarg, (char **)NULL, 0);
			break;
		case 'c':		/* loop count */
			count = strtol(optarg, (char **)NULL, 0);
			break;

		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
		case 'q':
			quiet++;
			break;
		case 'r':
			rd_back++;
			break;
		case 'v':
			verbose_flag++;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;

		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (optind + 2 != argc) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	if (geteuid() != 0) {
		printf("must be root to write to zEDC\n");
		exit(EXIT_FAILURE);
	}

	offs = strtoull(argv[optind++], NULL, 0);
	val  = strtoull(argv[optind++], NULL, 0);
	rbval = ~val;
	switch_cpu(cpu, verbose_flag);

	card = accel_open(card_no, card_type, DDCB_MODE_WR, &err_code,
			  0, DDCB_APPL_ID_IGNORE);
	if (card == NULL) {
		fprintf(stderr, "err: failed to open card %u type %u "
			"(%d/%s)\n", card_no, card_type, err_code,
			accel_strerror(card, err_code));
		exit(EXIT_FAILURE);
	}
	ddcb_debug(verbose_flag);

	for (i = 0; i < count; i++) {
		switch (width) {
		case 32:
			rc = accel_write_reg32(card, offs,
						     (uint32_t)val);
			xerrno = errno;
			if (rd_back)
				rbval = accel_read_reg32(card,
							       offs,
							       &rbrc);
			break;
		default:
		case 64:
			rc = accel_write_reg64(card, offs, val);
			xerrno = errno;
			if (rd_back)
				rbval = accel_read_reg64(card,
							       offs,
							       &rbrc);
			break;
		}

		if (rc != DDCB_OK) {
			fprintf(stderr, "err: could not write "
				"%016llx to [%08x]\n"
				"  %s: %s\n", (unsigned long long)val, offs,
				accel_strerror(card, rc), strerror(xerrno));
			accel_close(card);
			exit(EXIT_FAILURE);
		}
		if (rd_back) {
			if (rbrc != DDCB_OK) {
				fprintf(stderr, "err: read back failed\n");
				accel_close(card);
				exit(EXIT_FAILURE);
			}
			if (val != rbval) {
				fprintf(stderr, "err: post verify failed\n");
				accel_close(card);
				exit(EXIT_FAILURE);
			}
		}

		if (interval)
			usleep(interval);
	}

	if (!quiet)
		printf("[%08x] %016llx\n", offs, (long long)val);

	accel_close(card);
	exit(EXIT_SUCCESS);
}
