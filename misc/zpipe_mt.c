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
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <asm/byteorder.h>
#include <sched.h>
#include <getopt.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include "zlib.h"

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

/* FIXME Fake this for old RHEL versions e.g. RHEL5.6 */
#ifndef CPU_ALLOC
#define	  CPU_ALLOC(cpus)		      ({ void *ptr = NULL; ptr; })
#define	  CPU_ALLOC_SIZE(cpus)		      ({ int val = 0; val; })
#define	  CPU_ISSET_S(cpu, size, cpusetp)     ({ int val = 0; val; })
#define	  CPU_FREE(cpusetp)
#define	  CPU_ZERO_S(size, cpusetp)
#define	  CPU_SET_S(run_cpu, size, cpusetp)
#define	  sched_getcpu()		      ({ int val = 0; val; })
#define	  sched_setaffinity(x, size, cpusetp) ({ int val = 0; val; })
#endif

/* FIXME Fake this for old RHEL versions e.g. RHEL5.6 */
#ifndef CLOCK_MONOTONIC_RAW
#define   clock_gettime(clk_id, tp) ({ int val = 0; val; })
#endif
				/* HACK Complicated debug cases only */
#undef CONFIG_ERROR_TRIGGER	/* see wrap_hw.c too */
#ifdef CONFIG_ERROR_TRIGGER
extern void error_trigger(void);
#else
static inline void error_trigger(void) { }
#endif

static pthread_mutex_t mutex;
static int verbose = 0;
static int count = 0;
static int use_posix_memalign = 0;
static int pre_alloc_memory = 0;
static unsigned int CHUNK_i = 128 * 1024; /* 16384; */
static unsigned int CHUNK_o = 128 * 1024; /* 16384; */
static unsigned int data_size = 128 * 1024;
static unsigned int threads = 1;
static struct thread_data *d;
static int exit_on_err = 0;

#define pr_dbg(level, fmt, ...) do {					\
		if ((verbose) >= (level))				\
			fprintf(stderr, fmt, ## __VA_ARGS__);		\
	} while (0)

struct thread_data {
	pthread_t thread_id;
	pid_t tid;
	int thread_rc;
	int cpu;

	unsigned long compressions;
	unsigned long decompressions;
	unsigned long compare_ok;

	unsigned char *in;
	unsigned char *out;
} __attribute__((__may_alias__));

/**
 * Try to ping process to a specific CPU. Returns the CPU we are
 * currently running on.
 */
static int pin_to_cpu(int run_cpu)
{
	cpu_set_t *cpusetp;
	size_t size;
	int num_cpus;

	num_cpus = CPU_SETSIZE; /* take default, currently 1024 */
	cpusetp = CPU_ALLOC(num_cpus);
	if (cpusetp == NULL)
		return sched_getcpu();

	size = CPU_ALLOC_SIZE(num_cpus);

	CPU_ZERO_S(size, cpusetp);
	CPU_SET_S(run_cpu, size, cpusetp);
	if (sched_setaffinity(0, size, cpusetp) < 0) {
		CPU_FREE(cpusetp);
		return sched_getcpu();
	}

	/* figure out on which cpus we actually run */
	CPU_FREE(cpusetp);
	return run_cpu;
}

static pid_t gettid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

static inline unsigned long get_nsec(void)
{
	struct timespec ptime = { .tv_sec = 0, .tv_nsec = 0 };
	clock_gettime(CLOCK_MONOTONIC_RAW, &ptime);
	return ptime.tv_sec * 1000000000 + ptime.tv_nsec;
}

static inline void *__malloc(size_t size)
{
	if (use_posix_memalign) {
		int rc;
		void *ptr;

		rc = posix_memalign(&ptr, sysconf(_SC_PAGESIZE), size);
		if (rc != 0) {
			printf("err: errno=%d %s\n", errno, strerror(errno));
			return NULL;
		}
		return ptr;
	}

	return malloc(size);
}

static inline void __free(void *ptr)
{
	if (ptr)
		free(ptr);
}

static int check_for_pattern(const unsigned char *buf, unsigned int len,
			     int it, void *in, void *out, uint8_t pattern)
{
	unsigned int i;
	unsigned int zeros = 0;

	for (i = 0; i < len; i++) {
		if (buf[i] == pattern)
			zeros++;
		else
			zeros = 0;

		if (zeros >= 5) {
			fprintf(stderr, "%08lx.%08lx err: i=%016lx o=%016lx "
				"it=%d: %d or more times \"%02x\" "
				"at %016lx!\n",
				(unsigned long)getpid(),
				(unsigned long)gettid(),
				(unsigned long)in,
				(unsigned long)out,
				it, zeros, pattern,
				(unsigned long)&buf[i] - zeros);
			return 1;
		}
	}
	return 0;
}

/* Compress from file source to file dest until EOF on source.
   def() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_STREAM_ERROR if an invalid compression
   level is supplied, Z_VERSION_ERROR if the version of zlib.h and the
   version of the library linked do not match, or Z_ERRNO if there is
   an error reading or writing the files. */

#undef CONFIG_DUMP_DATA

static int def(struct thread_data *d, FILE *source, FILE *dest,
	       int level, int iter __attribute__((unused)))
{
	int ret, flush;
	unsigned have;
	z_stream strm;
	unsigned char *in;
	unsigned char *out;
	unsigned int chunk_i = CHUNK_i;
	unsigned int chunk_o = CHUNK_o;
	int nr = 0;

	in = (pre_alloc_memory) ? d->in : __malloc(CHUNK_i);
	if (in == NULL)
		return Z_ERRNO;

	out = (pre_alloc_memory) ? d->out : __malloc(CHUNK_o);
	if (out == NULL)
		return Z_ERRNO;

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;

	ret = deflateInit(&strm, level);
	if (ret != Z_OK) {
		if (!pre_alloc_memory) {
			__free(in);
			__free(out);
		}
		return ret;
	}

	/* compress until end of file */
	do {
		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)deflateEnd(&strm);
			if (!pre_alloc_memory) {
				__free(in);
				__free(out);
			}
			return Z_ERRNO;
		}
		flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_in = in;

		/* run deflate() on input until output buffer not full, finish
		   compression if all of source has been read in */
		do {
			strm.avail_out = chunk_o;
			strm.next_out = out;
			memset(strm.next_out, 0xF0, chunk_o);
			if (chunk_o >= 8)
				*((uint32_t *)&out[4]) = gettid();

			pr_dbg(3, "%08lx.%08lx 1) %02x%02x%02x%02x%02x ...\n",
			       (unsigned long)getpid(),
			       (unsigned long)gettid(),
			       out[0], out[1], out[2], out[3], out[4]);

			ret = deflate(&strm, flush);	/* no bad ret value */
			assert(ret != Z_STREAM_ERROR);	/* not clobbered */

			have = chunk_o - strm.avail_out;

			pr_dbg(3, "%08lx.%08lx 2) %02x%02x%02x%02x%02x ...\n",
			       (unsigned long)getpid(),
			       (unsigned long)gettid(),
			       out[0], out[1], out[2], out[3], out[4]);

			if (check_for_pattern(out, have, nr, in, out, 0x00) ||
			    check_for_pattern(out, have, nr, in, out, 0xf0) ||
			    check_for_pattern(out, have, nr, in, out, 0xf1)) {
				exit_on_err = 1;
				error_trigger();
				/* FIXME Write a REGISTER as trigger */
			}
			nr++;

			if (fwrite(out, 1, have, dest) != have ||
			    ferror(dest)) {
				(void)deflateEnd(&strm);
				if (!pre_alloc_memory) {
					__free(in);
					__free(out);
				}
				return Z_ERRNO;
			}
		} while (strm.avail_out == 0);
		assert(strm.avail_in == 0);	/* all input will be used */

		/* done when last data in file processed */
	} while (flush != Z_FINISH);
	assert(ret == Z_STREAM_END);	    /* stream will be complete */

	/* clean up and return */
	(void)deflateEnd(&strm);
	if (!pre_alloc_memory) {
		__free(in);
		__free(out);
	}
	return Z_OK;
}

/* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
static int inf(struct thread_data *d, FILE *source, FILE *dest,
	       int iter __attribute__((unused)))
{
	int ret;
	unsigned have;
	z_stream strm;
	unsigned char *in;
	unsigned char *out;
	unsigned int chunk_i = CHUNK_i;
	unsigned int chunk_o = CHUNK_o;

	in = (pre_alloc_memory) ? d->in : __malloc(CHUNK_i);
	if (in == NULL)
		return Z_ERRNO;

	out = (pre_alloc_memory) ? d->out : __malloc(CHUNK_o);
	if (out == NULL)
		return Z_ERRNO;

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;

	ret = inflateInit(&strm);
	if (ret != Z_OK) {
		if (!pre_alloc_memory) {
			__free(in);
			__free(out);
		}
		return ret;
	}

	/* decompress until deflate stream ends or end of file */
	do {
		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)inflateEnd(&strm);
			if (!pre_alloc_memory) {
				__free(in);
				__free(out);
			}
			return Z_ERRNO;
		}
		if (strm.avail_in == 0)
			break;
		strm.next_in = in;

		/* run inflate() on input until output buffer not full */
		do {
			strm.avail_out = chunk_o;
			strm.next_out = out;
			memset(strm.next_out, 0xF1, chunk_o);

			ret = inflate(&strm, Z_NO_FLUSH /* Z_SYNC_FLUSH */);
			/* assert(ret != Z_STREAM_ERROR); *//* not clobbered */
			switch (ret) {
			case Z_NEED_DICT:
				(void)inflateEnd(&strm);
				if (!pre_alloc_memory) {
					__free(in);
					__free(out);
				}
				return Z_DATA_ERROR;
			case Z_STREAM_ERROR:
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				(void)inflateEnd(&strm);
				if (!pre_alloc_memory) {
					__free(in);
					__free(out);
				}
				return ret;
			}
			have = chunk_o - strm.avail_out;
			if (fwrite(out, 1, have, dest) != have ||
			    ferror(dest)) {
				(void)inflateEnd(&strm);
				if (!pre_alloc_memory) {
					__free(in);
					__free(out);
				}
				return Z_ERRNO;
			}
		} while (strm.avail_out == 0);

		/* done when inflate() says it's done */
	} while (ret != Z_STREAM_END);

	/* clean up and return */
	(void)inflateEnd(&strm);
	if (!pre_alloc_memory) {
		__free(in);
		__free(out);
	}
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

/* report a zlib or i/o error */
static void zerr(int ret)
{
	int xerrno = errno;

	switch (ret) {
	case Z_ERRNO:
		fprintf(stderr, "errno=%d: %s\n", xerrno, strerror(xerrno));
		if (ferror(stdin))
			fputs("error reading stdin\n", stderr);
		if (ferror(stdout))
			fputs("error writing stdout\n", stderr);
		break;
	case Z_STREAM_ERROR:
		fputs("stream error\n", stderr);
		break;
	case Z_DATA_ERROR:
		fprintf(stderr, "invalid or incomplete deflate data (%d)\n",
			ret);
		break;
	case Z_MEM_ERROR:
		fputs("out of memory\n", stderr);
		break;
	case Z_VERSION_ERROR:
		fputs("zlib version mismatch!\n", stderr);
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

	fprintf(stderr, "%s usage: %s\n"
		"    [-X, --cpu <only run on this CPU number>]\n"
		"    [-t, --threads <threads>] # of threads in parallel\n"
		"    [-c, --count <count>]     # of files to comp/decomp\n"
		"    [-p, --use-posix-memalign]# use aligned allocationn\n"
		"    [-P, --pre-alloc-memory]  # zse pre-allocated memoryn\n"
		"    [-i, --i_bufsize <i_bufsize>]\n"
		"    [-o, --o_bufsize <o_bufsize>]\n"
		"    [-d, --data_size <data_size>]\n"
		"\n",
		b, b);
}

static void *libz_thread(void *data)
{
	int rc;
	unsigned int i, len = 0;
	struct thread_data *d = (struct thread_data *)data;
	FILE *i_fp, *o_fp, *n_fp;
	char i_fname[64], o_fname[64], n_fname[64];
	char diff_cmd[128];

	d->tid = gettid();
	d->cpu = sched_getcpu();

	for (i = 0; (i < (unsigned int)count) && (exit_on_err == 0); i++) {
		unsigned int j;
		int new_cpu;

		sprintf(i_fname, "i_%08x_%08x_%d.bin", getpid(), gettid(), i);
		sprintf(o_fname, "o_%08x_%08x_%d.bin", getpid(), gettid(), i);
		sprintf(n_fname, "n_%08x_%08x_%d.bin", getpid(), gettid(), i);

		i_fp = fopen(i_fname, "w+");
		for (j = 0, len = 0; len < data_size; j++) {
			uint64_t x[4];

			/* write binary data */
			x[0] = __cpu_to_be64(0x1122334455667788ull);
			x[1] = __cpu_to_be64((unsigned long)d->in);
			x[2] = __cpu_to_be64((unsigned long)d->out);
			x[3] = __cpu_to_be64((unsigned long)i);

			rc = fwrite(x, 1, sizeof(x), i_fp);
			if (rc <= 0)
				exit(EXIT_FAILURE);
#if CONFIG_ASCII_DATA
			rc = fprintf(i_fp,
				     "%d %s %s in=%016llx out=%016llx ...\n",
				     j, i_fname, o_fname,
				     (long long)d->in,
				     (long long)d->out);
			if (rc < 0)
				exit(EXIT_FAILURE);
#endif

			len += rc;  /* data_size */
		}
		fclose(i_fp);

		i_fp = fopen(i_fname, "r");   /* original data */
		if (i_fp == NULL)
			exit(EXIT_FAILURE);
		o_fp = fopen(o_fname, "w+");  /* compressed data */
		if (o_fp == NULL)
			exit(EXIT_FAILURE);

		pr_dbg(3, "%08x.%08x %d. compressing ...\n",
		       getpid(), gettid(), i);

		rc = def(d, i_fp, o_fp, Z_DEFAULT_COMPRESSION, i);
		if (rc != Z_OK) {
			error_trigger();

			fprintf(stderr, "err/def: rc=%d %s %s %s\n", rc,
				i_fname, o_fname, n_fname);
			zerr(rc);
			goto exit_failure;
		}

		new_cpu = sched_getcpu();
		if (d->cpu != new_cpu) {
			pr_dbg(1, "%08x.%08x CPU moved from %d to %d\n",
			       getpid(), gettid(), d->cpu, new_cpu);
			d->cpu = new_cpu;
		}

		fclose(i_fp);
		fclose(o_fp);
		d->compressions++;

		pr_dbg(3, "%08x.%08x %d. decompressing ...\n",
		       getpid(), gettid(), i);

		o_fp = fopen(o_fname, "r");   /* original data */
		if (o_fp == NULL)
			exit(EXIT_FAILURE);

		n_fp = fopen(n_fname, "w+");  /* new original data */
		if (n_fp == NULL)
			exit(EXIT_FAILURE);

		rc = inf(d, o_fp, n_fp, i);
		if (rc != Z_OK) {
			error_trigger();

			fprintf(stderr, "%08x.%08x err/inf: rc=%d %s %s %s\n",
				getpid(), gettid(), rc,
				i_fname, o_fname, n_fname);
			zerr(rc);

			fprintf(stderr, "Dumping %s ...\n", o_fname);
			sprintf(diff_cmd, "xxd %s", o_fname);
			rc = system(diff_cmd);
			if (rc != 0)
				fprintf(stderr, "%08x.%08x %s: %d\n",
					getpid(), gettid(),
					strerror(errno), errno);

			goto exit_failure;
		}

		new_cpu = sched_getcpu();
		if (d->cpu != new_cpu) {
			pr_dbg(1, "CPU moved from %d to %d\n",
			       d->cpu, new_cpu);
			d->cpu = new_cpu;
		}

		fclose(o_fp);
		fclose(n_fp);
		d->decompressions++;

		sprintf(diff_cmd, "diff -q %s %s", i_fname, n_fname);
		rc = system(diff_cmd);
		if (rc != 0) {
			error_trigger();

			fprintf(stderr, "%08x.%08x In %s and Out %s differ!\n",
				getpid(), gettid(), i_fname, n_fname);
			goto exit_failure;
		}

		d->compare_ok++;
		unlink(i_fname);
		unlink(o_fname);
		unlink(n_fname);
	}

	d->thread_rc = 0;
	pthread_exit(&d->thread_rc);

 exit_failure:
	exit_on_err = 1;
	d->thread_rc = -2;
	pthread_exit(&d->thread_rc);
}

static int run_threads(struct thread_data *d, unsigned int threads)
{
	int rc;
	unsigned int i, errors = 0;

	for (i = 0; i < threads; i++) {
		d[i].thread_rc = -1;
		if (pre_alloc_memory) {
			d[i].in = __malloc(CHUNK_i);
			if (d[i].in == NULL)
				return Z_ERRNO;
			d[i].out = __malloc(CHUNK_o);
			if (d[i].out == NULL)
				return Z_ERRNO;
		}

		rc = pthread_create(&d[i].thread_id, NULL,
				    &libz_thread, &d[i]);
		if (rc != 0) {
			fprintf(stderr,
				"starting %d. libz_thread failed!\n", i);
			return EXIT_FAILURE;
		}
	}

	/* FIXME give some time to setup the tid value ... ;-) */
	sleep(1);
	if (pre_alloc_memory)
		for (i = 0; i < threads; i++)
			fprintf(stderr,
				"  %08lx.%08lx "
				"in:%016lx-%016lx out:%016lx-%016lx\n",
				(unsigned long)getpid(),
				(unsigned long)d[i].tid,
				(unsigned long)d[i].in,
				(unsigned long)d[i].in  + CHUNK_i,
				(unsigned long)d[i].out,
				(unsigned long)d[i].out + CHUNK_i);

	for (i = 0; i < threads; i++) {
		rc = pthread_join(d[i].thread_id, NULL);
		if (rc != 0) {
			fprintf(stderr,
				"joining genwqe_health thread failed!\n");
			return EXIT_FAILURE;
		}
	}

	for (i = 0; i < threads; i++)
		errors += d[i].compressions - d[i].compare_ok;

	if (pre_alloc_memory) {
		for (i = 0; i < threads; i++) {
			__free(d[i].in);
			__free(d[i].out);
		}
	}

	return errors;
}

static void __print_results(struct thread_data *d, unsigned int threads)
{
	unsigned int i, errors = 0;

	fprintf(stderr, "Statistics:\n");
	for (i = 0; i < threads; i++) {
		fprintf(stderr,
			"  %08lx.%08lx thread_id=%08lx rc=%d cmp=%ld "
			"decmp=%ld cmp_ok=%ld\n",
			(unsigned long)getpid(),
			(unsigned long)d[i].tid,
			(unsigned long)d[i].thread_id,
			(int)d[i].thread_rc,
			d[i].compressions,
			d[i].decompressions,
			d[i].compare_ok);
		errors += d[i].compressions - d[i].compare_ok;
	}
	fprintf(stderr, "%d errors found%c\n", errors, errors ? '!' : '.');
}

static void print_results(void)
{
	__print_results(d, threads);
}

/* compress or decompress from stdin to stdout */
int main(int argc, char **argv)
{
	int rc = EXIT_SUCCESS;
	int cpu = -1;

	/* avoid end-of-line conversions */
	SET_BINARY_MODE(stdin);
	SET_BINARY_MODE(stdout);

	while (1) {
		int ch;
		int option_index = 0;
		static struct option long_options[] = {
			{ "cpu",	 required_argument,  NULL, 'X' },
			{ "i_bufsize",	 required_argument,  NULL, 'i' },
			{ "o_bufsize",	 required_argument,  NULL, 'o' },
			{ "data_size",	 required_argument,  NULL, 'd' },
			{ "threads",	 required_argument,  NULL, 't' },
			{ "count",	 required_argument,  NULL, 'c' },
			{ "use-posix-memalign", no_argument, NULL, 'p' },
			{ "pre-alloc-memory", no_argument,   NULL, 'P' },
			{ "verbose",	 no_argument,	     NULL, 'v' },
			{ "help",	 no_argument,	     NULL, 'h' },
			{ 0,		 no_argument,	     NULL, 0   },
		};

		ch = getopt_long(argc, argv, "X:d:Ppc:t:i:o:vh?",
				 long_options, &option_index);
		if (ch == -1)    /* all params processed ? */
			break;

		switch (ch) {
			/* which card to use */
		case 'X':
			cpu = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			verbose++;
			break;
		case 't':
			threads = str_to_num(optarg);
			break;
		case 'i':
			CHUNK_i = str_to_num(optarg);
			break;
		case 'd':
			data_size = str_to_num(optarg);
			break;
		case 'c':
			count = str_to_num(optarg);
			break;
		case 'p':
			use_posix_memalign = 1;
			break;
		case 'P':
			pre_alloc_memory = 1;
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

	pin_to_cpu(cpu);

	d = calloc(threads, sizeof(struct thread_data));
	if (d == NULL)
		return EXIT_FAILURE;

	atexit(print_results);

	rc = pthread_mutex_init(&mutex, NULL);
	if (rc != 0)
		fprintf(stderr, "err: initializing mutex failed!\n");

	rc = run_threads(d, threads);

	pthread_mutex_destroy(&mutex);

	if (rc != 0)
		exit(EXIT_FAILURE);

	exit(EXIT_SUCCESS);
}
