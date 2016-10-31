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
 * @file	genwqe_update.c
 * @brief	Genwqe SW utility.
 * This utility updates the Genwqes Flash with an new image from a *.rbf file
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <asm/byteorder.h>
#include <endian.h>

#include "genwqe_tools.h"
#include "libcard.h"

/* Sysfs entry to figure out the card type */
#define SYSFS_GENWQE_TYPE \
	"/sys/class/" GENWQE_DEVNAME "/" GENWQE_DEVNAME "%d_card/type"

int verbose_flag = 0;

static const char *version = GIT_VERSION;
static char sysfs_type[128] = "unknown";

struct genwqe_type {
	const char *card_id;
	size_t rbf_size;
};

/**
 * The size of the update file *.rbf is an architected value depending
 * on the chip used. If we should ever support a different chip we
 * need to enhance this list or use the --force option.
 */
static const struct genwqe_type card_types[] = {
	{ .card_id = "GenWQE5-A7",  .rbf_size = 33747356 }, /* standard card */
	{ .card_id = "GenWQE5-A4",  .rbf_size = 26724840 }, /* small card */
	{ .card_id = "GenWQE5-530", .rbf_size = 21465258 }, /* big old */
	{ .card_id = "GenWQE4-230", .rbf_size = 11819684 }, /* small old */
	{ .card_id = NULL, .rbf_size = 0 }, /* termination */
};

static size_t get_rbf_size(char *card_type)
{
	const struct genwqe_type *t;

	for (t = &card_types[0]; t->card_id != NULL; t++) {
		if (strcmp(card_type, t->card_id) == 0)
			return t->rbf_size;
	}
	return 0;
}

/**
 * Find out the card type. The card type indicates the size of the
 * update file *.rbf. This we need to know to do some sanity checking
 * to prevent folks from shooting into their food.
 */
static int read_card_type(int card_no)
{
	int rc;
	char sysfs[128];
	FILE *fp;

	snprintf(sysfs, sizeof(sysfs), SYSFS_GENWQE_TYPE, card_no);
	sysfs_type[sizeof(sysfs)-1] = 0;

	fp = fopen(sysfs, "r");
	if (fp == NULL)
		return -1;

	rc = fread(sysfs_type, 1, sizeof(sysfs_type), fp);
	sysfs_type[sizeof(sysfs_type)-1] = 0;
	if (rc <= 0) {
		fclose(fp);
		return -2;
	}

	sysfs_type[strlen(sysfs_type)-1] = 0;  /* remove trailing '\n' */
	fclose(fp);
	return 0;
}


/**
 * str_to_num - Convert string into number and cope with endings like
 *              KiB for kilobyte
 *              MiB for megabyte
 *              GiB for gigabyte
 */
static inline uint64_t str_to_num(char *str)
{
	char *s = str;
	uint64_t num = strtoull(s, &s, 0);

	if (*s == '\0')
		return num;

	if (strcmp(s, "KiB") == 0)
		num *= 1024;
	else if (strcmp(s, "MiB") == 0)
		num *= 1024 * 1024;
	else if (strcmp(s, "GiB") == 0)
		num *= 1024 * 1024 * 1024;

	return num;
}

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s [-h, --help] [-v,--verbose] [-C,--card <cardno>]\n"
	       "\t[-V, --version]\n"
	       "\t[-v, --verbose]\n"
	       "\t[-f, --file <image.rbf>]\n"
	       "\t[-p, --partition <partition>] Default: 1\n"
	       "\t[-x, --verify <0:no|1:yes>]\n"
	       "\n"
	       "This utility updates the Genwqes FLASH with an new image\n"
	       "from an *.rbf file. Do not disconnect the card from power\n"
	       "while updating. Ensure you have the correct update\n"
	       "image. Use of incorrect images or interrupting the update\n"
	       "will make the card unusable."
	       "\nExample flashing a Genwqe default Partition (Partition 1):\n"
	       "  %s -C0 -f chip_a5_latest.rbf\n"
	       "\nExample flashing a Genwqe backup Partition (Partition 0):\n"
	       "  %s -C0 -p 0 -f chip_a5_latest.rbf\n"
	       "\n"
	       "Please note that updating the card can take some time.\n"
	       "So please be patient and do not try to abort this process,\n"
	       "because this might corrupt the card image, and the card\n"
	       "won't work as expected afterwards.\n"
	       "\n",
	       prog, prog, prog);
}

static void print_move_flash_results(int retc, int attn, int progress)
{
	printf("  RETC: %x\n", retc);
	printf("  ATTN: %x ", attn);
	switch (attn) {
	case 0x0000:
		printf("OK\n");
		break;
	case 0x0001:
		printf("Parse Error (length wrong, addr bad, ...)\n");
		break;
	case 0x0002:
		printf("CRC Error (data)\n");
		break;
	case 0x0003:
		printf("Flash programmer timeout/sequence err.\n");
		break;
	case 0x0004:
		printf("DMA Timeout\n");
		break;
	case 0x0005:
		printf("Out of Bound (Addr. collision with images)\n");
		break;
	case 0xe001:
		printf("Allication logicIssued a RC not equal to "
		       "0x102, 0x104, or 0x108\n");
		break;
	case 0xe002:
		printf("Allication violated SQB protocol\n");
		break;
	case 0xe003:
		printf("LEM Attention\n");
		break;
	case 0xe004:
		printf("Timeout (recoverable). Application quieced "
		       "successfully.\n");
		break;
	case 0xe005:
		printf("Application times out, Quiece unsuccessful.\n");
		break;
	case 0xe006:
		printf("Queue Access Error\n");
		break;
	case 0xe007:
		printf("DMA engine override\n");
		break;
	case 0xf000:
		printf("Bad ICRC");
		break;
	case 0xf001:
		printf("Out of Sequence\n");
		break;
	case 0xf002:
		printf("Unsupported Preamble\n");
		break;
	case 0xf003:
		printf("Unsupported ACFUNC\n");
		break;
	case 0xf004:
		printf("SHI mis-sequenced\n");
		break;
	case 0xf005:
		printf("Illegal VF access\n");
		break;
	default:
		printf("unknown\n");
		break;
	}
	printf("  PROGRESS: %x ", progress);
	switch (progress) {
	case 0x0000:
		printf("Command Retrieved.\n");
		break;
	case 0x0100:
		printf("Sector Number N erased\n");
		break;
	case 0x0200:
		printf("All Secors Erased.\n");
		break;
	case 0x0201:
		printf("1st Block flashed.\n");
		break;
	case 0x0203:
		printf("Half Programmed.\n");
		break;
	}
}

/**
 * @brief	main function for update Genwqe's Image Flash
 */
int main(int argc, char *argv[])
{
	int ch, rc, err_code;
	int card_no = 0;
	int read_back = 0;
	int verify = 1;
	int force = 0;
	card_handle_t card;
	struct card_upd_params upd;
	char *env;
	char *pext;
	size_t rbf_size;

	memset(&upd, 0, sizeof(upd));
	upd.partition = '1';	/* Set Default Partition */

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "file",	required_argument, NULL, 'f' },
			{ "card",	required_argument, NULL, 'C' },
			{ "verify",	required_argument, NULL, 'x' },
			/* options */
			{ "partition",	required_argument, NULL, 'p' },

			/* misc/support */
			{ "version",	no_argument,	   NULL, 'V' },
			{ "verbose",	no_argument,	   NULL, 'v' },
			{ "help",	no_argument,	   NULL, 'h' },
			{ 0,		no_argument,	   NULL, 0   },
		};

		ch = getopt_long(argc, argv, "C:f:vVhp:x:",
				 long_options, &option_index);
		if (ch == -1)	/* all params processed ? */
			break;

		switch (ch) {
		/* which card to use */
		case 'C':
			card_no = strtol(optarg, (char **)NULL, 0);
			break;

		case 'f':
			upd.fname = optarg;
			break;
		case 'p':
			upd.partition = *optarg;
			break;
		case 'x':
			verify = strtol(optarg, (char **)NULL, 0);
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

	if (optind < argc)	/* input file */
		upd.fname = argv[optind++];

	if (optind != argc) {	/* now it must fit */
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	genwqe_card_lib_debug(verbose_flag);

	/* simulation is not supported with this tool */
	env = getenv("GENWQE_SIM");
	if ((env) && (atoi(env) > 0)) {
		pr_err("driver / HW simulation active !\n");
		exit(EXIT_FAILURE);
	}

	read_card_type(card_no);
	printf("Card Type: %s\n", sysfs_type);

	rbf_size = get_rbf_size(sysfs_type);
	printf("RBF Size:  %d bytes\n", (int)rbf_size);
	if (upd.flength == 0)
		upd.flength = rbf_size;	 /* take default if nothing is set */

	/* check consistency of parameters */
	if (upd.fname == NULL) {
		pr_err("no input/output file!\n");
		exit(EX_NOINPUT);
	}

	pext = strrchr(upd.fname, '.');
	if (!force && ((pext == NULL) || (strncmp(pext, ".rbf", 5) != 0))) {
		pr_err("'%s' is not an *.rbf file!\n", upd.fname);
		exit(EX_NOINPUT);
	}

	/* Check for 0 and 1 only. Partition v is used for VPD */
	if (upd.partition != '0' && upd.partition != '1') {
		pr_err("partition '%c' invalid\n",
		       isprint(upd.partition) ? upd.partition : '?');
		exit(EX_USAGE);
	}

	/* Open The Card */
	card = genwqe_card_open(card_no, GENWQE_MODE_RDWR, &err_code,
			0, GENWQE_APPL_ID_IGNORE);
	if (card == NULL) {
		pr_err("cannot open card %d! (err=%d)\n", card_no, err_code);
		exit(EXIT_FAILURE);
	}

	/* now do the flash update */
	if (read_back) {
		if (upd.flength == 0) {
			pr_err("don't forget to specify a size!\n");
			rc = EXIT_FAILURE;
			goto __exit;
		}
		rc = genwqe_flash_read(card, &upd);
		if (rc < 0) {
			int xerrno = errno;

			pr_err("reading bitstream failed!\n"
			       "  %s (errno=%d/%s)\n",
			       card_strerror(rc), xerrno, strerror(xerrno));
			print_move_flash_results(upd.retc, upd.attn,
						 upd.progress);
			rc = EXIT_FAILURE;
			goto __exit;
		}
	} else {
		struct stat s;

		rc = lstat(upd.fname, &s);
		if (rc != 0) {
			pr_err("cannot find %s!\n", upd.fname);
			rc = EXIT_FAILURE;
			goto __exit;
		}

		if (!force && (s.st_size != (ssize_t)upd.flength)) {
			pr_err("file size %d bytes does not match required "
			       "size of bitstream %d bytes!\n",
			       (int)s.st_size, upd.flength);
			rc = EXIT_FAILURE;
			goto __exit;
		}

		rc = genwqe_flash_update(card, &upd, verify);
		if (rc < 0) {
			int xerrno = errno;

			if (xerrno == ENOSPC) {
				pr_info("old bitstream with broken readback. "
					"Skipping verification.\n");
				rc = EXIT_SUCCESS;
				goto __exit;
			}
			pr_err("update process failed!\n"
			       "  %s (errno=%d/%s)\n"
			       "  Please ensure that you do not see "
			       "HW222218 where we had problems reading "
			       "flash.\n", card_strerror(rc), xerrno,
			       strerror(xerrno));

			print_move_flash_results(upd.retc, upd.attn,
						 upd.progress);
			rc = EXIT_FAILURE;
		}
	}

 __exit:
	genwqe_card_close(card);
	if (rc == EXIT_SUCCESS)
		printf("update process succeeded\n");
	exit(rc);
}
