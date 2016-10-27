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
 * This utility updates the GenWQE flash with VPD data. This version
 * can only take a binary input file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <stdbool.h>
#include <asm/byteorder.h>
#include <endian.h>
#include <errno.h>

#include "libcard.h"
#include "genwqe_tools.h"
#include "genwqe_vpd.h"

int verbose_flag = 0;
int _dbg_flag = 0;

static const char *version = GIT_VERSION;

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	fprintf(stdout, "Usage: %s [OPTIONS]...\n"
		"\n"
		"Mandatory arguments to long options are mandatory for short options too.\n"
		"  -h, --help	      print usage information\n"
		"  -V, --version      print version\n"
		"  -C, --card=CARDNO\n"
		"  -f, --file=VPD.BIN\n"
		"  -d, --dump	      use multiple d to increase dump info\n"
		"  -u, --update	      set this flag for update VPD\n"
		"  -s, --show	      set this flag to display VPD from a card\n"
		"  -v, --verbose      verbose level, use multiple v's to increase\n"
		"\n"
		"This utility updates the Genwqes FLASH with new VPD\n"
		"information from a *.bin file. Do not disconnect the card from power\n"
		"while updating. Ensure you have the correct update\n"
		"image. Use of incorrect images or interrupting the update\n"
		"will make the card unusable. In this case you need a\n"
		"USB-Blaster utility or similar to get it working again.\n"
		"\n"
		"Example flashing new vpd to GenWQE card:\n"
		"  %s -C0 -f vpd.bin\n"
		"Example to display vpd from GenWQE card to stdout:\n"
		"  %s -C0 -s\n"
		"Example to display and dump vpd from GenWQE card to stdout:\n"
		"  %s -C0 -s -d\n"
		"\n",
		prog, prog, prog, prog);
}

static int __dump_vpd(card_handle_t card, int dump_level, FILE *fp)
{
	bool bin_ok = false;
	int rc = EXIT_FAILURE;
	genwqe_vpd vpd;
	uint32_t crc = 0;

	rc = genwqe_read_vpd(card, &vpd);
	if (GENWQE_OK == rc) {
		if (dump_level)
			genwqe_hexdump(fp, (uint8_t *)&vpd, VPD_SIZE);

		pr_info("Checking now Binary VPD data from Card\n");
		crc = genwqe_crc32_gen((uint8_t *)&vpd, VPD_SIZE, CRC32_INIT_SEED);
		if (0 == crc)
			pr_info("Found Good VPD CRC\n");
		else	pr_err("Wrong CRC in VPD 0x%x\n", crc);
		pr_info("Display VPD data from Card\n");
		bin_ok = bin_2_csv(fp, VPD_SIZE, (uint8_t *)&vpd);
		if (bin_ok)
			rc = EXIT_SUCCESS;
		else pr_err("Invalid VPD. Use -dd option to dump data.\n");
	} else pr_err("Faild to read VPD from Card (%d). Check -C option.\n", rc);
	return rc;
}

static int __update_vpd(card_handle_t card, FILE *fp)
{
	bool bin_ok = false;
	int rc = EXIT_FAILURE;
	genwqe_vpd vpd;
	int	n;
	uint32_t	crc = 0;

	n = fread((uint8_t *)&vpd, 1 , VPD_SIZE, fp);
	if (VPD_SIZE == n) {
		crc = genwqe_crc32_gen((uint8_t *)&vpd, VPD_SIZE, CRC32_INIT_SEED);
		if (0 == crc) {
			pr_dbg("Input data CRC OK, Updating Card Now.\n");
			bin_ok = bin_2_csv(stdout, VPD_SIZE, (uint8_t *)&vpd);
			if (bin_ok) {
				rc = genwqe_write_vpd(card, &vpd);
				if (rc == GENWQE_OK)
					rc = EXIT_SUCCESS;
			} else pr_err("Invalid input file. Use -v option.\n");
		} else pr_err("Invalid CRC: 0x%x in input file.\n", crc);
	} else pr_err("%s\n", strerror(errno));
	return rc;
}

static struct option long_options[] = {
	/* functions */
	{ "read",	no_argument,	   NULL, 'r' },
	{ "dump",	no_argument,	   NULL, 'd' },
	{ "update",	no_argument,	   NULL, 'u' },
	{ "show",	no_argument,	   NULL, 's' },

	{ "file",	required_argument, NULL, 'f' },
	{ "card",	required_argument, NULL, 'C' },
	{ "version",	no_argument,	   NULL, 'V' },
	{ "verbose",	no_argument,	   NULL, 'v' },
	{ "help",	no_argument,	   NULL, 'h' },
	{ 0,		no_argument,	   NULL, 0   },
};

/**
 * @brief	main function for update Genwqe's Image Flash
 */
int main(int argc, char *argv[])
{
	int ch, rc=0, err_code;
	int card_no = -1;
	int update_vpd = 0;
	int show_vpd = 0;
	card_handle_t card;
	char *env;
	char *fname = NULL;
	FILE	*fp_in = NULL;	// Input file
	FILE	*fp_out = NULL;	// Output file

	while (1) {
		int option_index = 0;
		ch = getopt_long(argc, argv,
				 "vdusC:f:Vh",
				 long_options, &option_index);

		if (ch == -1)	/* all params processed ? */
			break;

		switch (ch) {
			/* which card to use */
		case 'C':	/* -C or --card */
			card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 'f':	/* -f or --file */
			fname = optarg;
			break;
		case 'u':	/* -u or --update */
			update_vpd = 1;
			break;
		case 's':	/* -s or --show */
			show_vpd = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'V':
			fprintf(stdout, "%s\n", version);
			exit(EXIT_SUCCESS);
		case 'v':
			verbose_flag++;
			break;
		case 'd':	/* -d or --dump */
			_dbg_flag++;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (update_vpd && show_vpd) {
		fprintf(stderr, "Please give only -u or -s Option.\n");
		exit(EXIT_FAILURE);
	}
	/* Check Options. i expect either show or update, set fp_in and fp_out */
	if (update_vpd) {	/* -i or --update is set */
		if (NULL == fname) {
			fp_in = stdin;
			pr_info("Input from stdin.\n");
		} else {
			pr_info("Input File: <%s>\n", fname);
			fp_in = fopen(fname, "r");
			if (NULL == fp_in) {
				pr_err("%s Open Errno: <%s>\n",
				       fname, strerror(errno));
				exit (EXIT_FAILURE);
			}
			fp_out = stdout;
		}
	}
	if (show_vpd) {	/* -s or --show is set */
		if (NULL == fname) {
			fp_out = stdout;
			pr_info("Output to stdout.\n");
		} else {
			fp_out = fopen(fname, "w");
			if (NULL == fp_out) {
				pr_err("%s Open Err: <%s>\n",
				       fname, strerror(errno));
				exit (EXIT_FAILURE);
			}
		}
	}
	if ((NULL == fp_in) && (NULL == fp_out)) {
		fprintf(stderr, "Please give -u or -s Option\n");
		exit(EXIT_FAILURE);
	}

	/* simulation is not supported with this tool */
	env = getenv("GENWQE_SIM");
	if ((env) && (atoi(env) > 0)) {
		pr_err("driver / HW simulation active !\n");
		if (show_vpd && fname) fclose(fp_out);
		if (update_vpd && fname) fclose(fp_in);
		exit(EXIT_FAILURE);
	}

	/* Check for a valid card number */
	if (-1 == card_no) {
		pr_err("Specify a valid GENWQE Card number (e.g. -C 0)\n");
		if (show_vpd && fname) fclose(fp_out);
		if (update_vpd && fname) fclose(fp_in);
		exit(EXIT_FAILURE);
	}
	pr_info("Try to open Card: %d\n", card_no);
	card = genwqe_card_open(card_no, GENWQE_MODE_RDWR, &err_code,
				0, GENWQE_APPL_ID_IGNORE);
	if (NULL == card) {
		pr_err("cannot open Genwqe Card: %d (err: %d)\n",
		       card_no, err_code);
		if (show_vpd && fname) fclose(fp_out);
		if (update_vpd && fname) fclose(fp_in);
		exit(EXIT_FAILURE);
	}

	/* No do the Action */
	genwqe_crc32_setup_lut();	/* Setup CRC lu table */
	if (show_vpd)
		rc = __dump_vpd(card, _dbg_flag, fp_out);
	if (update_vpd)
		rc = __update_vpd(card, fp_in);
	genwqe_card_close(card);

	/* Close open files */
	if (show_vpd && fname) fclose(fp_out);
	if (update_vpd && fname) fclose(fp_in);
	exit(rc);
}
