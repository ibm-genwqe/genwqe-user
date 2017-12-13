/*
 * Copyright 2015, 2016, International Business Machines
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
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#include <zlib.h>		/* standard interface */
#include "libddcb.h"
#include "wrapper.h"

/*
 * Functionality to switch between hardware and software zlib
 * implementations. Enhanced by tracing functionality for debugging
 * and workload analysis. Hardware performs best with sufficiently
 * large input and output buffers.
 *
 * FIXME setDictionary code needs to be properly tested and reviewed.
 *       Most likely it is not working right and needs to be fixed too.
 *
 * The 1st implementation to fallback to SW if the input buffer for
 * inflate is too small was to delay the h_inflateInit until init was
 * called. This finally resulted in a solution where in inflateReset I
 * called inflateEnd, which was causing inflateInit in inflate. This
 * might call obsolete memory allocations and freeing. Therefore I
 * gave up this approach and try to do the inflateEnd and new
 * inflateInit only if the fallback occurs.
 */

/*
 * Select default setting for accelerated zlib. Older version used
 * software as default. Since the library is packaged as extra
 * libz.so, we assume that users of it like to use hardware as
 * default.
 */
#define CONFIG_INFLATE_IMPL	 (ZLIB_HW_IMPL | ZLIB_FLAG_OMIT_LAST_DICT)
#define CONFIG_DEFLATE_IMPL	 (ZLIB_HW_IMPL | ZLIB_FLAG_OMIT_LAST_DICT)

#ifndef DEF_WBITS
#  define DEF_WBITS MAX_WBITS
#endif
/* default windowBits for decompression. MAX_WBITS is for compression only */

#if MAX_MEM_LEVEL >= 8
#  define DEF_MEM_LEVEL 8
#else
#  define DEF_MEM_LEVEL  MAX_MEM_LEVEL
#endif
/* default memLevel */

#define ZLIB_MAXDICTLEN		 (32 * 1024)

/* Good values are something like 8KiB or 16KiB */
#define CONFIG_INFLATE_THRESHOLD (16 * 1024)  /* 0: disabled */

int zlib_trace = 0x0;		/* no trace by default */
FILE *zlib_log = NULL;		/* default is stderr, unless overwritten */
int zlib_accelerator = DDCB_TYPE_GENWQE;
int zlib_card = -1;		/* Using redundant now as default */

unsigned int zlib_inflate_impl  = (CONFIG_INFLATE_IMPL &  ZLIB_IMPL_MASK);
unsigned int zlib_deflate_impl  = (CONFIG_DEFLATE_IMPL &  ZLIB_IMPL_MASK);
unsigned int zlib_inflate_flags = (CONFIG_INFLATE_IMPL & ~ZLIB_IMPL_MASK);
unsigned int zlib_deflate_flags = (CONFIG_DEFLATE_IMPL & ~ZLIB_IMPL_MASK);

static unsigned int zlib_inflate_threshold = CONFIG_INFLATE_THRESHOLD;
pthread_mutex_t zlib_stats_mutex; /* mutex to protect global stats */
struct zlib_stats zlib_stats;	/* global statistics */

/**
 * wrapper internal_state, hw/sw have different view of what
 * internal_state is.
 *
 * NOTE: Since we change the way the software zlib code is invoked,
 * from statically linking a z_ prefixed version to a version which
 * tries to load the code va dlopen/dlsym, we have now situations,
 * where the software libz calls functions like
 * inflate/deflateReset(2). In those cases the strm->state pointer
 * does not point to our own struct _internal_state, but to the
 * software internal state. As temporary or even final circumvention
 * we add here MAGIC0 and MAGIC1 to figure out the difference. If the
 * magic numbers are not setup right, we call the software variant.
 */
#define MAGIC0 0x1122334455667788ull
#define MAGIC1 0xaabbccddeeff00aaull

struct _internal_state {
	uint64_t magic0;
	enum zlib_impl impl;	/* hardware or software implementation */
	void *priv_data;	/* state from level below */
	bool allow_switching;

	/* For delayed inflateInit2() we need to remember parameters */
	int level;
	int method;
	int windowBits;
	int memLevel;
	int strategy;
	const char *version;
	int stream_size;
	gz_headerp gzhead;
	uint64_t magic1;

	Bytef *dictionary;	/* backlevel support for sw zlib < 1.2.8 */
	uInt dictLength;
};

static int has_wrapper_state(z_streamp strm)
{
	struct _internal_state *w;

	if (strm == NULL)
		return 0;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return 0;

	return ((w->magic0 == MAGIC0) && (w->magic1 == MAGIC1));
}

void zlib_set_accelerator(const char *accel, int card_no)
{
	if (strncmp(accel, "CAPI", 4) == 0)
		zlib_accelerator = DDCB_TYPE_CAPI;
	else
		zlib_accelerator = DDCB_TYPE_GENWQE;

	zlib_card = card_no;
}

void zlib_set_inflate_impl(enum zlib_impl impl)
{
	zlib_inflate_impl = impl;
}

void zlib_set_deflate_impl(enum zlib_impl impl)
{
	zlib_deflate_impl = impl;
}

/**
 * str_to_num - Convert string into number and copy with endings like
 *              KiB for kilobyte
 *              MiB for megabyte
 *              GiB for gigabyte
 */
uint64_t str_to_num(char *str)
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
	else {
		num = ULLONG_MAX;
		errno = ERANGE;
	}

	return num;
}

/**
 * Pretty print libz return codes for tracing.
 */
const char *ret_to_str(int ret)
{
	switch (ret) {
	case Z_OK:	      return "Z_OK";
	case Z_STREAM_END:    return "Z_STREAM_END";
	case Z_NEED_DICT:     return "Z_NEED_DICT";
	case Z_ERRNO:	      return "Z_ERRNO";
	case Z_STREAM_ERROR:  return "Z_STREAM_ERROR";
	case Z_DATA_ERROR:    return "Z_DATA_ERROR";
	case Z_MEM_ERROR:     return "Z_MEM_ERROR";
	case Z_BUF_ERROR:     return "Z_BUF_ERROR";
	case Z_VERSION_ERROR: return "Z_BUF_ERROR";
	default:	      return "UNKNOWN";
	}
}

/**
 * Pretty print flush codes for tracing.
 */
const char *flush_to_str(int flush)
{
	switch (flush) {
	case Z_NO_FLUSH:      return "Z_NO_FLUSH";
	case Z_PARTIAL_FLUSH: return "Z_PARTIAL_FLUSH";
	case Z_SYNC_FLUSH:    return "Z_SYNC_FLUSH";
	case Z_FULL_FLUSH:    return "Z_FULL_FLUSH";
	case Z_FINISH:	      return "Z_FINISH";
	case Z_BLOCK:	      return "Z_BLOCK";
#if defined(Z_TREES)		/* older zlibs do not have this */
	case Z_TREES:	      return "Z_TREES";
#endif
	default:	      return "UNKNOWN";
	}
}

static void _init(void) __attribute__((constructor));

/**
 * FIXME With the new zlib load mechanism a new problem arose: How do
 * I prevent us from loading ourselves?
 */
static void _init(void)
{
	int rc;
	const char *trace, *inflate_impl, *deflate_impl, *method;
	const char *zlib_logfile = NULL;
	char *inflate_threshold;

	zlib_logfile = getenv("ZLIB_LOGFILE");
	if (zlib_logfile != NULL) {
		zlib_log = fopen(zlib_logfile, "a+");
		if (zlib_log == NULL)
			zlib_log = stderr;
	} else zlib_log = stderr;

	trace = getenv("ZLIB_TRACE");
	if (trace != NULL)
		zlib_trace = strtol(trace, (char **)NULL, 0);

	deflate_impl  = getenv("ZLIB_DEFLATE_IMPL");
	if (deflate_impl != NULL) {
		zlib_deflate_impl = strtol(deflate_impl, (char **)NULL, 0);
		zlib_deflate_flags = zlib_deflate_impl & ~ZLIB_IMPL_MASK;
		zlib_deflate_impl &= ZLIB_IMPL_MASK;
		if (zlib_deflate_impl >= ZLIB_MAX_IMPL)
			zlib_deflate_impl = ZLIB_SW_IMPL;
	}

	inflate_impl = getenv("ZLIB_INFLATE_IMPL");
	if (inflate_impl != NULL) {
		zlib_inflate_impl = strtol(inflate_impl, (char **)NULL, 0);
		zlib_inflate_flags = zlib_inflate_impl & ~ZLIB_IMPL_MASK;
		zlib_inflate_impl &= ZLIB_IMPL_MASK;
		if (zlib_inflate_impl >= ZLIB_MAX_IMPL)
			zlib_inflate_impl = ZLIB_SW_IMPL;
	}

	inflate_threshold = getenv("ZLIB_INFLATE_THRESHOLD");
	if (inflate_threshold != NULL)
		zlib_inflate_threshold = str_to_num(inflate_threshold);

	/*
	 * Do it similar like zOS did it, such that we can share
	 * test-cases and documentation. If _HZC_COMPRESSION_METHOD is
	 * matching the string "software" we enforce software
	 * operation.
	 */
	method = getenv("_HZC_COMPRESSION_METHOD");
	if ((method != NULL) && (strcmp(method, "software") == 0)) {
		zlib_inflate_impl = ZLIB_SW_IMPL;
		zlib_deflate_impl = ZLIB_SW_IMPL;
	}

	pr_trace("%s: BUILD=%s ZLIB_TRACE=%x ZLIB_INFLATE_IMPL=%d "
		 "ZLIB_DEFLATE_IMPL=%d ZLIB_INFLATE_THRESHOLD=%d\n",
		 __func__, GIT_VERSION, zlib_trace,
		 zlib_inflate_impl, zlib_deflate_impl, zlib_inflate_threshold);

	if (zlib_gather_statistics()) {
		rc = pthread_mutex_init(&zlib_stats_mutex, NULL);
		if (rc != 0)
			pr_err("initializing phtread_mutex failed!\n");
	}

	/* Software is done first such that zlibVersion already work */
	zedc_sw_init();
	zedc_hw_init();
}

static void __deflate_update_totals(z_streamp strm)
{
	unsigned int total_in_slot, total_out_slot;

	if (strm->total_in) {
		total_in_slot = strm->total_in / 4096;
		if (total_in_slot >= ZLIB_SIZE_SLOTS)
			total_in_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.deflate_total_in[total_in_slot]++;
	}
	if (strm->total_out) {
		total_out_slot = strm->total_out / 4096;
		if (total_out_slot >= ZLIB_SIZE_SLOTS)
			total_out_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.deflate_total_out[total_out_slot]++;
	}
}

static void __inflate_update_totals(z_streamp strm)
{
	unsigned int total_in_slot, total_out_slot;

	if (strm->total_in) {
		total_in_slot = strm->total_in / 4096;
		if (total_in_slot >= ZLIB_SIZE_SLOTS)
			total_in_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.inflate_total_in[total_in_slot]++;
	}

	if (strm->total_out) {
		total_out_slot = strm->total_out / 4096;
		if (total_out_slot >= ZLIB_SIZE_SLOTS)
			total_out_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.inflate_total_out[total_out_slot]++;
	}
}

/**
 * Some statistics we print always, others we just print if someone
 * actually called the function. Print out variable if it is not
 * 0. Use variable name as string for the description.
 */
#define __sss(s...)       #s
#define __stringify(s...) __sss(s)

#define pr_stat(s, var) do {						\
		if ((s)->var)						\
			pr_info("%s: %lu\n", __stringify(var), (s)->var); \
	} while (0)

/**
 * __print_stats(): When library is not used any longer, print out
 * statistics e.g. when trace flag is set. This function is not
 * locking stats_mutex.
 */
static void __print_stats(void)
{
	unsigned int i;
	struct zlib_stats *s = &zlib_stats;

	pthread_mutex_lock(&zlib_stats_mutex);
	pr_info("deflateInit: %ld\n", s->deflateInit);
	pr_info("deflate: %ld sw: %ld hw: %ld\n",
		s->deflate[ZLIB_SW_IMPL] + s->deflate[ZLIB_HW_IMPL],
		s->deflate[ZLIB_SW_IMPL], s->deflate[ZLIB_HW_IMPL]);

	for (i = 0; i < ARRAY_SIZE(s->deflate_avail_in); i++) {
		if (s->deflate_avail_in[i] == 0)
			continue;
		pr_info("  deflate_avail_in %4i KiB: %ld\n",
			(i + 1) * 4, s->deflate_avail_in[i]);
	}
	for (i = 0; i < ARRAY_SIZE(s->deflate_avail_out); i++) {
		if (s->deflate_avail_out[i] == 0)
			continue;
		pr_info("  deflate_avail_out %4i KiB: %ld\n",
			(i + 1) * 4, s->deflate_avail_out[i]);
	}
	for (i = 0; i < ARRAY_SIZE(s->deflate_total_in); i++) {
		if (s->deflate_total_in[i] == 0)
			continue;
		pr_info("  deflate_total_in %4i KiB: %ld\n",
			(i + 1) * 4, s->deflate_total_in[i]);
	}
	for (i = 0; i < ARRAY_SIZE(s->deflate_total_out); i++) {
		if (s->deflate_total_out[i] == 0)
			continue;
		pr_info("  deflate_total_out %4i KiB: %ld\n",
			(i + 1) * 4, s->deflate_total_out[i]);
	}

	pr_stat(s, deflateReset);
	pr_stat(s, deflateParams);
	pr_stat(s, deflateBound);
	pr_stat(s, deflateSetDictionary);
	pr_stat(s, deflateSetHeader);
	pr_stat(s, deflatePrime);
	pr_stat(s, deflateCopy);

	pr_info("deflateEnd: %ld\n", s->deflateEnd);
	pr_info("inflateInit: %ld\n", s->inflateInit);
	pr_info("inflate: %ld sw: %ld hw: %ld\n",
		s->inflate[ZLIB_SW_IMPL] + s->inflate[ZLIB_HW_IMPL],
		s->inflate[ZLIB_SW_IMPL], s->inflate[ZLIB_HW_IMPL]);

	for (i = 0; i < ARRAY_SIZE(s->inflate_avail_in); i++) {
		if (s->inflate_avail_in[i] == 0)
			continue;
		pr_info("  inflate_avail_in %4i KiB: %ld\n",
			(i + 1) * 4, s->inflate_avail_in[i]);
	}
	for (i = 0; i < ARRAY_SIZE(s->inflate_avail_out); i++) {
		if (s->inflate_avail_out[i] == 0)
			continue;
		pr_info("  inflate_avail_out %4i KiB: %ld\n",
			(i + 1) * 4, s->inflate_avail_out[i]);
	}
	for (i = 0; i < ARRAY_SIZE(s->inflate_total_in); i++) {
		if (s->inflate_total_in[i] == 0)
			continue;
		pr_info("  inflate_total_in %4i KiB: %ld\n",
			(i + 1) * 4, s->inflate_total_in[i]);
	}
	for (i = 0; i < ARRAY_SIZE(s->inflate_total_out); i++) {
		if (s->inflate_total_out[i] == 0)
			continue;
		pr_info("  inflate_total_out %4i KiB: %ld\n",
			(i + 1) * 4, s->inflate_total_out[i]);
	}

	pr_stat(s, inflateReset);
	pr_stat(s, inflateReset2);
	pr_stat(s, inflateSetDictionary);
	pr_stat(s, inflateGetDictionary);
	pr_stat(s, inflateGetHeader);
	pr_stat(s, inflateSync);
	pr_stat(s, inflatePrime);
	pr_stat(s, inflateCopy);

	pr_info("inflateEnd: %ld\n", s->inflateEnd);

	pr_stat(s, adler32);
	pr_stat(s, adler32_combine);
	pr_stat(s, crc32);
	pr_stat(s, crc32_combine);
	pr_stat(s, adler32_combine64);
	pr_stat(s, crc32_combine64);
	pr_stat(s, get_crc_table);

	pr_stat(s, gzopen64);
	pr_stat(s, gzopen);
	pr_stat(s, gzdopen);
	pr_stat(s, gzbuffer);
	pr_stat(s, gztell64);
	pr_stat(s, gztell);
	pr_stat(s, gzseek64);
	pr_stat(s, gzseek);
	pr_stat(s, gzwrite);
	pr_stat(s, gzread);
	pr_stat(s, gzclose);
	pr_stat(s, gzoffset64);
	pr_stat(s, gzoffset);
	pr_stat(s, gzrewind);
	pr_stat(s, gzputs);
	pr_stat(s, gzgets);
	pr_stat(s, gzputc);
	pr_stat(s, gzgetc);
	pr_stat(s, gzungetc);
	pr_stat(s, gzprintf);
	pr_stat(s, gzerror);
	pr_stat(s, gzeof);
	pr_stat(s, gzflush);

	pr_stat(s, compress);
	pr_stat(s, compress2);
	pr_stat(s, compressBound);
	pr_stat(s, uncompress);

	pthread_mutex_unlock(&zlib_stats_mutex);
}

/**
 * If there is no hardware available we retry automatically the
 * software version.
 */
static int __deflateInit2_(z_streamp strm, struct _internal_state *w)
{
	int rc = Z_OK;
	int retries = 0;

	/* drop to SW mode, HW does not support level 0 */
	if (w->level == Z_NO_COMPRESSION)
		w->impl = ZLIB_SW_IMPL;

	do {
		pr_trace("[%p] __deflateInit2_: w=%p level=%d method=%d "
			 "windowBits=%d memLevel=%d strategy=%d version=%s/%s "
			 "stream_size=%d impl=%d\n",
			 strm, w, w->level, w->method, w->windowBits,
			 w->memLevel, w->strategy, w->version,
			 zlibVersion(), w->stream_size, w->impl);

		rc = w->impl ? h_deflateInit2_(strm, w->level, w->method,
					       w->windowBits, w->memLevel,
					       w->strategy, w->version,
					       w->stream_size) :
			       z_deflateInit2_(strm, w->level, w->method,
					       w->windowBits, w->memLevel,
					       w->strategy, w->version,
					       w->stream_size);
		if (rc != Z_OK) {
			pr_trace("[%p] %s: fallback to software (rc=%d)\n",
				 strm, __func__, rc);
			w->impl = ZLIB_SW_IMPL;
			retries++;
		}
	} while ((retries < 2) && (rc != Z_OK));

	return rc;
}

/**
 * deflateInit2_() - Initialize deflate context. If the hardware
 * implementation fails for some reason the code tries the software
 * version.
 */
int deflateInit2_(z_streamp strm,
		  int level,
		  int method,
		  int windowBits,
		  int memLevel,
		  int strategy,
		  const char *version,
		  int stream_size)
{
	int rc = Z_OK;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.deflateInit);

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return Z_ERRNO;

	w->magic0 = MAGIC0;
	w->magic1 = MAGIC1;
	w->level = level;
	w->method = method;
	w->windowBits = windowBits;
	w->memLevel = memLevel;
	w->strategy = strategy;
	w->version = version;
	w->stream_size = stream_size;
	w->priv_data = NULL;
	w->impl = zlib_deflate_impl; /* try default first */

	rc = __deflateInit2_(strm, w);
	if (rc != Z_OK) {
		free(w);
	} else {
		w->priv_data = strm->state;	/* backup sublevel state */
		strm->state = (void *)w;
	}
	return rc;
}

int deflateInit_(z_streamp strm, int level, const char *version,
		 int stream_size)
{
	return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, DEF_MEM_LEVEL,
			     Z_DEFAULT_STRATEGY, version, stream_size);
}

int deflateReset(z_streamp strm)
{
	int rc;
	struct _internal_state *w;

	if (!has_wrapper_state(strm))
		return z_deflateReset(strm);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	pr_trace("[%p] deflateReset w=%p impl=%d\n", strm, w, w->impl);
	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		zlib_stats.deflateReset++;
		__deflate_update_totals(strm);
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	strm->state = w->priv_data;
	rc = w->impl ? h_deflateReset(strm) :
		       z_deflateReset(strm);

	strm->state = (void *)w;
	return rc;
}

int deflateSetDictionary(z_streamp strm,
			 const Bytef *dictionary,
			 uInt  dictLength)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	pr_trace("[%p] deflateSetDictionary: dictionary=%p dictLength=%d "
		 "adler32=%08llx\n", strm, dictionary, dictLength,
		 (long long)z_adler32(1, dictionary, dictLength));

	zlib_stats_inc(&zlib_stats.deflateSetDictionary);

	strm->state = w->priv_data;
	rc = w->impl ? h_deflateSetDictionary(strm, dictionary, dictLength) :
		       z_deflateSetDictionary(strm, dictionary, dictLength);
	
	pr_trace("[%p]    calculated adler32=%08x\n", strm,
		 (unsigned int)strm->adler);
	strm->state = (void *)w;

	return rc;
}

int deflateSetHeader(z_streamp strm, gz_headerp head)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	pr_trace("[%p] deflateSetHeader\n", strm);
	zlib_stats_inc(&zlib_stats.deflateSetHeader);

	strm->state = w->priv_data;
	rc = w->impl ? h_deflateSetHeader(strm, head) :
		       z_deflateSetHeader(strm, head);
	strm->state = (void *)w;

	return rc;
}

int deflatePrime(z_streamp strm, int bits, int value)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.deflatePrime);

	strm->state = w->priv_data;
	rc = w->impl ? Z_UNSUPPORTED :
		       z_deflatePrime(strm, bits, value);
	strm->state = (void *)w;

	return rc;
}

int deflateCopy(z_streamp dest, z_streamp source)
{
	int rc;
	struct _internal_state *w_source;
	struct _internal_state *w_dest;

	pr_trace("[%p] deflateCopy: dest=%p source=%p\n",
		 source, dest, source);

	if ((dest == NULL) || (source == NULL))
		return Z_STREAM_ERROR;

	memcpy(dest, source, sizeof(*dest));

	w_source = (struct _internal_state *)source->state;
	if (w_source == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.deflateCopy);

	w_dest = calloc(1, sizeof(*w_dest));
	if (w_dest == NULL)
		return Z_ERRNO;

	memcpy(w_dest, w_source, sizeof(*w_dest));
	source->state = w_source->priv_data;
	dest->state = NULL;	/* this needs to be created */

	rc = w_source->impl ? h_deflateCopy(dest, source):
			      z_deflateCopy(dest, source);
	if (rc != Z_OK) {
		pr_err("[%p] deflateCopy returned %d\n", source, rc);
		free(w_dest);
		w_dest = NULL;
		goto err_out;
	}

	w_dest->priv_data = dest->state;
	dest->state   = (void *)w_dest;

 err_out:
	source->state = (void *)w_source;
	return rc;
}

int deflate(z_streamp strm, int flush)
{
	int rc = 0;
	struct _internal_state *w;
	unsigned int avail_in_slot, avail_out_slot;

	if (0 == has_wrapper_state(strm)) {
		rc = z_deflate(strm, flush);
		return rc;
	}

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		avail_in_slot = strm->avail_in / 4096;
		if (avail_in_slot >= ZLIB_SIZE_SLOTS)
			avail_in_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.deflate_avail_in[avail_in_slot]++;

		avail_out_slot = strm->avail_out / 4096;
		if (avail_out_slot >= ZLIB_SIZE_SLOTS)
			avail_out_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.deflate_avail_out[avail_out_slot]++;
		zlib_stats.deflate[w->impl]++;
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	pr_trace("[%p] deflate:   flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_out=%ld crc/adler=%08lx "
		 "impl=%d\n", strm, flush_to_str(flush), strm->next_in,
		 strm->avail_in, strm->next_out, strm->avail_out,
		 strm->total_out, strm->adler, w->impl);

	strm->state = w->priv_data;
	/* impl can only be ZLIB_HW_IMPL or ZLIB_SW_IMPL */
	switch (w->impl) {
	case ZLIB_HW_IMPL:
		rc = h_deflate(strm, flush);
		break;
	case ZLIB_SW_IMPL:
		rc = z_deflate(strm, flush);
		break;
	default:
		pr_trace("[%p] deflate: impl (%d) is not valid for me\n",
			 strm, w->impl);
		break;
	}
	strm->state = (void *)w;

	pr_trace("[%p]            flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_out=%ld crc/adler=%08lx "
		 "rc=%s\n", strm, flush_to_str(flush), strm->next_in,
		 strm->avail_in, strm->next_out, strm->avail_out,
		 strm->total_out, strm->adler, ret_to_str(rc));

	return rc;
}

static int __deflateEnd(z_streamp strm, struct _internal_state *w)
{
	int rc;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	if (w == NULL)
		return Z_STREAM_ERROR;

	strm->state = w->priv_data;
	rc = w->impl ? h_deflateEnd(strm) :
		       z_deflateEnd(strm);
	strm->state = NULL;

	return rc;
}

uLong deflateBound(z_streamp strm, uLong sourceLen)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL) {
		return MAX(h_deflateBound(NULL, sourceLen),
			   z_deflateBound(NULL, sourceLen));
	}

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.deflateBound);

	strm->state = w->priv_data;
	rc = w->impl ? h_deflateBound(strm, sourceLen) :
		       z_deflateBound(strm, sourceLen);

	strm->state = (void *)w;
	return rc;
}

int deflateEnd(z_streamp strm)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		zlib_stats.deflateEnd++;
		__deflate_update_totals(strm);
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	rc = __deflateEnd(strm, w);

	pr_trace("[%p] deflateEnd w=%p rc=%d\n", strm, w, rc);
	free(w);

	return rc;
}

/**
 * HW283780 LIR 40774: java.lang.InternalError seen when NoCompression
 * is set in Hardware Mode on Linux P
 *
 * Once we are in HW compression mode the HW will always do the
 * same. There is not chance here, like it is in software to change
 * the level or strategy. We return Z_OK to keep the calling code
 * happy. If that code would start checking the resulting data, it
 * will see that the HW compression was not doing what it was supposed
 * to do e.g. do a sync and produce copy-blocks e.g. when level is set
 * to 0.
 */
int deflateParams(z_streamp strm, int level, int strategy)
{
	int rc = Z_OK;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	/* Let us adjust level and strategy */
	w->level = level;
	w->strategy = strategy;
	zlib_stats_inc(&zlib_stats.deflateParams);

	pr_trace("[%p] deflateParams level=%d strategy=%d impl=%d\n",
		 strm, level, strategy, w->impl);

	strm->state = w->priv_data;
	switch (w->impl) {
	case ZLIB_HW_IMPL:
		/*
                 * For the Z_NO_COMPRESSION case, implement fallback
                 * to software. This is for the case where w->level
                 * was have been setup by deflateParams().
		 */
		if ((strm->total_in != 0) || (w->level != Z_NO_COMPRESSION)) {
			strm->state = (void *)w;
			return Z_OK;
		}

		/* Redo initialization in software mode */
		pr_trace("[%p]   Z_NO_COMPRESSION total_in=%ld\n",
			 strm, strm->total_in);

		rc = __deflateEnd(strm, w);
		if (rc != Z_OK)
			goto err;

		strm->total_in = 0;
		strm->total_out = 0;

		rc = __deflateInit2_(strm, w);
		if (rc != Z_OK)
			goto err;

		w->priv_data = strm->state; /* backup sublevel state */
		break;
	case ZLIB_SW_IMPL:
		rc = z_deflateParams(strm, level, strategy);
		break;
	default:
		pr_err("[%p] deflateParams impl=%d invalid\n", strm, w->impl);
		break;
	}

 err:
	strm->state = (void *)w;
	return rc;
}

static int __inflateInit2_(z_streamp strm, struct _internal_state *w)
{
	int rc, retries;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	if (w == NULL)
		return Z_STREAM_ERROR;

	retries = 0;
	do {
		pr_trace("[%p] inflateInit2_: w=%p windowBits=%d "
			 "version=%s/%s stream_size=%d impl=%d\n",
			 strm, w, w->windowBits,
			 w->version, zlibVersion(), w->stream_size, w->impl);

		rc = w->impl ? h_inflateInit2_(strm, w->windowBits, w->version,
					       w->stream_size) :
			       z_inflateInit2_(strm, w->windowBits, w->version,
					       w->stream_size);
		if (Z_OK == rc)
			break;	/* OK, i Can exit now */

		pr_trace("[%p] %s: fallback to software (rc=%d)\n",
			 strm, __func__, rc);
		w->impl = ZLIB_SW_IMPL;
		w->allow_switching = false;

		retries++;
	} while (retries < 2);

	if (rc != Z_OK)
		goto err;

	w->priv_data = strm->state;		/* backup sublevel state */
 err:
	return rc;
}

int inflateInit2_(z_streamp strm, int  windowBits,
		  const char *version, int stream_size)
{
	int rc = Z_OK;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	strm->total_in = 0;
	strm->total_out = 0;
	zlib_stats_inc(&zlib_stats.inflateInit);

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return Z_MEM_ERROR;

	w->allow_switching = true;
	w->magic0 = MAGIC0;
	w->magic1 = MAGIC1;
	w->windowBits = windowBits;
	w->version = version;
	w->stream_size = stream_size;
	w->priv_data = NULL;
	w->impl = zlib_inflate_impl; /* try default first */
	w->dictLength = 0;

	if (!z_hasGetDictionary()) {
		w->dictionary = calloc(1, ZLIB_MAXDICTLEN);
		if (w->dictionary == NULL) {
			rc = Z_MEM_ERROR;
			goto free_w;
		}
	}

	rc = __inflateInit2_(strm, w);
	if (rc == Z_OK)
		strm->state = (void *)w;
	else
		goto free_dict;

	return rc;

 free_dict:
	if (w->dictionary) {
		free(w->dictionary);
		w->dictionary = NULL;
	}
 free_w:
	free(w);
	return rc;
}

int inflateInit_(z_streamp strm, const char *version, int stream_size)
{
	return inflateInit2_(strm, DEF_WBITS, version, stream_size);
}

int inflateReset(z_streamp strm)
{
	int rc;
	struct _internal_state *w;

	if (!has_wrapper_state(strm))
		return z_inflateReset(strm);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	/*
	 * We need to count even though priv_data could still be
	 * NULL. Otherwise the statistics will not be right at the
	 * end.
	 */
	pr_trace("[%p] inflateReset\n", strm);
	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		zlib_stats.inflateReset++;
		__inflate_update_totals(strm);
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	w->allow_switching = true;
	w->gzhead = NULL;	/* clear gz header */
	w->dictLength = 0;	/* clear cached dictionary */

	strm->state = w->priv_data;
	rc = (w->impl) ? h_inflateReset(strm) :
			 z_inflateReset(strm);

	strm->total_in = 0;
	strm->total_out = 0;
	strm->state = (void *)w;

	return rc;
}

extern int inflateReset2(z_streamp strm, int windowBits);
int inflateReset2(z_streamp strm, int windowBits)
{

	int rc;
	struct _internal_state *w;

	if (!has_wrapper_state(strm))
		return z_inflateReset2(strm, windowBits);

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	/*
	 * We need to count even though priv_data could still be
	 * NULL. Otherwise the statistics will not be right at the
	 * end.
	 */
	pr_trace("[%p] inflateReset2 impl=%d\n", strm, w->impl);
	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		zlib_stats.inflateReset2++;
		__inflate_update_totals(strm);
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	w->allow_switching = true;
	w->dictLength = 0;	/* clear cached dictionary */

	strm->state = w->priv_data;
	rc = (w->impl) ? h_inflateReset2(strm, windowBits) :
			 z_inflateReset2(strm, windowBits);

	strm->total_in = 0;
	strm->total_out = 0;
	strm->state = (void *)w;

	return rc;
}

int inflateSetDictionary(z_streamp strm,
			 const Bytef *dictionary,
			 uInt  dictLength)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.inflateSetDictionary);

	strm->state = w->priv_data;
	if (w->impl)
		rc = h_inflateSetDictionary(strm, dictionary, dictLength);
	else {
		rc = z_inflateSetDictionary(strm, dictionary, dictLength);

		/* Update a private copy for the case we have not
		   inflateGetDict */
		if (!z_hasGetDictionary()) {
			memcpy(w->dictionary, dictionary,
			       MIN((uInt)ZLIB_MAXDICTLEN, dictLength));
			w->dictLength = dictLength;
		}
	}
	strm->state = (void *)w;

	pr_trace("[%p] inflateSetDictionary: dictionary=%p dictLength=%d "
		 "adler32=%08llx rc=%d\n", strm,
		 dictionary, dictLength,
		 (long long)z_adler32(1, dictionary, dictLength), rc);

	return rc;
}

/**
 * zlib older than 1.2.8 has no inflateGetDictionary. To get the
 * software/hardware switching working without this function we create
 * a copy of the dictionary. I a user has set it via
 * inflateSetDictionary, we have still a copy in this code which we
 * can use.
 */
extern int inflateGetDictionary(z_streamp strm, Bytef *dictionary,
				uInt *dictLength);
int inflateGetDictionary(z_streamp strm, Bytef *dictionary, uInt *dictLength)
{
	int rc = Z_OK;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.inflateGetDictionary);

	strm->state = w->priv_data;
	if (w->impl)
		rc = h_inflateGetDictionary(strm, dictionary, dictLength);
	else {
		if (z_hasGetDictionary())
			rc = z_inflateGetDictionary(strm, dictionary,
						    dictLength);
		else {
			memcpy(dictionary, w->dictionary, w->dictLength);
			if (dictLength)
				*dictLength = w->dictLength;
		}
	}
	strm->state = (void *)w;

	pr_trace("[%p] inflateGetDictionary: dictionary=%p &dictLength=%p "
		 "rc=%d\n", strm, dictionary, dictLength, rc);

	return rc;
}

int inflateGetHeader(z_streamp strm, gz_headerp head)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	pr_trace("[%p] inflateGetHeader: head=%p\n", strm, head);
	zlib_stats_inc(&zlib_stats.inflateGetHeader);

	w->gzhead = head;
	strm->state = w->priv_data;
	rc = w->impl ? h_inflateGetHeader(strm, head) :
		       z_inflateGetHeader(strm, head);
	strm->state = (void *)w;
	return rc;
}


int inflatePrime(z_streamp strm, int bits, int value)
{
	int rc;
	struct _internal_state *w = (struct _internal_state *)strm->state;

	if (w == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.inflatePrime);

	strm->state = w->priv_data;
	rc = w->impl ? Z_UNSUPPORTED :
		       z_inflatePrime(strm, bits, value);
	strm->state = (void *)w;

	return rc;
}

int inflateSync(z_streamp strm)
{
	int rc;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	zlib_stats_inc(&zlib_stats.inflateSync);

	strm->state = w->priv_data;
	rc = w->impl ? Z_UNSUPPORTED :
		       z_inflateSync(strm);
	strm->state = (void *)w;

	return rc;
}

static int __inflateEnd(z_streamp strm, struct _internal_state *w)
{
	int rc;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	if (w == NULL)
		return Z_STREAM_ERROR;

	strm->state = w->priv_data;
	rc = w->impl ? h_inflateEnd(strm) :
		       z_inflateEnd(strm);

	strm->state = NULL;
	return rc;
}

int inflateEnd(z_streamp strm)
{
	int rc = Z_OK;
	struct _internal_state *w;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		zlib_stats.inflateEnd++;
		__inflate_update_totals(strm);
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	rc = __inflateEnd(strm, w);

	if (w->dictionary) {
		free(w->dictionary);
		w->dictionary = NULL;
	}

	pr_trace("[%p] inflateEnd w=%p rc=%d\n", strm, w, rc);
	free(w);

	return rc;
}

int inflate(z_streamp strm, int flush)
{
	int rc = Z_OK;
	struct _internal_state *w;
	unsigned int avail_in_slot, avail_out_slot;
	uint8_t dictionary[ZLIB_MAXDICTLEN];
	unsigned int dictLength = 0;

	if (strm == NULL)
		return Z_STREAM_ERROR;

	w = (struct _internal_state *)strm->state;
	if (w == NULL)
		return Z_STREAM_ERROR;

	/*
	 * Special situation triggered by strange JAVA zlib use-case:
	 * If we do not have any data to decompress, return
	 * Z_BUF_ERROR instead of trying to decypher 0 bytes. The
	 * decision to use HW or SW is deferred until we see avail_in
	 * != 0 for the first time.
	 */
	if ((strm->total_in == 0) && (w->allow_switching)) {
		/* Special case where there is no data. This occurs
		   quite often in the JAVA use-case. */
		if (strm->avail_in == 0)
			return Z_BUF_ERROR;

		if ((strm->avail_in < zlib_inflate_threshold) &&
		    (w->impl == ZLIB_HW_IMPL)) {
			pr_trace("[%p] inflate: avail_in=%d smaller "
				 "%d switching to software mode!\n",
				 strm, strm->avail_in, zlib_inflate_threshold);

			rc = inflateGetDictionary(strm, dictionary,
						  &dictLength);
			if (rc != Z_OK)
				goto err;

			/* Free already allocated resources, but not w */
			rc = __inflateEnd(strm, w);
			if (rc != Z_OK)
				goto err;

			/* Enforce software here! */
			w->impl = ZLIB_SW_IMPL;

			/* Reinit but not w */
			rc = __inflateInit2_(strm, w);
			if (rc != Z_OK)
				goto err;

			strm->state = (void *)w;
			if (w->gzhead != NULL)
				inflateGetHeader(strm, w->gzhead);

			if (dictLength != 0) {
				rc = inflateSetDictionary(strm, dictionary,
							  dictLength);
				if (rc != Z_OK) {
					inflateEnd(strm);
					goto err;
				}
			}
		} else if ((strm->avail_in >= zlib_inflate_threshold) &&
			   (w->impl == ZLIB_SW_IMPL) &&
			   (zlib_inflate_impl == ZLIB_HW_IMPL)) {
			pr_trace("[%p] inflate: avail_in=%d bigger or equal "
				 "%d switching to hardware mode!\n",
				 strm, strm->avail_in, zlib_inflate_threshold);

			rc = inflateGetDictionary(strm, dictionary,
						  &dictLength);
			if (rc != Z_OK)
				goto err;

			/* Free already allocated resources, but not w */
			rc = __inflateEnd(strm, w);
			if (rc != Z_OK)
				goto err;

			/* Try hardware mode here! */
			w->impl = zlib_inflate_impl;

			/* Reinit but not w */
			rc = __inflateInit2_(strm, w);
			if (rc != Z_OK)
				goto err;

			strm->state = (void *)w;
			if (w->gzhead != NULL)
				inflateGetHeader(strm, w->gzhead);

			if (dictLength != 0) {
				rc = inflateSetDictionary(strm, dictionary,
							  dictLength);
				if (rc != Z_OK) {
					inflateEnd(strm);
					goto err;
				}
			}
		}
	}

	if (zlib_gather_statistics()) {
		pthread_mutex_lock(&zlib_stats_mutex);
		avail_in_slot = strm->avail_in / 4096;
		if (avail_in_slot >= ZLIB_SIZE_SLOTS)
			avail_in_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.inflate_avail_in[avail_in_slot]++;

		avail_out_slot = strm->avail_out / 4096;
		if (avail_out_slot >= ZLIB_SIZE_SLOTS)
			avail_out_slot = ZLIB_SIZE_SLOTS - 1;
		zlib_stats.inflate_avail_out[avail_out_slot]++;
		zlib_stats.inflate[w->impl]++;
		pthread_mutex_unlock(&zlib_stats_mutex);
	}

	pr_trace("[%p] inflate:   flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_in=%ld total_out=%ld "
		 "crc/adler=%08lx\n",
		 strm, flush_to_str(flush), strm->next_in,
		 strm->avail_in, strm->next_out, strm->avail_out,
		 strm->total_in, strm->total_out, strm->adler);

	strm->state = w->priv_data;
	rc = w->impl ? h_inflate(strm, flush) :
		       z_inflate(strm, flush);

	/* stop switching after lowlevel inflate has been called */
	w->allow_switching = false;
	strm->state = (void *)w;

	pr_trace("[%p]            flush=%s next_in=%p avail_in=%d "
		 "next_out=%p avail_out=%d total_in=%ld total_out=%ld "
		 "crc/adler=%08lx rc=%s\n",
		 strm, flush_to_str(flush), strm->next_in,
		 strm->avail_in, strm->next_out, strm->avail_out,
		 strm->total_in, strm->total_out, strm->adler, ret_to_str(rc));

 err:
	return rc;
}

/* Implement the *Back() functions by using software only */

int inflateBack(z_streamp strm, in_func in, void *in_desc,
		out_func out, void *out_desc)
{
	return z_inflateBack(strm, in, in_desc, out, out_desc);
}

/**
 * FIXME Implement fallback if hw is not available.
 */
int inflateBackInit_(z_streamp strm, int windowBits, unsigned char *window,
		     const char *version, int stream_size)
{
	return z_inflateBackInit_(strm, windowBits, window, version,
				  stream_size);
}

int inflateBackEnd(z_streamp strm)
{
	return z_inflateBackEnd(strm);
}

const char *zlibVersion(void)
{
	return z_zlibVersion();
}

uLong zlibCompileFlags(void)
{
	return z_zlibCompileFlags();
}

uLong compressBound(uLong sourceLen)
{
	zlib_stats_inc(&zlib_stats.compressBound);
	return MAX(h_deflateBound(NULL, sourceLen),
			   z_deflateBound(NULL, sourceLen));
}

/*
 * adler32: Returns the value of the result of the z_ prefixed adler32 function
 *
 */
uLong adler32(uLong adler, const Bytef *buf, uInt len)
{
	zlib_stats_inc(&zlib_stats.adler32);
	pr_trace("adler32(len=%lld)\n", (long long)len);

	return z_adler32(adler, buf, len);
}

/*
 * adler32_combine: Returns the value of the result of the z_ prefixed
 * adler32_combine function
 *
 */
uLong adler32_combine(uLong adler1, uLong adler2, z_off_t len2)
{
	zlib_stats_inc(&zlib_stats.adler32_combine);
	pr_trace("adler32_combine(len2=%lld)\n", (long long)len2);

	return z_adler32_combine(adler1, adler2, len2);
}

/*
 * crc32: Returns the value of the result of the z_ prefixed crc32 function
 *
 */
uLong crc32(uLong crc, const Bytef *buf, uInt len)
{
	zlib_stats_inc(&zlib_stats.crc32);
	pr_trace("crc32(len=%lld)\n", (long long)len);

	return z_crc32(crc, buf, len);
}

/*
 * crc32_combine: Returns the value of the result of the z_ prefixed
 * crc32_combine function
 *
 */
uLong crc32_combine(uLong crc1, uLong crc2, z_off_t len2)
{
	zlib_stats_inc(&zlib_stats.crc32_combine);
	pr_trace("crc32_combine(len2=%lld)\n", (long long)len2);

	return z_crc32_combine(crc1, crc2, len2);
}

const char *zError(int err)
{
	return z_zError(err);
}

static void _done(void) __attribute__((destructor));

static void _done(void)
{
	if (zlib_gather_statistics()) {
		__print_stats();
		pthread_mutex_destroy(&zlib_stats_mutex);
	}

	zedc_hw_done();
	zedc_sw_done();

	if (zlib_log != stderr)
		fclose(zlib_log);

	return;
}
