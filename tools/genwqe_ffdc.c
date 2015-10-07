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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <libcard.h>
#include "genwqe_tools.h"

static const char *version = GIT_VERSION;
int verbose_flag = 0;

static inline uint64_t genwqe_readq(card_handle_t c, uint32_t reg)
{
	int rc;
	uint64_t val;

	val = genwqe_card_read_reg64(c, reg, &rc);
	if (rc != GENWQE_OK)
		fprintf(stderr, "warn: genwqe_readq returned %d\n", rc);

	return val;
}

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Utility to do first failure data capture (FFDC).\n"
	       "\n"
	       "Usage: %s [-h] [-v,--verbose]\n"
	       "\t[-C, --card <cardno>]\n"
	       "\t[-Q, --dump-queues] Dump DDCB queue registers of all funcs\n"
	       "\t[-V, --version]\n"
	       "\t[-v, --verbose]\n"
	       "\n", prog);
}

static uint64_t vreadq(card_handle_t c, uint32_t reg, int func)
{
	int rc;
	uint64_t val;

	rc = genwqe_card_write_reg64(c, IO_PF_SLC_VIRTUAL_WINDOW, func & 0xf);
	if (rc != GENWQE_OK) {
		fprintf(stderr,
			"warn: genwqe_card_write_reg64 returned %d errno=%d\n",
			rc, errno);
		return (uint64_t)-1;
	}

	val = genwqe_card_read_reg64(c, reg, &rc);
	if (rc != GENWQE_OK) {
		fprintf(stderr,
			"warn: genwqe_card_read_reg64 returned %d errno=%d\n",
			rc, errno);
		return (uint64_t)-1;
	}

	return val;
}

static void do_dump_queues(card_handle_t c)
{
	int func;

	pr_info("[%s] Genwqe queue config and debug registers\n", __func__);

	for (func = 0; func < 16; func++) {
		printf("PCI FUNCTION %d\n"
		       "  0x%08x %016llx IO_QUEUE_CONFIG\n"
		       "  0x%08x %016llx IO_QUEUE_STATUS\n"
		       "  0x%08x %016llx IO_QUEUE_SEGMENT\n"
		       "  0x%08x %016llx IO_QUEUE_INITSQN\n"
		       "  0x%08x %016llx IO_QUEUE_WRAP\n"
		       "  0x%08x %016llx IO_QUEUE_OFFSET\n"
		       "  0x%08x %016llx IO_QUEUE_WTIME\n"
		       "  0x%08x %016llx IO_QUEUE_ERRCNTS\n"
		       "  0x%08x %016llx IO_QUEUE_LRW\n", func,
		       IO_SLC_QUEUE_CONFIG,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_CONFIG, func),
		       IO_SLC_QUEUE_STATUS,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_STATUS, func),
		       IO_SLC_QUEUE_SEGMENT,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_SEGMENT, func),
		       IO_SLC_QUEUE_INITSQN,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_INITSQN, func),
		       IO_SLC_QUEUE_WRAP,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_WRAP, func),
		       IO_SLC_QUEUE_OFFSET,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_OFFSET, func),
		       IO_SLC_QUEUE_WTIME,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_WTIME, func),
		       IO_SLC_QUEUE_ERRCNTS,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_ERRCNTS, func),
		       IO_SLC_QUEUE_LRW,
		       (long long)vreadq(c, IO_SLC_VF_QUEUE_LRW, func));
	}
}

int main(int argc, char *argv[])
{
	int ch;
	int dump_queues = 0;
	int card_no = 0;
	int err_code;
	int rc;
	card_handle_t card;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			/* options */
			{ "card",	 required_argument, NULL, 'C' },

			{ "dump-queues", no_argument,       NULL, 'Q' },

			/* misc/support */
			{ "version",	 no_argument,       NULL, 'V' },
			{ "verbose",	 no_argument,       NULL, 'v' },
			{ "help",	 no_argument,       NULL, 'h' },
			{ 0,		 no_argument,       NULL, 0   },
		};

		ch = getopt_long(argc, argv, "C:QVvh", long_options,
				 &option_index);
		if (ch == -1)	/* all params processed ? */
			break;

		switch (ch) {
		/* which card to use */
		case 'C':
			card_no = strtol(optarg, (char **)NULL, 0);
			break;

		case 'Q':
			dump_queues = 1;
			break;

		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;

		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);

		case 'v':
			verbose_flag++;
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

	/* Open the Card */
	card = genwqe_card_open(card_no, GENWQE_MODE_RDWR, &err_code,
				0, GENWQE_APPL_ID_IGNORE);
	if (card == NULL) {
		pr_err("opening genwqe card (err=%d)\n", err_code);
		exit(EXIT_FAILURE);
	}
	genwqe_card_lib_debug(verbose_flag);

	rc = EXIT_FAILURE;
	if (dump_queues) {
		do_dump_queues(card);
		rc = EXIT_SUCCESS;
	}
	/* Close driver */
	genwqe_card_close(card);
	if (rc == EXIT_FAILURE)
		usage(argv[0]);

	exit(rc);
}
