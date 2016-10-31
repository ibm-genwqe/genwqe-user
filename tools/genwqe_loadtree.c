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
 * Load a Text file as a new tree into the Capi Card
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>

#include "genwqe_tools.h"
#include <libddcb.h>

#define MAX_LINE 512

static const char *version = GIT_VERSION;
int	verbose_flag = 0;

static int do_mmio(accel_t card, uint32_t addr, uint64_t data)
{
	int	rc = 0;

	rc = accel_write_reg64(card, addr, (uint64_t)data);
	return rc;
}

static int check_app(accel_t card)
{
	uint64_t	data;
	int	rc;

	/* Check Application Version for Version higher than 0403 */
	/* Register 8 does have following layout for the 64 bits */
	/* RRRRFFIINNNNNNNN */
	/* RRRR     == 16 bit Software Release (0404) */
	/* FF       ==  8 bit Software Fix Level on card (01) */
	/* II       ==  8 bit Software Interface ID (03) */
	/* NNNNNNNN == 32 Bit Function (475a4950) = (GZIP) */
	data = accel_read_reg64(card, 8, &rc);
	if (0 == rc) {
		data = data >> 32;
		if (0x03 == (data & 0xff)) {
			/* Check 16 bits Release */
			data = data >> 16;
			if (data > 0x0402)
				return 0;
		}
	}
	return 1;
}

static void help(char *prog)
{
	printf("Usage: %s [-CvhV] file\n"
		"\t-C, --card <cardno>	Card to use, default is 0\n"
		"\t-V, --version		Print Version number for this tool\n"
		"\t-h, --help		This help message\n"
		"\t-q, --quiet		No output at all\n"
		"\t-v, --verbose	verbose mode, up to -vvv\n",
		prog);
	return;
}

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
	bool quiet = false;
	accel_t	card;
	int	card_no = 0;
	int	err_code;
	int	ch;
	uint32_t	addr;
	long long	data;
	FILE	*fp;
	char	line[MAX_LINE];
	int	line_no, line_len, rc, mmio_done = 0;
	char	*filename = NULL;

	rc = EXIT_SUCCESS;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "card",       required_argument, NULL, 'C' },
			{ "version",    no_argument,       NULL, 'V' },
			{ "quiet",      no_argument,       NULL, 'q' },
			{ "help",       no_argument,       NULL, 'h' },
			{ "verbose",    no_argument,       NULL, 'v' },
			{ 0,            no_argument,       NULL,  0  },
		};
		ch = getopt_long(argc, argv, "C:Vqhv",
			long_options, &option_index);
		if (-1 == ch)
			break;
		switch (ch) {
		case 'C':
			card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'q':
			quiet = true;
			break;
		case 'h':
			help(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'v':
			verbose_flag++;
			break;
		default:
			help(argv[0]);
                	exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		filename = argv[optind++];
		fp = fopen(filename, "r");
		if (NULL == fp) {
			printf("Err: Can not open: %s\n", filename);
                	exit(EXIT_FAILURE);
		}
	} else {
		help(argv[0]);
               	exit(EXIT_FAILURE);
	}
	if (!quiet)
		printf("Using Input Tree File: '%s'\n", filename);

	if (verbose_flag > 1)
		ddcb_debug(verbose_flag);

	if (!quiet && verbose_flag)
		printf("Open CAPI Card: %d\n", card_no);
	card = accel_open(card_no, DDCB_TYPE_CAPI, DDCB_MODE_WR, &err_code,
		0, DDCB_APPL_ID_IGNORE);
	if (NULL == card) {
		printf("Err: failed to open CAPI Card: %u "
                        "(%d / %s)\n", card_no, err_code,
                        accel_strerror(card, err_code));
		printf("\tcheck Permissions in /dev/cxl or kernel log\n");
		fclose(fp);
                exit(EXIT_FAILURE);
	}
	if (check_app(card)) {
		printf("Err: Wrong Card Appl ID. Need to have > 0403\n");
		rc = EXIT_FAILURE;
                goto exit_close;
	}

	line_no = 0;
	mmio_done = 0;
	while (fgets(line, MAX_LINE, fp) != NULL ) {
		line_no++;
		line_len = (int)strlen(line);
		if (30 != line_len) {
			if (!quiet && verbose_flag)
				printf("Skip Line [%d] Invalid Len: %d\n",
					line_no, line_len);
			continue;
		}
		line[line_len - 1] = '\0'; /* remove newline character */
		if (!quiet && verbose_flag)
			printf("Read Line [%d] <%s>\n",
				line_no, line);
		/* only use lines that start with "0x" */
		if ( tolower(line[0]) != '0') {
			if (!quiet && verbose_flag)
				printf("Skip Line [#%d] <%s>\n",
					     line_no, line);
			continue;
		}
		/* e.g. 0x00002100 0x0E0000000008000000 */
		rc = sscanf(&line[0], "0x%x", &addr);
		if (1 != rc) {
			printf("Err: Wrong Addr in Line [#%d]\n",
			     line_no);
			continue;
		}
		if (0x00002100 != (addr & 0xff00)) {
			printf("Err: %08x Wrong MMIO Addr in Line [%d]\n",
				addr, line_no);
			continue;
		}
		rc = sscanf(&line[11], "0x%llx", &data);
		if (1 != rc) {
			printf("Err: Wrong Data in Line [#%d]\n",
			     line_no);
			continue;
		}
		if (!quiet && verbose_flag)
			printf("MMIO Write Addr: %08x Data: %016llx\n",
				addr, data);
		rc = do_mmio(card, addr, (uint64_t)data);
		if (0 != rc) {
			printf("Err: MMIO Write Error Addr: %08x Data: %016llx "
				"at line [%d]\n",
				addr, data, line_no);
			break;
		}
		mmio_done++;
	}
 exit_close:
	if (!quiet && verbose_flag)
		printf("Close Capi Card: %d\n", card_no);
	accel_close(card);
	if (!quiet && verbose_flag)
		printf("Close File: %s\n", filename);
	fclose(fp);
	if (!quiet)
		printf("%s Exit wth Rc: %d (%d MMIO Writes done)\n",
			argv[0], rc, mmio_done);
	exit(rc);
}
