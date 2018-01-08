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
#include "zlib.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

static int verbose = 0;
static unsigned int seed = 0x1974;
static int rnd = 0;
static unsigned int CHUNK_i = 4 * 1024 * 1024; /* 16384; */
static unsigned int CHUNK_o = 4 * 1024 * 1024; /* 16384; */

/* Compress from file source to file dest until EOF on source.
   def() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_STREAM_ERROR if an invalid compression
   level is supplied, Z_VERSION_ERROR if the version of zlib.h and the
   version of the library linked do not match, or Z_ERRNO if there is
   an error reading or writing the files. */
static int def(FILE *source, FILE *dest, int level, int strategy,
	       int windowBits, uint8_t *dictionary, int dictLength)
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
	if (out == NULL) {
		free(in);
		return Z_ERRNO;
	}

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	ret = deflateInit2(&strm, level, Z_DEFLATED, windowBits, 8,
			   strategy);
	if (ret != Z_OK) {
		free(in);
		free(out);
		return ret;
	}

	if (dictLength > 0) {
		ret = deflateSetDictionary(&strm, dictionary, dictLength);
		if (ret != Z_OK) {
			free(in);
			free(out);
			return ret;
		}
	}

	/* compress until end of file */
	do {
		chunk_i = rnd ? random() % CHUNK_i + 1 : CHUNK_i;
		if (verbose) fprintf(stderr, "chunk_i=%d\n", chunk_i);

		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)deflateEnd(&strm);
			free(in);
			free(out);
			return Z_ERRNO;
		}
		flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_in = in;

		/* run deflate() on input until output buffer not full, finish
		   compression if all of source has been read in */
		do {
			chunk_o = rnd ? random() % CHUNK_o + 1 : CHUNK_o;
			if (verbose) fprintf(stderr, "chunk_o=%d\n", chunk_o);

			strm.avail_out = chunk_o;
			strm.next_out = out;
			ret = deflate(&strm, flush);	/* no bad ret value */

			assert(ret != Z_STREAM_ERROR);	/* not clobbered */
			have = chunk_o - strm.avail_out;
			if (fwrite(out, 1, have, dest) != have ||
			    ferror(dest)) {
				(void)deflateEnd(&strm);
				free(in);
				free(out);
				return Z_ERRNO;
			}
		} while (strm.avail_out == 0);
		assert(strm.avail_in == 0);	/* all input will be used */

		/* done when last data in file processed */
	} while (flush != Z_FINISH);
	assert(ret == Z_STREAM_END);	    /* stream will be complete */

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
static int inf(FILE *source, FILE *dest, int windowBits,
	       uint8_t *dictionary, int dictLength)
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
	if (out == NULL) {
		free(in);
		return Z_ERRNO;
	}

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;

	ret = inflateInit2(&strm, windowBits);
	if (ret != Z_OK) {
		free(in);
		free(out);
		return ret;
	}

	if (!((windowBits >= 8) && (windowBits <= 15)) &&  /* !ZLIB */
	    (dictLength > 0)) {
		ret = inflateSetDictionary(&strm, dictionary, dictLength);
		if (ret != Z_OK) {
			free(in);
			free(out);
			return ret;
		}
	}

	/* decompress until deflate stream ends or end of file */
	do {
		chunk_i = rnd ? random() % CHUNK_i + 1 : CHUNK_i;
		if (verbose) fprintf(stderr, "chunk_i=%d\n", chunk_i);

		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)inflateEnd(&strm);
			free(in);
			free(out);
			return Z_ERRNO;
		}
		if (strm.avail_in == 0)
			break;
		strm.next_in = in;

		/* run inflate() on input until output buffer not full */
		do {
		try_again:
			chunk_o = rnd ? random() % CHUNK_o + 1 : CHUNK_o;
			if (verbose) fprintf(stderr, "chunk_o=%d\n", chunk_o);

			strm.avail_out = chunk_o;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH /* Z_SYNC_FLUSH */);

			assert(ret != Z_STREAM_ERROR);	/* not clobbered */
			switch (ret) {
			case Z_NEED_DICT:
				if (((windowBits >= 8) &&
				     (windowBits <= 15)) &&  /* ZLIB! */
				    (dictLength > 0)) {
					ret = inflateSetDictionary(&strm,
								   dictionary,
								   dictLength);
					if (ret != Z_OK) {
						(void)inflateEnd(&strm);
						free(in);
						free(out);
						return ret;
					}
					goto try_again;	/* try again */
				}
				(void)inflateEnd(&strm);
				free(in);
				free(out);
				return ret;
			case Z_DATA_ERROR: /* and fall through */
			case Z_MEM_ERROR:
				(void)inflateEnd(&strm);
				free(in);
				free(out);
				return ret;
			}

			have = chunk_o - strm.avail_out;
			if (fwrite(out, 1, have, dest) != have ||
			    ferror(dest)) {
				(void)inflateEnd(&strm);
				free(in);
				free(out);
				return Z_ERRNO;
			}
		} while (strm.avail_out == 0);

		/* done when inflate() says it's done */
	} while (ret != Z_STREAM_END);

	/* clean up and return */
	(void)inflateEnd(&strm);
	free(in);
	free(out);
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

/* report a zlib or i/o error */
static void zerr(int ret)
{
	fputs("zpipe_rnd: ", stderr);
	switch (ret) {
	case Z_ERRNO:
		if (ferror(stdin))
			fputs("error reading stdin\n", stderr);
		if (ferror(stdout))
			fputs("error writing stdout\n", stderr);
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
	case Z_NEED_DICT:
		fputs("need dictionary data\n", stderr);
		break;
	case Z_VERSION_ERROR:
		fputs("zlib version mismatch!\n", stderr);
		break;
	default:
		fprintf(stderr, "zlib unknown error %d\n", ret);
		break;
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

	fprintf(stderr, "%s usage: %s [-d, --decompress]\n"
		"    [-F, --format <ZLIB|DEFLATE|GZIP>]\n"
		"    [-S, --strategy <0..4>] 0: DEFAULT,\n"
		"      1: FILTERED, 2: HUFFMAN_ONLY, 3: RLE, 4: FIXED\n"
		"    [-r, --rnd\n"		
		"    [-s, --seed <seed>\n"		
		"    [-1, --fast]\n"
		"    [-6, --default]\n"
		"    [-9, --best]\n"
		"    [-i, --i_bufsize <i_bufsize>]\n"
		"    [-D, --dictionary <dictionary>]\n"
		"    [-o, --o_bufsize <o_bufsize>] < source > dest\n",
		b, b);
}

static int figure_out_windowBits(const char *format)
{
	if (strcmp(format, "ZLIB") == 0)
		return 15;	/* 8..15: ZLIB encoding (RFC1950)  */
	else if (strcmp(format, "DEFLATE") == 0)
		return -15; /* -15 .. -8: inflate/deflate (RFC1951) */
	else if (strcmp(format, "GZIP") == 0)
		return 31;	/* GZIP encoding (RFC1952) */

	return 15;
}

/**
 * Load dictionary into buffer.
 * Max size is 32 KiB.
 */
static ssize_t dict_load(const char *fname, uint8_t *buff, size_t len)
{
	int rc;
	FILE *fp;

	if ((fname == NULL) || (buff == NULL) || (len == 0))
		return -EINVAL;

	fp = fopen(fname, "r");
	if (!fp) {
		fprintf(stderr, "Cannot open file %s: %s\n",
			fname, strerror(errno));
		return -1;
	}
	rc = fread(buff, 1, len, fp);
	if (rc == -1)
		fprintf(stderr, "Cannot read file %s: %s\n",
			fname, strerror(errno));

	fclose(fp);
	return rc;
}

/**
 * Compress or decompress from stdin to stdout.
 */
int main(int argc, char **argv)
{
	int ret;
	int compress = 1;
	const char *format = "ZLIB";
	const char *dictName = NULL;
	uint8_t dictionary[32 * 1024];  /* 32 KiB maximum size */
	int dictLength = 0;
	int windowBits;
	int level = Z_DEFAULT_COMPRESSION;
	int strategy = Z_DEFAULT_STRATEGY;

	/* avoid end-of-line conversions */
	SET_BINARY_MODE(stdin);
	SET_BINARY_MODE(stdout);

	while (1) {
		int ch;
		int option_index = 0;
		static struct option long_options[] = {
			{ "decompress",  no_argument,       NULL, 'd' },
			{ "strategy",	 required_argument, NULL, 'S' },
			{ "format",	 required_argument, NULL, 'F' },
			{ "fast",        no_argument,       NULL, '1' },
			{ "default",     no_argument,       NULL, '6' },
			{ "best", 	 no_argument,       NULL, '9' },
			{ "seed",	 required_argument, NULL, 's' },
			{ "i_bufsize",   required_argument, NULL, 'i' },
			{ "o_bufsize",   required_argument, NULL, 'o' },
			{ "dictionary",	 required_argument, NULL, 'D' },
			{ "rnd",	 no_argument,	    NULL, 'r' },
			{ "verbose",	 no_argument,       NULL, 'v' },
			{ "help",	 no_argument,       NULL, 'h' },
			{ 0,		 no_argument,       NULL, 0   },
		};

		ch = getopt_long(argc, argv, "169D:F:rs:i:o:S:dvh?",
				 long_options, &option_index);
		if (ch == -1)    /* all params processed ? */
			break;

		switch (ch) {
			/* which card to use */
		case 'd':
			compress = 0;
			break;
		case 'F':
			format = optarg;
			break;
		case '1':
			level = Z_BEST_SPEED;
			break;
		case '6':
			level = Z_DEFAULT_COMPRESSION;
			break;
		case '9':
			level = Z_BEST_COMPRESSION;
			break;
		case 'D':
			dictName = optarg;
			break;
		case 'r':
			rnd++;
			break;
		case 'v':
			verbose++;
			break;
		case 's':
			seed = str_to_num(optarg);
			break;
		case 'S':
			strategy = str_to_num(optarg);
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

	srandom(seed);
	windowBits = figure_out_windowBits(format);
	dictLength = dict_load(dictName, dictionary, sizeof(dictionary));

	/* do compression if no arguments */
	if (compress == 1) {
		ret = def(stdin, stdout, level, strategy,
			  windowBits, dictionary, dictLength);
		if (ret != Z_OK)
			zerr(ret);
		return ret;
	}

	/* do decompression if -d specified */
	else if (compress == 0) {
		ret = inf(stdin, stdout, windowBits, dictionary, dictLength);
		if (ret != Z_OK)
			zerr(ret);
		return ret;
	}

	/* otherwise, report usage */
	else {
		usage(argv[0]);
		return 1;
	}

	exit(EXIT_SUCCESS);
}
