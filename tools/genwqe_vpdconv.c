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
 *
 * Converter utility for GenWQE Project
 *
 * convert from CSV file to binary file
 *	used for making the vpd bin file from a cvs file.
 *	The input file format is defined and fix.
 *	This tool can alos convert a binary file back to
 *	the original CSV file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <asm/byteorder.h>
#include <errno.h>

#include "genwqe_tools.h"
#include "genwqe_vpd.h"

static const char *version = GIT_VERSION;
int verbose_flag = 0;
int _dbg_flag = 0;

static void usage(char *name)
{
    printf("Usage: %s [OPTIONS]...\n"
	   "\n"
	   "Mandatory arguments to long options are mandatory for short options too.\n"
	   "  -h, --help	print usage information\n"
	   "  -V, --version	print version\n"
	   "  -i, --input=FILE	input filename, uses stdin if option is missing\n"
	   "  -o, --output=FILE output filename, uses stdout if option is missing\n"
	   "  -v, --verbose	verbose mode, multiple v's to increase verbosity\n"
	   "  --crcoff		do not check and correct crc in output File\n"
	   "  --reverse		takes as input a binaray file and creates a CSV output file\n"
	   "\n"
	   "This utility converts a comma separated VPD file (CSV file) for the GenWQE Card\n"
	   "to a binary file which can be used for flash programming for VPD data.\n"
	   "The CVS input file format (0x%x) is fix. Only the data can be changed.\n",
	   name, VPD_VERSION);
}

/* Global flags modified by getopt_long() function */
static int  make_crc = 1;
static int  reverse_mode = 0;

static struct option long_options[] = {
	    { "input",	 required_argument, NULL, 'i' },
	    { "output",	 required_argument, NULL, 'o' },
	    { "version", no_argument,	    NULL, 'V' },
	    { "verbose", no_argument,	    NULL, 'v' },
	    { "help",	 no_argument,	    NULL, 'h' },
	    { "crcoff",	 no_argument,	    &make_crc,	   0 },
	    { "reverse", no_argument,	    &reverse_mode, 1 },
	    { 0,	 no_argument,	    NULL,  0 },
};

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
    int option;
    char *input_file = NULL;
    char *output_file = NULL;
    FILE *ip = NULL, *op = NULL;
    uint8_t *buffer = NULL;
    uint32_t	crc32, crc32_from_csv;
    int size, rc = EXIT_SUCCESS;
    union swap_me new_crc32;
    size_t file_size;

    /* Process command line args */
    while (1) {
	int option_index = 0;
	option = getopt_long(argc, argv, "i:o:vVh",
		long_options, &option_index);
	if (EOF == option)    /* all params processed ? */
	    break;

	switch (option) {
	case 0:
	    /* Long options will go here, but i have nothing to do */
	    break;
	case 'i':
	    if (NULL != optarg)
		input_file = optarg;
	    else {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	    }
	    break;
	case 'o':
	    if (NULL != optarg)
		output_file = optarg;
	    else {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	    }
	    break;
	case 'h':
	    usage(argv[0]);
	    exit(EXIT_SUCCESS);
	    break;
	case 'V':
	    fprintf(stdout, "%s\n", version);
	    exit(EXIT_SUCCESS);
	    break;
	case 'v':
	    verbose_flag++;
	    if (verbose_flag >1)
		_dbg_flag++;
	    break;
	default:
	    usage(argv[0]);
	    exit(EXIT_FAILURE);
	    break;
	}
    }

    if (optind < argc) {
	input_file = argv[optind++];
	if (optind < argc) {
	    output_file = argv[optind++];
	    if (optind < argc) {
		pr_err("Too many args\n");
		exit(EXIT_FAILURE);
	    }
	}
    }
    if (input_file) {
	pr_dbg("Input File:   <%s>\n", input_file);
	ip = fopen(input_file, "r");
	if (NULL == ip) {
	    pr_err("%s Open: <%s>\n", strerror(errno), input_file);
	    exit (EXIT_FAILURE);
	}
    }
    if (output_file) {
	pr_dbg("Output File:  <%s>\n", output_file);
	op = fopen(output_file, "w");
	if (NULL == op) {
	    pr_err("%s Open: <%s>\n", strerror(errno),	output_file);
	    if (ip) fclose(ip);
	    exit (EXIT_FAILURE);
	}
    }

    /* Use stdin if ip not set */
    if (NULL == ip) {
	pr_dbg("Read from stdin\n");
	ip = stdin;
    }
    /* Use stdout if op not set */
    if (NULL == op) {
	pr_dbg("Write to stdout\n");
	op = stdout;
    }

    buffer = malloc(GENWQE_VPD_BUFFER_SIZE);
    if (buffer) {
	genwqe_crc32_setup_lut();
	if (1 == reverse_mode) {    // --reverse option was set
	    file_size = fread(buffer, 1, GENWQE_VPD_BUFFER_SIZE, ip);
	    pr_dbg("Bin file now in buffer = %d\n", (int)file_size);
	    if (VPD_SIZE != file_size) {
		pr_err("Your Binary input does have %d of %d Bytes\n",
		    (int)file_size, (int)VPD_SIZE);
	    } else bin_2_csv(op, file_size, buffer);
	} else {
	    if (csv_2_bin(ip, buffer, &size, &crc32, &crc32_from_csv)) {
		if ((0 != crc32) && (1 == make_crc)) {
		    crc32 = genwqe_crc32_gen(buffer, size-4, CRC32_INIT_SEED);
		    new_crc32.ui32 = __be32_to_cpu(crc32);
		    buffer[size-4] = new_crc32.BYTE.ub8[0];
		    buffer[size-3] = new_crc32.BYTE.ub8[1];
		    buffer[size-2] = new_crc32.BYTE.ub8[2];
		    buffer[size-1] = new_crc32.BYTE.ub8[3];
		    pr_info("Input CRC: 0x%x -> Good CRC: 0x%x added to Output.\n",
			crc32_from_csv, crc32);
		}
		fwrite(buffer, 1, size, op);
	    } else {
		rc = EXIT_FAILURE;
	    }
	}
	free(buffer);
    } else {
	pr_err("Malloc(%d)\n", GENWQE_VPD_BUFFER_SIZE);
	rc = EXIT_FAILURE;
    }
    pr_dbg("Close Input and Output.\n");
    fclose(ip);
    fclose(op); // Close Output may result in a empty file on error
    pr_info("Exit with rc: %d\n", rc);
    exit(rc);
}
