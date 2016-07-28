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

#define _LARGEFILE64_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <dlfcn.h>
#include <wrapper.h>

/* older zlibs might not have this */
#ifndef z_off64_t
#  define z_off64_t off64_t
#endif

#if defined(CONFIG_DLOPEN_MECHANISM)

typedef void * __attribute__ ((__may_alias__)) pvoid_t;

#define register_sym(name)						\
	do {								\
		dlerror();    /* Clear any existing error */		\
		/* sw_trace("loading [%s]\n", #name); */		\
		*(pvoid_t *)(&p_##name) = dlsym(handle, #name);		\
		if ((error = dlerror()) != NULL) {			\
			sw_trace("%s\n", error);			\
			/* exit(EXIT_FAILURE); */			\
		}							\
	} while (0)

#define check_sym(name, rc)						\
	do {								\
		if ((name) == NULL) {					\
			pr_err("%s not loadable, consider using a "	\
			       "newer libz version.\n", #name);		\
			return (rc);					\
		}							\
	} while (0)

static void *handle = NULL;

int (* p_deflateInit2_)(z_streamp strm, int level, int method,
			int windowBits, int memLevel, int strategy,
			const char *version, int stream_size);
int z_deflateInit2_(z_streamp strm, int level, int method,
		    int windowBits, int memLevel, int strategy,
		    const char *version, int stream_size)
{
	int rc;

	check_sym(p_deflateInit2_, Z_STREAM_ERROR);
	rc = (* p_deflateInit2_)(strm, level, method, windowBits, memLevel,
				 strategy, version, stream_size);
	return rc;
}

static int (* p_deflateParams)(z_streamp strm, int level, int strategy);
int z_deflateParams(z_streamp strm, int level, int strategy)
{
	check_sym(p_deflateParams, Z_STREAM_ERROR);
	return (* p_deflateParams)(strm, level, strategy);
}

static uLong (* p_deflateBound)(z_streamp strm, uLong sourceLen);
uLong z_deflateBound(z_streamp strm, uLong sourceLen)
{
	check_sym(p_deflateBound, Z_STREAM_ERROR);
	return (* p_deflateBound)(strm, sourceLen);
}

static int (* p_deflateReset)(z_streamp strm);
int z_deflateReset(z_streamp strm)
{
	check_sym(p_deflateReset, Z_STREAM_ERROR);
	return (* p_deflateReset)(strm);
}

static int (* p_deflateSetDictionary)(z_streamp strm, const Bytef *dictionary,
				      uInt  dictLength);
int z_deflateSetDictionary(z_streamp strm, const Bytef *dictionary,
			   uInt  dictLength)
{
	check_sym(p_deflateSetDictionary, Z_STREAM_ERROR);
	return (* p_deflateSetDictionary)(strm, dictionary, dictLength);
}

static int (* p_deflateSetHeader)(z_streamp strm, gz_headerp head);
int z_deflateSetHeader(z_streamp strm, gz_headerp head)
{
	check_sym(p_deflateSetHeader, Z_STREAM_ERROR);
	return p_deflateSetHeader(strm, head);
}


static int (* p_deflatePrime)(z_streamp strm, int bits, int value);
int z_deflatePrime(z_streamp strm, int bits, int value)
{
	check_sym(p_deflatePrime, Z_STREAM_ERROR);
	return (* p_deflatePrime)(strm, bits, value);
}

static int (* p_deflateCopy)(z_streamp dest, z_streamp source);
int z_deflateCopy(z_streamp dest, z_streamp source)
{
	check_sym(p_deflateCopy, Z_STREAM_ERROR);
	return (* p_deflateCopy)(dest, source);
}

static int (* p_deflate)(z_streamp strm, int flush);
int z_deflate(z_streamp strm, int flush)
{
	check_sym(p_deflate, Z_STREAM_ERROR);
	return (* p_deflate)(strm, flush);
}

static int (* p_deflateEnd)(z_streamp strm);
int z_deflateEnd(z_streamp strm)
{
	check_sym(p_deflateEnd, Z_STREAM_ERROR);
	return (* p_deflateEnd)(strm);
}

static int (* p_inflateInit2_)(z_streamp strm, int  windowBits,
			       const char *version, int stream_size);
int z_inflateInit2_(z_streamp strm, int  windowBits, const char *version,
		    int stream_size)
{
	int rc;

	check_sym(p_inflateInit2_, Z_STREAM_ERROR);
	rc = (* p_inflateInit2_)(strm, windowBits, version, stream_size);
	return rc;
}

static int (* p_inflateReset)(z_streamp strm);
int z_inflateReset(z_streamp strm)
{
	check_sym(p_inflateReset, Z_STREAM_ERROR);
	return (* p_inflateReset)(strm);
}

static int (* p_inflateReset2)(z_streamp strm, int windowBits);
int z_inflateReset2(z_streamp strm, int windowBits)
{
	check_sym(p_inflateReset2, Z_STREAM_ERROR);
	return (* p_inflateReset2)(strm, windowBits);
}

static int (* p_inflateSetDictionary)(z_streamp strm, const Bytef *dictionary,
				 uInt  dictLength);
int z_inflateSetDictionary(z_streamp strm, const Bytef *dictionary,
			   uInt  dictLength)
{
	check_sym(p_inflateSetDictionary, Z_STREAM_ERROR);
	return (* p_inflateSetDictionary)(strm, dictionary, dictLength);
}

/**
 * No warning in this case since we try to emulate this in the
 * functions above.
 */
static int (* p_inflateGetDictionary)(z_streamp strm, const Bytef *dictionary,
				      uInt *dictLength);
int z_inflateGetDictionary(z_streamp strm, const Bytef *dictionary,
			   uInt *dictLength)
{
	if (p_inflateGetDictionary == NULL)
		return Z_STREAM_ERROR;
	return (* p_inflateGetDictionary)(strm, dictionary, dictLength);
}

bool z_hasGetDictionary(void)
{
	return (p_inflateGetDictionary != NULL);
}

static int (* p_inflateGetHeader)(z_streamp strm, gz_headerp head);
int z_inflateGetHeader(z_streamp strm, gz_headerp head)
{
	check_sym(p_inflateGetHeader, Z_STREAM_ERROR);
	return (* p_inflateGetHeader)(strm, head);
}

static int (* p_inflatePrime)(z_streamp strm, int bits, int value);
int z_inflatePrime(z_streamp strm, int bits, int value)
{
	check_sym(p_inflatePrime, Z_STREAM_ERROR);
	return (* p_inflatePrime)(strm, bits, value);
}

static int (* p_inflateSync)(z_streamp strm);
int z_inflateSync(z_streamp strm)
{
	check_sym(p_inflateSync, Z_STREAM_ERROR);
	return (* p_inflateSync)(strm);
}

static int (* p_inflate)(z_streamp strm, int flush);
int z_inflate(z_streamp strm, int flush)
{
	check_sym(p_inflate, Z_STREAM_ERROR);
	return (* p_inflate)(strm, flush);
}

static int (* p_inflateEnd)(z_streamp strm);
int z_inflateEnd(z_streamp strm)
{
	check_sym(p_inflateEnd, Z_STREAM_ERROR);
	return (* p_inflateEnd)(strm);
}

static int (* p_inflateBackInit_)(z_streamp strm, int windowBits,
				  unsigned char *window, const char *version,
				  int stream_size);
int z_inflateBackInit_(z_streamp strm, int windowBits,
		       unsigned char *window, const char *version,
		       int stream_size)
{
	check_sym(p_inflateBackInit_, Z_STREAM_ERROR);
	return (* p_inflateBackInit_)(strm, windowBits, window, version,
				      stream_size);
}

static int (* p_inflateBack)(z_streamp strm, in_func in, void *in_desc,
			     out_func out, void *out_desc);
int z_inflateBack(z_streamp strm, in_func in, void *in_desc,
		      out_func out, void *out_desc)
{
	check_sym(p_inflateBack, Z_STREAM_ERROR);
	return (* p_inflateBack)(strm, in, in_desc, out, out_desc);
}

static int (* p_inflateBackEnd)(z_streamp strm);
int z_inflateBackEnd(z_streamp strm)
{
	check_sym(p_inflateBackEnd, Z_STREAM_ERROR);
	return (* p_inflateBackEnd)(strm);
}

static uLong (* p_adler32)(uLong adler, const Bytef *buf, uInt len);
uLong z_adler32(uLong adler, const Bytef *buf, uInt len)
{
	check_sym(p_adler32, Z_STREAM_ERROR);
	return (* p_adler32)(adler, buf, len);
}

static uLong (* p_adler32_combine)(uLong adler1, uLong adler2, z_off_t len2);
uLong z_adler32_combine(uLong adler1, uLong adler2, z_off_t len2)
{
	check_sym(p_adler32_combine, Z_STREAM_ERROR);
	return (* p_adler32_combine)(adler1, adler2, len2);
}

static uLong (* p_crc32)(uLong crc, const Bytef *buf, uInt len);
uLong z_crc32(uLong crc, const Bytef *buf, uInt len)
{
	check_sym(p_crc32, Z_STREAM_ERROR);
	return (* p_crc32)(crc, buf, len);
}

static uLong (* p_crc32_combine)(uLong crc1, uLong crc2, z_off_t len2);
uLong z_crc32_combine(uLong crc1, uLong crc2, z_off_t len2)
{
	check_sym(p_crc32_combine, Z_STREAM_ERROR);
	return (* p_crc32_combine)(crc1, crc2, len2);
}

static const char *(* p_zError)(int err);
const char *z_zError(int err)
{
	check_sym(p_zError, NULL);
	return (* p_zError)(err);
}

static uLong (* p_zlibCompileFlags)(void);
uLong z_zlibCompileFlags(void)
{
	return p_zlibCompileFlags();
}

static const char * (* p_zlibVersion)(void);
const char *z_zlibVersion(void)
{
	check_sym(p_zlibVersion, NULL);
	return (* p_zlibVersion)();
}

static gzFile (* p_gzopen)(const char *path, const char *mode);
gzFile gzopen(const char *path, const char *mode)
{
	zlib_stats_inc(&zlib_stats.gzopen);
	check_sym(p_gzopen, NULL);
	return (* p_gzopen)(path, mode);
}

static gzFile (* p_gzdopen)(int fd, const char *mode);
gzFile gzdopen(int fd, const char *mode)
{
	zlib_stats_inc(&zlib_stats.gzdopen);
	check_sym(p_gzdopen, NULL);
	return (* p_gzdopen)(fd, mode);
}

static int (* p_gzwrite)(gzFile file, voidpc buf, unsigned len);
int gzwrite(gzFile file, voidpc buf, unsigned len)

{
	zlib_stats_inc(&zlib_stats.gzwrite);
	check_sym(p_gzwrite, -1);
	return (* p_gzwrite)(file, buf, len);
}

static int (* p_gzread)(gzFile file, voidp buf, unsigned len);
int gzread(gzFile file, voidp buf, unsigned len)
{
	zlib_stats_inc(&zlib_stats.gzread);
	check_sym(p_gzread, -1);
	return (* p_gzread)(file, buf, len);
}

static int (* p_gzclose)(gzFile file);
int gzclose(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzclose);
	check_sym(p_gzread, Z_STREAM_ERROR);
	return (* p_gzclose)(file);
}

static int (* p_gzungetc)(int c, gzFile file);
int gzungetc(int c, gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzungetc);
	check_sym(p_gzungetc, -1);
	return (* p_gzungetc)(c, file);
}

static int (* p_gzflush)(gzFile file, int flush);
int gzflush(gzFile file, int flush)
{
	zlib_stats_inc(&zlib_stats.gzflush);
	check_sym(p_gzflush, Z_STREAM_ERROR);
	return (* p_gzflush)(file, flush);
}

static int (* p_gzeof)(gzFile file);
int gzeof(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzeof);
	check_sym(p_gzeof, 0);
	return (* p_gzeof)(file);
}

static z_off_t (* p_gztell)(gzFile file);
z_off_t gztell(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gztell);
	check_sym(p_gztell, -1ll);
	return (* p_gztell)(file);
}

static const char * (* p_gzerror)(gzFile file, int *errnum);
const char *gzerror(gzFile file, int *errnum)
{
	zlib_stats_inc(&zlib_stats.gzerror);
	check_sym(p_gzerror, NULL);
	return (* p_gzerror)(file, errnum);
}

static z_off_t (* p_gzseek)(gzFile file, z_off_t offset, int whence);
z_off_t gzseek(gzFile file, z_off_t offset, int whence)
{
	zlib_stats_inc(&zlib_stats.gzseek);
	check_sym(p_gzseek, -1ll);
	return (* p_gzseek)(file, offset, whence);
}

static int (* p_gzrewind)(gzFile file);
int gzrewind(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzrewind);
	check_sym(p_gzrewind, -1);
	return (* p_gzrewind)(file);
}

static char * (* p_gzgets)(gzFile file, char *buf, int len);
char * gzgets(gzFile file, char *buf, int len)
{
	zlib_stats_inc(&zlib_stats.gzgets);
	check_sym(p_gzgets, NULL);
	return (* p_gzgets)(file, buf, len);
}

static int (* p_gzputc)(gzFile file, int c);
int gzputc(gzFile file, int c)
{
	zlib_stats_inc(&zlib_stats.gzputc);
	check_sym(p_gzputc, -1);
	return (* p_gzputc)(file, c);
}

/*FIXME gzgetc is potentially a macro ... */
static int (* p_gzgetc)(gzFile file);
#undef gzgetc
int gzgetc(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzgetc);
	check_sym(p_gzgetc, -1);
	return (* p_gzgetc)(file);
}

static int (* p_gzputs)(gzFile file, const char *s);
int gzputs(gzFile file, const char *s)
{
	zlib_stats_inc(&zlib_stats.gzputs);
	check_sym(p_gzputs, -1);
	return (* p_gzputs)(file, s);
}

static int (* p_gzprintf)(gzFile file, const char *format, ...);
int gzprintf(gzFile file, const char *format, ...)
{
	int count;
	va_list ap;

	zlib_stats_inc(&zlib_stats.gzprintf);
	check_sym(p_gzprintf, -1);

	va_start(ap, format);
	count = (* p_gzprintf)(file, format, ap);
	va_end(ap);

	return count;
}

static int (* p_compress)(Bytef *dest, uLongf *destLen,
			  const Bytef *source, uLong sourceLen);
int compress(Bytef *dest, uLongf *destLen, const Bytef *source,
	     uLong sourceLen)
{
	zlib_stats_inc(&zlib_stats.compress);
	check_sym(p_compress, Z_STREAM_ERROR);
	return (* p_compress)(dest, destLen, source, sourceLen);
}

static int (* p_compress2)(Bytef *dest, uLongf *destLen,
			   const Bytef *source, uLong sourceLen, int level);
int compress2(Bytef *dest, uLongf *destLen, const Bytef *source,
	      uLong sourceLen, int level)
{
	zlib_stats_inc(&zlib_stats.compress2);
	check_sym(p_compress2, Z_STREAM_ERROR);
	return (* p_compress2)(dest, destLen, source, sourceLen, level);
}

static uLong (* p_compressBound)(uLong sourceLen);
uLong compressBound(uLong sourceLen)
{
	zlib_stats_inc(&zlib_stats.compressBound);
	check_sym(p_compressBound, Z_STREAM_ERROR);
	return (* p_compressBound)(sourceLen);
}

static int (* p_uncompress)(Bytef *dest, uLongf *destLen,
			    const Bytef *source, uLong sourceLen);
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source,
	       uLong sourceLen)
{
	zlib_stats_inc(&zlib_stats.uncompress);
	check_sym(p_uncompress, Z_STREAM_ERROR);
	return (* p_uncompress)(dest, destLen, source, sourceLen);
}

#if ZLIB_VERNUM >= 0x1270
static int (* p_gzbuffer)(gzFile file, unsigned size);
int gzbuffer(gzFile file, unsigned size)
{
	zlib_stats_inc(&zlib_stats.gzbuffer);
	check_sym(p_gzbuffer, -1);
	return (* p_gzbuffer)(file, size);
}

static uLong (* p_adler32_combine64)(uLong adler1, uLong adler2,
				     z_off64_t len2);
uLong adler32_combine64(uLong adler1, uLong adler2, z_off64_t len2)
{
	zlib_stats_inc(&zlib_stats.adler32_combine64);
	check_sym(p_adler32_combine64, Z_STREAM_ERROR);
	return (* p_adler32_combine64)(adler1, adler2, len2);
}

static uLong (* p_crc32_combine64)(uLong crc1, uLong crc2, z_off64_t len2);
uLong crc32_combine64(uLong crc1, uLong crc2, z_off64_t len2)
{
	zlib_stats_inc(&zlib_stats.crc32_combine64);
	check_sym(p_crc32_combine64, Z_STREAM_ERROR);
	return (* p_crc32_combine64)(crc1, crc2, len2);
}

static gzFile (* p_gzopen64)(const char *path, const char *mode);
gzFile gzopen64(const char *path, const char *mode)
{
	zlib_stats_inc(&zlib_stats.gzopen64);
	check_sym(p_gzopen64, NULL);
	return (* p_gzopen64)(path, mode);
}

static z_off64_t (* p_gztell64)(gzFile file);
z_off64_t gztell64(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gztell64);
	check_sym(p_gztell64, -1ll);
	return (* p_gztell64)(file);
}

static z_off_t (* p_gzseek64)(gzFile file, z_off64_t offset, int whence);
z_off_t gzseek64(gzFile file, z_off64_t offset, int whence)
{
	zlib_stats_inc(&zlib_stats.gzseek64);
	check_sym(p_gzseek64, -1ll);
	return (* p_gzseek64)(file, offset, whence);
}

static z_off_t (* p_gzoffset)(gzFile file);
z_off_t gzoffset(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzoffset);
	check_sym(p_gzoffset, -1ll);
	return (* p_gzoffset)(file);
}

static z_off64_t (* p_gzoffset64)(gzFile file);
z_off_t gzoffset64(gzFile file)
{
	zlib_stats_inc(&zlib_stats.gzoffset64);
	check_sym(p_gzoffset64, -1ll);
	return (* p_gzoffset64)(file);
}

static const z_crc_t *(* p_get_crc_table)(void);
const z_crc_t *get_crc_table()
{
	zlib_stats_inc(&zlib_stats.get_crc_table);
	check_sym(p_get_crc_table, NULL);
	return (* p_get_crc_table)();
}

#endif

/**
 * NOTE: We had different variants trying to find the right libz.so.1
 * in our system. Unfortunately this can differ for the various
 * distributions:
 *
 * RHEL7.2:
 *   $ ldconfig -p | grep libz.so.1 | cut -d' ' -f4 | head -n1
 *   /lib64/libz.so.1
 *
 * Ubuntu 15.10:
 *   $ ldconfig -p | grep libz.so.1 | cut -d' ' -f4 | head -n1
 *   /lib/powerpc64le-linux-gnu/libz.so.1
 *
 * Intel with RHEL6.7:
 *   $ ldconfig -p | grep libz.so.1 | cut -d' ' -f4 | head -n1
 *   /lib64/libz.so.1
 *
 * We are setting this via config.mk option and allow the
 * distributions to overwrite this via rpm spec file.
 *
 * NOTE: We tried loading "libz.so.1" without full path, but that
 * turned out to be dangerous. We tried to load our own lib, which was
 * leading to endless recursive calls and segfaults as results.
 */
void zedc_sw_init(void)
{
	char *error;
	const char *zlib_path = getenv("ZLIB_PATH");

	/* User has setup environment variable to find libz.so.1 */
	if (zlib_path != NULL) {
		sw_trace("Loading software zlib \"%s\"\n", zlib_path);
		dlerror();
		handle = dlopen(zlib_path, RTLD_LAZY);
		if (handle != NULL)
			goto load_syms;
	}

	/* We saw dlopen returning non NULL value in case of passing ""! */
	if (strcmp(CONFIG_ZLIB_PATH, "") == 0) {
		pr_err("  Empty CONFIG_ZLIB_PATH \"%s\"\n",
		       CONFIG_ZLIB_PATH);
		return;
	}

	/* Loading private zlib.so.1 using CONFIG_ZLIB_PATH */
	sw_trace("Loading software zlib \"%s\"\n", CONFIG_ZLIB_PATH);
	dlerror();
	handle = dlopen(CONFIG_ZLIB_PATH, RTLD_LAZY);
	if (handle == NULL) {
		pr_err("  %s\n", dlerror());
		return;
	}

load_syms:
	register_sym(zlibVersion);

	sw_trace("  ZLIB_VERSION=%s (header) zlibVersion()=%s (code)\n",
		 ZLIB_VERSION, z_zlibVersion());

	if (strcmp(ZLIB_VERSION, z_zlibVersion()) != 0) {
		pr_err("libz.so.1=%s and zlib.h=%s do not match!\n",
		       z_zlibVersion(), ZLIB_VERSION);
		return;
	}

	register_sym(deflateInit2_);
	register_sym(deflateParams);
	register_sym(deflateBound);
	register_sym(deflateReset);
	register_sym(deflatePrime);
	register_sym(deflateCopy);
	register_sym(deflate);
	register_sym(deflateSetDictionary);
	register_sym(deflateSetHeader);
	register_sym(deflateEnd);

	register_sym(inflateInit2_);
	register_sym(inflateSync);
	register_sym(inflatePrime);
	register_sym(inflate);
	register_sym(inflateReset);
	register_sym(inflateReset2);
	register_sym(inflateSetDictionary);
	register_sym(inflateGetDictionary);
	register_sym(inflateGetHeader);
	register_sym(inflateEnd);

	register_sym(inflateBackInit_);
	register_sym(inflateBack);
	register_sym(inflateBackEnd);

	register_sym(gzopen);
	register_sym(gzdopen);
	register_sym(gzwrite);
	register_sym(gzread);
	register_sym(gzclose);
	register_sym(gzflush);
	register_sym(gzungetc);
	register_sym(gzeof);
	register_sym(gztell);
	register_sym(gzerror);
	register_sym(gzseek);
	register_sym(gzrewind);
	register_sym(gzputs);
	register_sym(gzputc);
	register_sym(gzgetc);
	register_sym(gzputs);
	register_sym(gzprintf);

	register_sym(compress);
	register_sym(compress2);
	register_sym(compressBound);
	register_sym(uncompress);

	register_sym(zError);
	register_sym(zlibCompileFlags);

	register_sym(adler32);
	register_sym(adler32_combine);
	register_sym(crc32);
	register_sym(crc32_combine);


#if ZLIB_VERNUM >= 0x1270
	register_sym(gzbuffer);
	register_sym(gzopen64);
	register_sym(gzseek64);
	register_sym(gztell64);
	register_sym(gzoffset);
	register_sym(gzoffset64);
	register_sym(adler32_combine64);
	register_sym(crc32_combine64);
	register_sym(get_crc_table);
#endif
}

void zedc_sw_done(void)
{
	if (handle != NULL) {
		sw_trace("Closing software zlib\n");
		dlclose(handle);
	}
}

#else

/*
 * Prefixing symbols has nasty side effects. One of them is that libc
 * symbols get prefixed too. Such that we see z_free and not free
 * anymore. Let us fix this up here to see if it works in general.
 */
void *z_malloc(size_t size);
void *z_malloc(size_t size)
{
	return malloc(size);
}

void z_free(void *ptr);
void z_free(void *ptr)
{
	free(ptr);
}

void *z_memcpy(void *dest, const void *src, size_t n);
void *z_memcpy(void *dest, const void *src, size_t n)
{
	return memcpy(dest, src, n);
}

size_t z_strlen(const char *s);
size_t z_strlen(const char *s)
{
	return strlen(s);
}

void *z_memset(void *s, int c, size_t n);
void *z_memset(void *s, int c, size_t n)
{
	return memset(s, c, n);
}

int z_close(int fd);
int z_close(int fd)
{
	return close(fd);
}

int z_open(const char *pathname, int flags, mode_t mode);
int z_open(const char *pathname, int flags, mode_t mode)
{
	return open(pathname, flags, mode);
}

ssize_t z_read(int fd, void *buf, size_t count);
ssize_t z_read(int fd, void *buf, size_t count)
{
	return read(fd, buf, count);
}

ssize_t z_write(int fd, const void *buf, size_t count);
ssize_t z_write(int fd, const void *buf, size_t count)
{
	return write(fd, buf, count);
}

long long z_lseek64(int fd, long long offset, int whence);
long long z_lseek64(int fd, long long offset, int whence)
{
	return lseek64(fd, offset, whence);
}

int z_snprintf(char *str, size_t size, const char *format, ...);
int z_snprintf(char *str, size_t size, const char *format, ...)
{
	int rc;
	va_list ap;

	va_start(ap, format);
	rc = snprintf(str, size, format, ap);
	va_end(ap);
	return rc;
}

int z_vsnprintf(char *str, size_t size, const char *format, va_list ap);
int z_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	int rc;

	rc = vsnprintf(str, size, format, ap);
	return rc;
}

extern int *z___errno_location (void);
extern int *z___errno_location (void)
{
	return __errno_location();
}


void *z_memchr(const void *s, int c, size_t n);
void *z_memchr(const void *s, int c, size_t n)
{
	return memchr(s, c, n);
}

char *z_strerror(int errnum);
char *z_strerror(int errnum)
{
	return strerror(errnum);
}

void zedc_sw_init(void)
{
	sw_trace("Using z_ prefixed libz.a\n");
	sw_trace("  ZLIB_VERSION %s (header version)\n", ZLIB_VERSION);
	sw_trace("  zlibVersion  %s (libz.so version)\n", z_zlibVersion());

	if (strcmp(ZLIB_VERSION, z_zlibVersion()) != 0) {
		pr_err("libz.so %s and zlib.h %s do not match!\n",
		       z_zlibVersion(), ZLIB_VERSION);
		return;
	}
}

void zedc_sw_done(void)
{
	sw_trace("Closing software zlib\n");
}

#endif	/* CONFIG_DLOPEN_MECHANSIM */

