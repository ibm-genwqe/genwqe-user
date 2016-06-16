/*
 * Copyright 2016, International Business Machines
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
 * zpipe.c: example of proper use of zlib's inflate() and deflate()
 * Not copyrighted -- provided to the public domain
 * Version 1.4	11 December 2005  Mark Adler
 */

/*
 * Test the gzFile functionality provided by zlib.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <libgen.h>
#include <errno.h>
#include <limits.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <asm/byteorder.h>

#include <sched.h>

#include <zlib.h>
#include <zaddons.h>

#define SET_BINARY_MODE(file)

/** common error printf */
#define pr_err(fmt, ...) do {					\
		fprintf(stderr, "gzip: " fmt, ## __VA_ARGS__);	\
	} while (0)

static const char *version = GIT_VERSION;

static unsigned long CHUNK_i = 32 * 1024;
static unsigned long CHUNK_o = 8 * 1024;  /* too small ;-) */
static int verbose = 0;

/**
 * Common tool return codes
 *	 0: EX_OK/EXIT_SUCCESS
 *	 1: Catchall for general errors/EXIT_FAILURE
 *	 2: Misuse of shell builtins (according to Bash documentation)
 *  64..78: predefined in sysexits.h
 *
 * 79..128: Exit codes for our applications
 *
 *     126: Command invoked cannot execute
 *     127: "command not found"
 *     128: Invalid argument to exit
 *   128+n: Fatal error signal "n"
 *     255: Exit status out of range (exit takes only integer args in the
 *	    range 0 - 255)
 */
#define EX_ERRNO	79 /* libc problem */
#define EX_MEMORY	80 /* mem alloc failed */
#define EX_ERR_DATA	81 /* data not as expected */
#define EX_ERR_CRC	82 /* CRC wrong */
#define EX_ERR_ADLER	83 /* Adler checksum wrong */
#define EX_ERR_CARD	84 /* accelerator problem */
#define EX_COMPRESS	85 /* compression did not work */
#define EX_DECOMPRESS	86 /* decompression failed */
#define EX_ERR_DICT	87 /* dictionary compare failed */

/**
 * str_to_num() - Convert string into number and cope with endings like
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

static void usage(FILE *fp, char *prog)
{
	fprintf(fp, "Usage: %s [OPTION]... [FILE]...\n"
		"\n"
		"Report bugs via https://github.com/ibm-genwqe/genwqe-user.\n"
		"\n", prog);
}

static inline
ssize_t file_size(const char *fname)
{
	int rc;
	struct stat s;

	rc = lstat(fname, &s);
	if (rc != 0) {
		fprintf(stderr, "err: Cannot find %s!\n", fname);
		return rc;
	}

	return s.st_size;
}

static inline ssize_t
file_read(const char *fname, uint8_t *buff, size_t len)
{
	int rc;
	FILE *fp;

	if ((fname == NULL) || (buff == NULL) || (len == 0))
		return -EINVAL;

	fp = fopen(fname, "r");
	if (!fp) {
		fprintf(stderr, "err: Cannot open file %s: %s\n",
			fname, strerror(errno));
		return -ENODEV;
	}
	rc = fread(buff, len, 1, fp);
	if (rc == -1) {
		fprintf(stderr, "err: Cannot read from %s: %s\n",
			fname, strerror(errno));
		fclose(fp);
		return -EIO;
	}

	fclose(fp);
	return rc;
}

static inline ssize_t
file_write(const char *fname, const uint8_t *buff, size_t len)
{
	int rc;
	FILE *fp;

	if ((fname == NULL) || (buff == NULL) || (len == 0))
		return -EINVAL;

	fp = fopen(fname, "w+");
	if (!fp) {
		fprintf(stderr, "err: Cannot open file %s: %s\n",
			fname, strerror(errno));
		return -ENODEV;
	}
	rc = fwrite(buff, len, 1, fp);
	if (rc == -1) {
		fprintf(stderr, "err: Cannot write to %s: %s\n",
			fname, strerror(errno));
		fclose(fp);
		return -EIO;
	}

	fclose(fp);
	return rc;
}

static int do_compress(const char *i_fname, const char *o_fname,
		       size_t chunk_i, size_t chunk_o, int level)
{
	char mode[16];
	gzFile ofp;
	FILE *ifp;
	ssize_t len;
	int rc;
	uint8_t *buf;

	ifp = fopen(i_fname, "r");
	if (ifp != NULL) {
		pr_err("Could not open %s\n", i_fname);
		return -1;
	}

	buf = malloc(chunk_i);
	if (NULL == buf) {
		pr_err("%s\n", strerror(errno));
		goto err_ifp;
	}

	sprintf(mode, "wb%d", level);
	ofp = gzopen(o_fname, mode);
	if (ofp != NULL) {
		pr_err("Could not open %s\n", o_fname);
		goto err_buf;
	}

	rc = gzbuffer(ofp, chunk_o);
	if (rc != 0) {
		pr_err("Could not set gzFile buffer size %d\n", rc);
		goto err_ofp;
	}

	do {
		len = fread(buf, 1, chunk_i, ifp);
		if (ferror(ifp)) {
			pr_err("ferror %d\n", (int)len);
			goto err_ofp;
		}
		rc = gzwrite(ofp, buf, len);
		if (rc == 0) {
			pr_err("gzwrite %d\n", rc);
			goto err_ofp;
		}
	} while (!feof(ifp));

	gzclose(ofp);
	free(buf);
	fclose(ifp);

	return 0;

 err_ofp:
	gzclose(ofp);
 err_buf:
	free(buf);
 err_ifp:
	fclose(ifp);
	return -1;
}

static int do_decompress(const char *i_fname, const char *o_fname,
			 size_t chunk_i, size_t chunk_o,
			 off64_t offs __attribute__((unused)),
			 size_t size __attribute__((unused)))
{
	gzFile ifp;
	FILE *ofp;
	ssize_t len;
	int rc;
	uint8_t *buf;

	ofp = fopen(o_fname, "r");
	if (ofp != NULL) {
		pr_err("Could not open %s\n", o_fname);
		return -1;
	}

	buf = malloc(chunk_i);
	if (NULL == buf) {
		pr_err("%s\n", strerror(errno));
		goto err_ofp;
	}

	ifp = gzopen(i_fname, "rb");
	if (ifp != NULL) {
		pr_err("Could not open %s\n", i_fname);
		goto err_buf;
	}

	rc = gzbuffer(ifp, chunk_o);
	if (rc != 0) {
		pr_err("Could not set gzFile buffer size %d\n", rc);
		goto err_ifp;
	}

	do {
		len = gzread(ifp, buf, chunk_i);
		if (len < 0) {
			pr_err("gzread error %d\n", (int)len);
			goto err_ifp;
		}
		rc = fwrite(buf, len, 1, ofp);
		if (rc < 1) {
			pr_err("fwrite %d\n", rc);
			goto err_ifp;
		}

	} while (len < (int)chunk_i); /* is this right? */

	rc = gzclose(ifp);
	if (rc != Z_OK) {
		pr_err("gzclose error %d\n", rc);
		goto err_buf;
	}
	free(buf);
	fclose(ofp);
	return 0;

 err_ifp:
	rc = gzclose(ifp);
	if (rc != Z_OK)
		pr_err("gzclose error %d\n", rc);
 err_buf:
	free(buf);
 err_ofp:
	fclose(ofp);
	return -1;
}


/* compress or decompress from stdin to stdout */
int main(int argc, char **argv)
{
	int rc = Z_OK;
	int level = Z_DEFAULT_COMPRESSION;
	char *prog = basename(argv[0]);
	unsigned long size = 0;
	off64_t offs = 0;
	const char *i_fname = NULL; /* input */
	const char *o_fname = NULL; /* output */
	bool use_compress = true;
	struct stat s;
	const char *accel = "GENWQE";
	const char *accel_env = getenv("ZLIB_ACCELERATOR");
	int card_no = 0;
	const char *card_no_env = getenv("ZLIB_CARD");

	/* Use environment variables as defaults. Command line options
	   can than overrule this. */
	if (accel_env != NULL)
		accel = accel_env;

	if (card_no_env != NULL)
		card_no = atoi(card_no_env);

	/* avoid end-of-line conversions */
	SET_BINARY_MODE(stdin);
	SET_BINARY_MODE(stdout);

	while (1) {
		int ch;
		int option_index = 0;
		static struct option long_options[] = {
			{ "help",	 no_argument,       NULL, 'h' },

			/* our own options */
			{ "accelerator-type", required_argument, NULL, 'A' },
			{ "card_no",	 required_argument, NULL, 'B' },
			{ "size",	 required_argument, NULL, 's' },
			{ "offset",	 required_argument, NULL, 'O' },
			{ "decompress",	 no_argument, NULL, 'd' },
			{ "i_bufsize",   required_argument, NULL, 'i' },
			{ "o_bufsize",   required_argument, NULL, 'o' },
			{ 0,		 no_argument,       NULL, 0   },
		};

		ch = getopt_long(argc, argv, "123456789A:B:i:o:s:O:h?V",
				 long_options, &option_index);
		if (ch == -1)    /* all params processed ? */
			break;

		switch (ch) {

		case 'A':
			accel = optarg;
			break;
		case 'B':
			card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 's':
			size = strtol(optarg, (char **)NULL, 0);
			break;
		case 'O':
			offs = strtol(optarg, (char **)NULL, 0);
			break;
		case 'd':
			use_compress = false;
			break;
		case '1':
			level = Z_BEST_SPEED;
			break;
		case '2':
			level = 2;
			break;
		case '3':
			level = 3;
			break;
		case '4':
			level = 4;
			break;
		case '5':
			level = 5;
			break;
		case '6':
			level = 6;
			break;
		case '7':
			level = 7;
			break;
		case '8':
			level = 8;
			break;
		case '9':
			level = Z_BEST_COMPRESSION;
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			fprintf(stdout, "%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'i':
			CHUNK_i = str_to_num(optarg);
			break;
		case 'o':
			CHUNK_o = str_to_num(optarg);
			break;
		case 'h':
		case '?':
			usage(stdout, prog);
			exit(EXIT_SUCCESS);
			break;
		}
	}

	zlib_set_accelerator(accel, card_no);
	zlib_set_inflate_impl(ZLIB_HW_IMPL);
	zlib_set_deflate_impl(ZLIB_HW_IMPL);

	if (optind < argc) {	       /* input file */
		i_fname = argv[optind++];

		rc = lstat(i_fname, &s);
		if (rc != 0) {
			pr_err("File %s does not exist!\n", i_fname);
			exit(EX_ERRNO);
		}
		if ((rc == 0) && S_ISLNK(s.st_mode)) {
			pr_err("%s: Too many levels of symbolic links\n",
			       i_fname);
			exit(EXIT_FAILURE);
		}
	}
	if (optind < argc) {     /* output file */
		o_fname = argv[optind++];
	} else {
		usage(stderr, prog);
		exit(EXIT_FAILURE);
	}
	if (optind != argc) {   /* now it must fit */
		usage(stderr, prog);
		exit(EXIT_FAILURE);
	}

	fprintf(stderr, "Compress %s to %s in %ld bytes, "
		"out %ld bytes with level %d\n",
		i_fname, o_fname, CHUNK_i, CHUNK_o, level);

	if (use_compress)
		rc = do_compress(i_fname, o_fname, CHUNK_i, CHUNK_o,
				 level);
	else
		rc = do_decompress(i_fname, o_fname, CHUNK_i, CHUNK_o,
				   offs, size);

	exit(rc);
}
