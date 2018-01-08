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

/* zpipe.c: example of proper use of zlib's inflate() and deflate()
   Not copyrighted -- provided to the public domain
   Version 1.4	11 December 2005  Mark Adler */

/*
 * Mount debugfs on old RHEL systems:
 *   sudo mount -t debugfs /sys/kernel/debug
 *
 * Repro hardware multithreaded problem with:
 *   export ZLIB_DEFLATE_IMPL=1
 *   export ZLIB_INFLATE_IMPL=1
 *   export ZLIB_IBUF_TOTAL=0
 *   export ZLIB_OBUF_TOTAL=0
 *   make && ./zlib_mt_perf -t4 -c1000 -i1KiB -o1KiB -d4KiB -P
 *
 *
 * Check the influence of multithreading on INFLATE performance:
 *
 *   for t in 1 2 3 4 8 16 32 64 ; do \
 *       ZLIB_INFLATE_IMPL=0x01 ./tests/zlib/tools/zlib_mt_perf \
 *           -i32KiB -o32KiB -f test_data.bin.gz -c2 -t$t ; \
 *   done
 *
 * Same for DEFLATE:
 *   for t in 1 2 3 4 8 16 32 64 ; do \
 *       ZLIB_INFLATE_IMPL=0x01 ./tests/zlib/tools/zlib_mt_perf -D \
 *           -i32KiB -o32KiB -f test_data.bin -c2 -t$t ; \
 *   done
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
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
#include <sys/syscall.h>
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

#ifndef MAX
#  define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

static const char *version = GIT_VERSION;

static pthread_mutex_t mutex;
static bool print_hdr = true;
static int verbose = 0;
static unsigned int count = 0;
static unsigned int CHUNK_i = 128 * 1024; /* 16384; */
static unsigned int CHUNK_o = 128 * 1024; /* 16384; */
static unsigned int threads = 1;
static struct thread_data *d;
static int exit_on_err = 0;
static unsigned int infl_ndefl = 1;	// inflate
static char i_fname[128], c_fname[128];
static unsigned int pin_cpu_ena = 0;
static unsigned long int time_ns_threads = 0;

#define printfv(level, fmt, ...) do {					\
		if ((verbose) >= (level))				\
			fprintf(stderr, fmt, ## __VA_ARGS__);		\
	} while (0)

struct thread_data {
	pthread_t thread_id;    // Thread id assigned by pthread_create()
	pid_t tid;		// inp: thread id
	int thread_rc;		// ret: rc of thread
	int cpu;		// inp: cpu running on
	bool first_run;

	unsigned int comp_calls;    // ret: # of compression calls
	unsigned int decomp_calls;  // ret: # of decompression calls
	unsigned long defl_total;   // ret: total bytes compressed
	unsigned long defl_time;    // ret: total time used for compression
	unsigned long infl_total;   // ret: total bytes decompressed
	unsigned long infl_time;    // ret: total time used for decompression

	unsigned char *in;	// inp: pre-alloc memory ptr
	unsigned char *out;	// inp: pro-alloc memory ptr

	uint32_t checksum;
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
	int rc;
	void *ptr;

	rc = posix_memalign(&ptr, sysconf(_SC_PAGESIZE), size);
	if (rc != 0) {
		fprintf(stderr, "err: errno=%d %s\n", errno, strerror(errno));
		return NULL;
	}

	return ptr;
}

static inline void __free(void *ptr)
{
	if (!ptr)
		return;
	free(ptr);
}

/* Compress from file source to file dest until EOF on source.
   def() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_STREAM_ERROR if an invalid compression
   level is supplied, Z_VERSION_ERROR if the version of zlib.h and the
   version of the library linked do not match, or Z_ERRNO if there is
   an error reading or writing the files. */
static int defl(struct thread_data *d, FILE *source, int level)
{
	int ret, flush;
	//unsigned have;
	z_stream strm;
	unsigned char *in;
	unsigned char *out;
	unsigned int chunk_i = CHUNK_i;
	unsigned int chunk_o = CHUNK_o;
	unsigned long int time_ns_beg, time_ns_end;
	unsigned long int time_ns = 0;

	in = __malloc(CHUNK_i);
	if (in == NULL)
		return Z_ERRNO;

	out = __malloc(CHUNK_o);
	if (out == NULL) {
		__free(in);
		return Z_ERRNO;
	}

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;

	ret = deflateInit2(&strm, level, Z_DEFLATED, 31, 8,
			   Z_DEFAULT_STRATEGY);
	if (ret != Z_OK) {
		__free(in);
		__free(out);
		return ret;
	}

	/* compress until end of file */
	do {
		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)deflateEnd(&strm);
			__free(in);
			__free(out);
			return Z_ERRNO;
		}
		flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_in = in;

		/* run deflate() on input until output buffer not full, finish
		   compression if all of source has been read in */
		do {
			strm.avail_out = chunk_o;
			strm.next_out = out;

			time_ns_beg=get_nsec();
			ret = deflate(&strm, flush);	/* no bad ret value */
			time_ns_end=get_nsec();
			time_ns += (time_ns_end - time_ns_beg);
			d->comp_calls++;
			assert(ret != Z_STREAM_ERROR);	/* not clobbered */

			/* Throw away results, we just like to know
			   how fast we are, no checking done. */
//			have = chunk_o - strm.avail_out;
//			if (fwrite(out, 1, have, dest) != have ||
//			    ferror(dest)) {
//				(void)deflateEnd(&strm);
//				__free(in);
//				__free(out);
//				return Z_ERRNO;
//			}
		} while (strm.avail_out == 0);
		assert(strm.avail_in == 0);	/* all input will be used */

		/* done when last data in file processed */
	} while (flush != Z_FINISH);
	assert(ret == Z_STREAM_END);	    /* stream will be complete */

	d->defl_total += strm.total_in;
	d->defl_time += time_ns;

	ret = Z_OK;
	if (d->first_run) {
		d->checksum = strm.adler;
		d->first_run = false;
	} else if (strm.adler != d->checksum) {
		fprintf(stderr, "Err: checksum mismatch %08lx != %08x\n",
			strm.adler, d->checksum);
		ret = Z_STREAM_ERROR;
	}

	/* clean up and return */
	(void)deflateEnd(&strm);
	__free(in);
	__free(out);
	return ret;
}

/* Decompress from file source to file dest until stream ends or EOF.
   inf() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_DATA_ERROR if the deflate data is
   invalid or incomplete, Z_VERSION_ERROR if the version of zlib.h and
   the version of the library linked do not match, or Z_ERRNO if there
   is an error reading or writing the files. */
static int infl(struct thread_data *d, FILE *source)
{
	int ret;
//	unsigned have;
	z_stream strm;
	unsigned char *in;
	unsigned char *out;
	unsigned int chunk_i = CHUNK_i;
	unsigned int chunk_o = CHUNK_o;
	unsigned long int time_ns_beg, time_ns_end;
	unsigned long int time_ns = 0;

	in = __malloc(CHUNK_i);
	if (in == NULL)
		return Z_ERRNO;

	out = __malloc(CHUNK_o);
	if (out == NULL) {
		__free(in);
		return Z_ERRNO;
	}

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;

	ret = inflateInit2(&strm,31); // GZIP Format
	if (ret != Z_OK) {
		__free(in);
		__free(out);
		return ret;
	}

	/* decompress until deflate stream ends or end of file */
	do {
		strm.avail_in = fread(in, 1, chunk_i, source);
		if (ferror(source)) {
			(void)inflateEnd(&strm);
			__free(in);
			__free(out);
			return Z_ERRNO;
		}
		if (strm.avail_in == 0)
			break;
		strm.next_in = in;

		/* run inflate() on input until output buffer not full */
		do {
			strm.avail_out = chunk_o;
			strm.next_out = out;

			time_ns_beg=get_nsec();
			ret = inflate(&strm, Z_NO_FLUSH /* Z_SYNC_FLUSH */);
			time_ns_end=get_nsec();
			time_ns += (time_ns_end - time_ns_beg);
			d->decomp_calls++;

			/* assert(ret != Z_STREAM_ERROR); *//* not clobbered */
			switch (ret) {
			case Z_NEED_DICT:
				(void)inflateEnd(&strm);
				__free(in);
				__free(out);
				return Z_DATA_ERROR;
			case Z_STREAM_ERROR:
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				(void)inflateEnd(&strm);
				__free(in);
				__free(out);
				return ret;
			}

			/* Throw away results, we just like to know
			   how fast we are, no checking done. */
//			have = chunk_o - strm.avail_out;
//			if (fwrite(out, 1, have, dest) != have ||
//			    ferror(dest)) {
//				(void)inflateEnd(&strm);
//				__free(in);
//				__free(out);
//				return Z_ERRNO;
//			}
		} while (strm.avail_out == 0);

		/* done when inflate() says it's done */
	} while (ret != Z_STREAM_END);

	d->infl_total += strm.total_out;
	d->infl_time += time_ns;

	if (d->first_run) {
		d->checksum = strm.adler;
		d->first_run = false;
	} else if (strm.adler != d->checksum) {
		fprintf(stderr, "Err: checksum mismatch %08lx != %08x\n",
			strm.adler, d->checksum);
		ret = Z_STREAM_ERROR;
	}

	/* clean up and return */
	(void)inflateEnd(&strm);
	__free(in);
	__free(out);
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

	printf("%s usage: %s [OPTIONS]\n"
	       "  -X, --pin_cpu - pin each thread to own cpu\n"
	       "  -t, --threads <threads> threads in parallel\n"
	       "  -c, --count <count> files to comp/decomp\n"
	       "  -i, --i_bufsize <i_bufsize>\n"
	       "  -o, --o_bufsize <o_bufsize>\n"
	       "  -D, --deflate - execute deflate. default: inflate\n"
	       "  -f  --filename <filename>\n"
	       "  -v  --verbose\n"
	       "  -V  --version\n"
	       "\n", b, b);
}

static void *libz_thread_defl(void *data)
{
	int rc;
	unsigned int i;
	struct thread_data *d = (struct thread_data *)data;
	FILE *i_fp;
	d->defl_total=0;
	d->defl_time=0;
	d->comp_calls=0;
	d->tid = gettid();
	d->cpu = sched_getcpu();
	d->first_run = true;
	d->checksum = 0;

	i_fp = fopen(i_fname, "r");   /* original data */
	if (i_fp == NULL) {
		fprintf(stderr, "ERROR: Can't open file %s\n",i_fname);
		exit(EXIT_FAILURE);
	}

	for (i = 0; (i < count) && (exit_on_err == 0); i++) {
		rc = defl(d, i_fp, Z_DEFAULT_COMPRESSION);
		if (rc != Z_OK) {
			fprintf(stderr, "err/def: rc=%d %s\n", rc, i_fname);
			zerr(rc);
			goto exit_failure;
		}
		rewind(i_fp);
	}

	fclose(i_fp);
	d->thread_rc = 0;
	pthread_exit(&d->thread_rc);

 exit_failure:
	fclose(i_fp);
	exit_on_err = 1;
	d->thread_rc = -2;
	pthread_exit(&d->thread_rc);
}

static void *libz_thread_infl(void *data)
{
	int rc;
	unsigned int i;
	struct thread_data *d = (struct thread_data *)data;
	FILE *c_fp;
	d->infl_total = 0;
	d->infl_time = 0;
	d->decomp_calls = 0;
	d->tid = gettid();
	d->cpu = sched_getcpu();
	d->first_run = true;
	d->checksum = 0;

	printfv(1, "   Thread %d using cpu %d\n",d->tid, d->cpu);

	c_fp = fopen(c_fname, "r");   /* original data */
	if (c_fp == NULL) {
		fprintf(stderr, "Error: Can't open file %s\n",c_fname);
		exit(EXIT_FAILURE);
	}

	for (i = 0; (i < count) && (exit_on_err == 0); i++) {
		rc = infl(d, c_fp);
		if (rc != Z_OK) {
			fprintf(stderr, "%08x.%08x err/inf: rc=%d %s\n",
				getpid(), gettid(), rc,	c_fname);
			zerr(rc);
			goto exit_failure;
		}
		rewind(c_fp);
	}

	fclose(c_fp);
	d->thread_rc = 0;
	pthread_exit(&d->thread_rc);

 exit_failure:
	fclose(c_fp);
	exit_on_err = 1;
	d->thread_rc = -2;
	pthread_exit(&d->thread_rc);
}

static int run_threads(struct thread_data *d, unsigned int threads)
{
	int rc;
	unsigned int i;
	unsigned long int time_ns_beg, time_ns_end;

	for (i = 0; i < threads; i++)
		d[i].thread_rc = -1;

	time_ns_beg=get_nsec();	// Take system time at thread begins
	for (i = 0; i < threads; i++) {
		if ( pin_cpu_ena == 1 )
			pin_to_cpu(i);	// pin thread to cpu

		if (infl_ndefl == 1) {
			rc = pthread_create(&d[i].thread_id, NULL,
					&libz_thread_infl, &d[i]);
		} else {
			rc = pthread_create(&d[i].thread_id, NULL,
					&libz_thread_defl, &d[i]);
		}
		if (rc != 0) {
			fprintf(stderr,
				"starting %d. libz_thread failed!\n", i);
			return EXIT_FAILURE;
		}
	}

	for (i = 0; i < threads; i++) {
		rc = pthread_join(d[i].thread_id, NULL);
		if (rc != 0) {
			fprintf(stderr,
				"joining threads failed!\n");
			return EXIT_FAILURE;
		}
	}

	time_ns_end=get_nsec();  // Take system time at thread ends
	time_ns_threads += (time_ns_end - time_ns_beg);
	return EXIT_SUCCESS;
}

static void __print_deflate_results(struct thread_data *d,
				    unsigned int threads)
{
	unsigned int i, error = 0;
	unsigned int comp_calls = 0;
	unsigned long int defl_total = 0;

	if (print_hdr)
		printfv(0, "thread ;    TID ; err ; "
			" #defl ;      bytes ;      time msec ; "
			" throughput MiB/sec ; checksum ; in/out KiB\n");

	for (i = 0; i < threads; i++) {
		printfv(1, "%6d ; %6ld ; %3d ; "
			"%6d ; %10ld ; %10ld     ; %11.3f     ; %08x ;\n",
			i, (unsigned long)d[i].tid, (int)d[i].thread_rc,
			d[i].comp_calls,
			d[i].defl_total,
			d[i].defl_time / 1000,  /* msec */
			d[i].defl_time ?
			d[i].defl_total*1000 / (double)d[i].defl_time : 0.0,
			d[i].checksum);

		if (d[i].thread_rc != 0)
			error = 1;

		comp_calls += d[i].comp_calls;
		defl_total += d[i].defl_total;
	}

	printfv(0, "%6d ;    all ;     ; "
		"%6d ; %10ld ; %10ld     ; %11.3f    ; %08x ; "
		"%d/%d\n", i,
		comp_calls, defl_total, time_ns_threads / 1000, /* msec */
		time_ns_threads ?
		defl_total * 1000/(double)time_ns_threads : 0.0,
		d[0].checksum, CHUNK_i/1024, CHUNK_o/1024);

	if (error == 1) {
		fprintf(stderr, "Error: Thread failed\n");
		return;
	}
}

static void __print_inflate_results(struct thread_data *d,
				    unsigned int threads)
{
	unsigned int i, error=0;
	unsigned int decomp_calls=0;
	unsigned long int infl_total=0;

	if (print_hdr)
		printfv(0, "thread ;    TID ; err ; "
			" #defl ;      bytes ;      time msec ; "
			" throughput MiB/sec ; checksum ; in/out KiB\n");

	for (i = 0; i < threads; i++) {
		printfv(1, "%6d ; %6ld ; %3d ; "
			"%6d ; %10ld ; %10ld     ; %11.3f     ; %08x ;\n",
			i, (unsigned long)d[i].tid, (int)d[i].thread_rc,
			d[i].decomp_calls,
			d[i].infl_total,
			d[i].infl_time / 1000, /* msec */
			d[i].infl_time ?
			d[i].infl_total * 1000 / (double)d[i].infl_time : 0.0,
			d[i].checksum);

		if (d[i].thread_rc != 0)
			error = 1;

		decomp_calls += d[i].decomp_calls;
		infl_total += d[i].infl_total;
	}

	printfv(0, "%6d ;    all ;     ; "
		"%6d ; %10ld ; %10ld     ; %11.3f    ; %08x ; "
		"%d/%d\n", i,
		decomp_calls, infl_total, time_ns_threads / 1000, /* msec */
		time_ns_threads ?
		infl_total * 1000 / (double)time_ns_threads : 0.0,
		d[0].checksum, CHUNK_i/1024, CHUNK_o/1024);

	if (error == 1) {
		fprintf(stderr, "Error: Thread failed\n");
		return;
	}
}

static void print_results(void)
{
	if (infl_ndefl)
		__print_inflate_results(d, threads);
	else
		__print_deflate_results(d, threads);
}

/* compress or decompress from stdin to stdout */
int main(int argc, char **argv)
{
	int rc = EXIT_SUCCESS;

	/* avoid end-of-line conversions */
	SET_BINARY_MODE(stdin);
	SET_BINARY_MODE(stdout);

	while (1) {
		int ch;
		int option_index = 0;
		static struct option long_options[] = {
			{ "pin_cpu",	 no_argument,	     NULL, 'X' },
			{ "i_bufsize",	 required_argument,  NULL, 'i' },
			{ "o_bufsize",	 required_argument,  NULL, 'o' },
			{ "threads",	 required_argument,  NULL, 't' },
			{ "count",	 required_argument,  NULL, 'c' },
			{ "filename",	 required_argument,  NULL, 'f' },
			{ "deflate",	 no_argument,	     NULL, 'D' },
			{ "pre-alloc-memory", no_argument,   NULL, 'P' },
			{ "no-header",	 no_argument,	     NULL, 'N' },
			{ "version",	 no_argument,	     NULL, 'V' },
			{ "verbose",	 no_argument,	     NULL, 'v' },
			{ "help",	 no_argument,	     NULL, 'h' },
			{ 0,		 no_argument,	     NULL, 0   },
		};

		ch = getopt_long(argc, argv, "Xd:f:Dc:t:i:o:NVvh?",
				 long_options, &option_index);
		if (ch == -1)    /* all params processed ? */
			break;

		switch (ch) {
			/* which card to use */
		case 'X':
			pin_cpu_ena = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 't':
			threads = str_to_num(optarg);
			break;
		case 'c':
			count = str_to_num(optarg);
			break;
		case 'i':
			CHUNK_i = str_to_num(optarg);
			break;
		case 'o':
			CHUNK_o = str_to_num(optarg);
			break;
		case 'f':
			sprintf(i_fname, "%s", optarg);
			sprintf(c_fname, "%s", i_fname);
			break;
		case 'D':
			infl_ndefl = 0;
			break;
		case 'N':
			print_hdr = false;
			break;
		case 'V':
			fprintf(stdout, "%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'h':
		case '?':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		}
	}

	d = calloc(threads, sizeof(struct thread_data));
	if (d == NULL)
		return EXIT_FAILURE;

	atexit(print_results);

	rc = pthread_mutex_init(&mutex, NULL);
	if (rc != 0)
		fprintf(stderr, "err: initializing mutex failed!\n");

	rc = run_threads(d, threads);

	pthread_mutex_destroy(&mutex);
	exit(rc);
}
