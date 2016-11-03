/* zpipe.c: example of proper use of zlib's inflate() and deflate()
   Not copyrighted -- provided to the public domain
   Version 1.4	11 December 2005  Mark Adler */

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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <libgen.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */

#include "zlib.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

static int verbose = 0;
static unsigned int CHUNK_i = 16 * 1024; /* 16384; */
static unsigned int CHUNK_o = 16 * 1024; /* 16384; */

static pid_t gettid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

static int figure_out_window_bits(const char *format)
{
	if (strcmp(format, "ZLIB") == 0)
		return 15;	/* 8..15: ZLIB encoding (RFC1950)  */
	else if (strcmp(format, "DEFLATE") == 0)
		return -15; /* -15 .. -8: inflate/deflate (RFC1951) */
	else if (strcmp(format, "GZIP") == 0)
		return 31;	/* GZIP encoding (RFC1952) */

	return 15;
}

/* Compress from file source to file dest until EOF on source.
   def() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_STREAM_ERROR if an invalid compression
   level is supplied, Z_VERSION_ERROR if the version of zlib.h and the
   version of the library linked do not match, or Z_ERRNO if there is
   an error reading or writing the files. */
static int def(FILE *source, FILE *dest, int window_bits,
	       int level, size_t *compressed_size)
{
	int ret, flush;
	unsigned have;
	z_stream strm;
	unsigned char *in;
	unsigned char *out;
	unsigned int chunk_i = CHUNK_i;
	unsigned int chunk_o = CHUNK_o;

	in = malloc(CHUNK_i);
	if (in == NULL)
		return Z_ERRNO;

	out = malloc(CHUNK_o);
	if (in == NULL)
		return Z_ERRNO;

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.total_out = 0;

	ret = deflateInit2(&strm, level, Z_DEFLATED, window_bits, 8,
			   Z_DEFAULT_STRATEGY);
	if (ret != Z_OK)
		return ret;

	/* compress until end of file */
	do {
		chunk_i = CHUNK_i;
		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)deflateEnd(&strm);
			return Z_ERRNO;
		}
		flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_in = in;

		/* run deflate() on input until output buffer not full, finish
		   compression if all of source has been read in */
		do {
			chunk_o = CHUNK_o;
			strm.avail_out = chunk_o;
			strm.next_out = out;
			ret = deflate(&strm, flush);	/* no bad ret value */
			assert(ret != Z_STREAM_ERROR);	/* not clobbered */
			have = chunk_o - strm.avail_out;
			if (fwrite(out, 1, have, dest) != have || ferror(dest)) {
				(void)deflateEnd(&strm);
				return Z_ERRNO;
			}
		} while (strm.avail_out == 0);
		assert(strm.avail_in == 0);	/* all input will be used */

		/* done when last data in file processed */
	} while (flush != Z_FINISH);
	assert(ret == Z_STREAM_END);	    /* stream will be complete */

	if (compressed_size)
		*compressed_size = strm.total_out;

	/* clean up and return */
	(void)deflateEnd(&strm);
	free(in);
	free(out);
	return Z_OK;
}

/* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
static int inf(FILE *source, FILE *dest, int window_bits,
	       size_t *decompressed_bytes)
{
	int ret;
	unsigned have;
	z_stream strm;
	unsigned char *in;
	unsigned char *out;
	unsigned int chunk_i = CHUNK_i;
	unsigned int chunk_o = CHUNK_o;

	in = malloc(CHUNK_i);
	if (in == NULL)
		return Z_ERRNO;

	out = malloc(CHUNK_o);
	if (in == NULL)
		return Z_ERRNO;

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	strm.total_in = 0;

	ret = inflateInit2(&strm, window_bits);
	if (ret != Z_OK)
		return ret;

	/* decompress until deflate stream ends or end of file */
	do {
		chunk_i = CHUNK_i;
		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)inflateEnd(&strm);
			return Z_ERRNO;
		}
		if (strm.avail_in == 0)
			break;
		strm.next_in = in;

		/* run inflate() on input until output buffer not full */
		do {
			chunk_o = CHUNK_o;
			strm.avail_out = chunk_o;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH /* Z_SYNC_FLUSH */);
			assert(ret != Z_STREAM_ERROR);	/* not clobbered */
			switch (ret) {
			case Z_NEED_DICT:
				ret = Z_DATA_ERROR;	/* and fall through */
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				(void)inflateEnd(&strm);
				return ret;
			}
			have = chunk_o - strm.avail_out;
			if (fwrite(out, 1, have, dest) != have ||
			    ferror(dest)) {
				(void)inflateEnd(&strm);
				return Z_ERRNO;
			}
		} while (strm.avail_out == 0);

		/* done when inflate() says it's done */
	} while (ret != Z_STREAM_END);

	/* clean up and return */
	if (decompressed_bytes)
		*decompressed_bytes = strm.total_in;

	(void)inflateEnd(&strm);
	free(in);
	free(out);
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

/* report a zlib or i/o error */
static void zerr(int ret)
{
	fprintf(stderr, "zpipe_append (%d): ", ret);
	switch (ret) {
	case Z_ERRNO:
		if (ferror(stdin))
			fputs("error reading stdin\n", stderr);
		else if (ferror(stdout))
			fputs("error writing stdout\n", stderr);
		else
			fprintf(stderr, "errno=%d %s\n",
				errno, strerror(errno));
		break;
	case Z_STREAM_ERROR:
		fputs("invalid compression level\n", stderr);
		break;
	case Z_DATA_ERROR:
		fputs("invalid or incomplete deflate data\n", stderr);
		break;
	case Z_MEM_ERROR:
		fputs("out of memory\n", stderr);
		break;
	case Z_VERSION_ERROR:
		fputs("zlib version mismatch!\n", stderr);
		break;
	default:
		fputs("unknown error\n", stderr);
	}
}

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

static void usage(char *prog)
{
	char *b = basename(prog);

	fprintf(stderr, "%s usage: %s [-h] [-v] \\\n"
		"    [-F, --format <ZLIB|DEFLATE|GZIP>]\\\n"
		"    [-i, --i_bufsize <i_bufsize>] \\\n"
		"    [-o, --o_bufsize <o_bufsize>]\n",
		b, b);
}

/* compress or decompress from stdin to stdout */
int main(int argc, char **argv)
{
	int j, rc;
	size_t expected_bytes = 0, decompressed_bytes = 0;
	FILE *i_fp, *o_fp, *n_fp;
	char i_fname[64], o_fname[64], n_fname[64];
	char diff_cmd[128];
	const char *pattern = "This is the END!";
	int window_bits;
	const char *format = "ZLIB";

	/* avoid end-of-line conversions */
	SET_BINARY_MODE(stdin);
	SET_BINARY_MODE(stdout);

	while (1) {
		int ch;
		int option_index = 0;
		static struct option long_options[] = {
			{ "format",	 required_argument, NULL, 'F' },
			{ "i_bufsize",	 required_argument, NULL, 'i' },
			{ "o_bufsize",	 required_argument, NULL, 'o' },
			{ "verbose",	 no_argument,	    NULL, 'v' },
			{ "help",	 no_argument,	    NULL, 'h' },
			{ 0,		 no_argument,	    NULL, 0   },
		};

		ch = getopt_long(argc, argv, "F:i:o:vh?",
				 long_options, &option_index);
		if (ch == -1)    /* all params processed ? */
			break;

		switch (ch) {
			/* which card to use */
		case 'F':
			format = optarg;
			break;
		case 'v':
			verbose++;
			break;
		case 'i':
			CHUNK_i = str_to_num(optarg);
			break;
		case 'o':
			CHUNK_o = str_to_num(optarg);
			break;
		case 'h':
		case '?':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		}
	}

	window_bits = figure_out_window_bits(format);

	sprintf(i_fname, "i_%d_%d.bin", getpid(), gettid());
	sprintf(o_fname, "o_%d_%d.bin", getpid(), gettid());
	sprintf(n_fname, "n_%d_%d.bin", getpid(), gettid());

	/* Write output data */
	i_fp = fopen(i_fname, "w+");
	for (j = 0; j < 1000; j++) {
		rc = fprintf(i_fp, "%d %s %s ...\n", j,
			     i_fname, o_fname);
		if (rc < 0)
			exit(EXIT_FAILURE);
	}
	fclose(i_fp);

	i_fp = fopen(i_fname, "r");   /* original data */
	if (i_fp == NULL)
		exit(EXIT_FAILURE);

	o_fp = fopen(o_fname, "w+");  /* compressed data */
	if (o_fp == NULL)
		exit(EXIT_FAILURE);

	/* Compress data */
	rc = def(i_fp, o_fp, window_bits, Z_DEFAULT_COMPRESSION,
		 &expected_bytes);
	if (rc != Z_OK) {
		fprintf(stderr, "err: compression failed.\n");
		zerr(rc);
		return rc;
	}
	fclose(i_fp);

	/* Append pattern */
	rc = fprintf(o_fp, "%s", pattern);
	fclose(o_fp);

	o_fp = fopen(o_fname, "r");   /* original data */
	if (o_fp == NULL)
		exit(EXIT_FAILURE);

	n_fp = fopen(n_fname, "w+");  /* new original data */
	if (n_fp == NULL)
		exit(EXIT_FAILURE);

	rc = inf(o_fp, n_fp, window_bits, &decompressed_bytes);
	if (expected_bytes != decompressed_bytes) {
		fprintf(stderr, "err: compressed size mismatch %lld!=%lld.\n",
			(long long)expected_bytes,
			(long long)decompressed_bytes);
		exit(EXIT_FAILURE);
	}

	if (rc != Z_OK) {
		fprintf(stderr, "err: decompression failed.\n");
		zerr(rc);
		return rc;
	}

	fclose(o_fp);
	fclose(n_fp);

	sprintf(diff_cmd, "diff -q %s %s", i_fname, n_fname);
	rc = system(diff_cmd);
	if (rc != 0) {
		fprintf(stderr, "Input %s and Output %s differ!\n",
			i_fname, n_fname);
		exit(EXIT_FAILURE);
	}

	exit(EXIT_SUCCESS);
}
