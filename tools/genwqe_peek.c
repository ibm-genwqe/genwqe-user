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
	       "  -c, --count <num>         number of peeks do be done.\n"
	       "  -e, --must-be <value>     compare and exit if not equal.\n"
	       "  -n, --must-not-be <value> compare and exit if equal.\n"
	       "  -a, --and-mask <valud>    mask read value before compare.\n"
	       "  <addr>\n"
	       "\n"
	       "Example:\n"
	       "  genwqe_peek 0x0000\n"
	       "  [00000000] 000000021032a178\n\n",
	       prog);
}

/**
 * @brief
 *
 */
int main(int argc, char *argv[])
{
	int ch, rc = 0;
	int card_no = 0;
	int card_type = DDCB_TYPE_GENWQE;
	accel_t card;
	int err_code = 0;
	int cpu = -1;
	int width = 64;
	uint32_t offs;
	uint64_t val = 0xffffffffffffffffull;
	uint64_t and_mask = 0xffffffffffffffffull;
	uint64_t equal_val = val;
	uint64_t not_equal_val = val;
	int equal = 0, not_equal = 0;
	int quiet = 0;
	unsigned long i, count = 1;
	unsigned long interval = 0;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			/* functions */

			/* options */
			{ "card",	 required_argument, NULL, 'C' },
			{ "accelerator-type", required_argument, NULL, 'A' },
			{ "cpu",	 required_argument, NULL, 'X' },

			{ "width",	 required_argument, NULL, 'w' },
			{ "interval",	 required_argument, NULL, 'i' },
			{ "count",	 required_argument, NULL, 'c' },
			{ "must-be",	 required_argument, NULL, 'e' },
			{ "must-not-be", required_argument, NULL, 'n' },
			{ "and-mask",    required_argument, NULL, 'a' },

			/* misc/support */
			{ "version",	 no_argument,	    NULL, 'V' },
			{ "quiet",	 no_argument,	    NULL, 'q' },
			{ "verbose",	 no_argument,	    NULL, 'v' },
			{ "help",	 no_argument,	    NULL, 'h' },
			{ 0,		 no_argument,	    NULL, 0   },
		};

		ch = getopt_long(argc, argv,
				 "C:A:X:w:i:c:e:n:a:Vqvh",
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
		case 'e':
			equal = 1;
			equal_val = strtoull(optarg, NULL, 0);
			break;
		case 'n':
			not_equal = 1;
			not_equal_val = strtoull(optarg, NULL, 0);
			break;
		case 'a':
			and_mask = strtoull(optarg, NULL, 0);
			break;

		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
		case 'q':
			quiet++;
			break;
		case 'v':
			verbose_flag = 1;
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

	if (optind + 1 != argc) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	offs = strtoull(argv[optind], NULL, 0);

	if (equal && not_equal) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	switch_cpu(cpu, verbose_flag);

	card = accel_open(card_no, card_type, DDCB_MODE_RD, &err_code,
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
			val = accel_read_reg32(card, offs, &rc);
			break;
		default:
		case 64:
			val = accel_read_reg64(card, offs, &rc);
			break;
		}

		if (rc != DDCB_OK) {
			fprintf(stderr, "err: could not read [%08x] rc=%d\n",
				offs, rc);
			accel_close(card);
			exit(EXIT_FAILURE);
		}
		if ((equal) &&
		    (equal_val != (val & and_mask))) {
			fprintf(stderr, "err: [%08x] %016llx != %016llx\n",
				offs, (long long)val, (long long)equal_val);
			accel_close(card);
			exit(EX_ERR_DATA);
		}
		if ((not_equal) &&
		    (not_equal_val == (val & and_mask))) {
			fprintf(stderr, "err: [%08x] %016llx == %016llx\n",
				offs, (long long)val,
				(long long)not_equal_val);
			accel_close(card);
			exit(EX_ERR_DATA);
		}

		if (interval)
			usleep(interval);
	}

	if (!quiet)
		printf("[%08x] %016llx\n", offs, (long long)val);

	accel_close(card);
	exit(EXIT_SUCCESS);
}
