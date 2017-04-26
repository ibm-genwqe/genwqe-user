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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <malloc.h>
#include <time.h>
#include <signal.h>
#include <zlib.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/time.h>
#include <asm/byteorder.h>
#include <pthread.h>

#include <sched.h>

#include "libddcb.h"
#include "genwqe_tools.h"
#include "force_cpu.h"
#include "memcopy_ddcb.h"

/* Error injection bitmask */
#define ERR_INJ_NONE   0x0
#define ERR_INJ_INPUT  0x1
#define ERR_INJ_OUTPUT 0x2
#define ERR_INJ_SIZE   0x4
#define ERR_INJ_DDCB   0x8

static const char *version = GIT_VERSION;

int verbose_flag = 0;

#define VERBOSE0(...) do {					\
		fprintf(stderr, __VA_ARGS__);			\
	} while (0)

#define VERBOSE1(...) do {					\
		if (verbose_flag > 0)				\
			fprintf(stderr, __VA_ARGS__);		\
	} while (0)

#define VERBOSE2(...) do {					\
		if (verbose_flag > 1)				\
			fprintf(stderr, __VA_ARGS__);		\
	} while (0)

#define VERBOSE3(...) do {					\
		if (verbose_flag > 3)				\
			fprintf(stderr, __VA_ARGS__);		\
	} while (0)

#define EVERBOSE(...) do {					\
		fprintf(stderr, __VA_ARGS__);			\
	} while (0)

struct memcpy_in_parms {
	int card_no;		/* Card 0 default, changed with -C option */
	int card_type;		/* card type 0 def, changed with -A option */
	int mode;		/* Change with -n option */
	bool quiet;		/* quiet=false default, changed with -q opt */
	int cpu;		/* -1 default, changed with - -C option */
	int count;		/* 1 default, change with -c option */
	bool force_cmp;		/* default false, Change with -F option */
	int use_sglist;		/* 0 default, change with -g option */
	int preload;		/* 1 default, chane with -l option */
	int threads;		/* 1 default, change with -t option */
	FILE *o_fp;		/* Output File pointer */
	FILE *fpattern;		/* pattern input file pointer */
	uint64_t in_ats_type;	/* ATS_TYPE_FLAT_RDWR or ATS_TYPE_SGL_RDWR */
	unsigned int page_size;
	int data_buf_size;	/* 4k default, changed with -s option */
	unsigned int pgoffs_i;	/* offset in the 4k Aligned input buffer */
	unsigned int pgoffs_o;	/* offset in the 4k Aligned output buffer */
	uint32_t mcpy_crc32;	/* my value to compare */
	uint32_t mcpy_adler32;	/* my value to compare */
	int have_threads;
	struct timespec stime;	/* Start time */
	struct timespec etime;	/* End time */
	unsigned int err_inj;	/* error injection while running DDCBs */
};

struct memcpy_thread_data {
	int thread;
	pthread_t tid;
	accel_t accel;
	uint8_t	*ibuf4k;	/* 4 K aligned buffer */
	uint8_t *ibuf;		/* the 4k aligned buffer + pgoffs_i */
	struct memcpy_in_parms *ip;
	uint64_t out_ats_type;	/* ATS_TYPE_FLAT_RDWR or
					   ATS_TYPE_SGL_RDWR */
	int err;		/* Return code from Thread */
	int errors;		/* Return data */
	int memcopies;
	long long bytes_copied;	/* Return data */
	uint64_t total_usec;	/* Return time in usec */
	struct timespec stime;	/* Thread Start time */
	struct timespec etime;	/* Thread End time */
};

static void *__memcpy_thread(void *data);

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s\n"
	       "  -h, --help               print usage information\n"
	       "  -v, --verbose            verbose mode\n"
	       "  -C, --card <cardno>      use this card for operation\n"
	       "  -A, --accelerator-type=GENWQE|CAPI CAPI is only available "
	       "for System p\n"
	       "  -V, --version\n"
	       "  -q, --quiet              quiece output\n"
	       "  -c, --count <number>     do multiple memcopies\n"
	       "  -l, --preload <number>   preload multiple ddcb's. "
	       "(default 1, only for CAPI Card)\n"
	       "  -X, --cpu <cpu>          only run on this CPU\n"
	       "  -D, --debug              create debug data on failure\n"
	       "  -G, --use-sglist         use the scatter gather list\n"
	       "  -n, --nonblocking        use nonblcoking behavior\n"
	       "  -p, --patternfile <filename>]\n"
	       "  -s, --bufsize <bufsize>  default is 4KiB\n"
	       "  -i, --pgoffs_i <offs>    byte offset for input buffer\n"
	       "  -o, --pgoffs_o <offs>    byte offset for output buffer\n"
	       "  -F, --force-compare <output_data.bin>\n"
	       "  -t, --threads <num>      run <num> threads, default is 1\n"
	       "  -Y, --inject-error <err> IN:0x1, OUT:0x2, SIZE:0x4, DDCB:0x8\n"
	       "\n"
	       "This utility sends memcopy DDCBs to the application\n"
	       "chip unit. It can be used to check the cards health and/or\n"
	       "to produce stress on the card to verify its correct\n"
	       "function.\n"
	       "\n"
	       "Example:\n"
	       "    dd if=/dev/urandom bs=4096 count=1024 of=input_data.bin\n"
	       "    %s -C0 -F -D --patternfile input_data.bin output_data.bin\n"
	       "    echo $?\n"
	       "    diff input_data.bin output_data.bin\n"
	       "    echo $?\n"
	       "\n", prog, prog);
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
	else {
		pr_err("--size or -s out of range, use KiB/MiB or GiB only\n");
		num = ULLONG_MAX;
		errno = ERANGE;
		exit(EXIT_FAILURE);
	}

	return num;
}

static void INT_handler(int sig);
static bool stop_memcopying = false;

static void INT_handler(int sig)
{
	signal(sig, SIG_IGN);
	stop_memcopying = true;
	/* signal(SIGINT, INT_handler); *//* Try again */
}

static void __hexdump(uint8_t *buff, unsigned int size, unsigned int offs)
{
	unsigned int i;
	const uint8_t *b = (uint8_t *)buff;

	for (i = 0; i < size; i++) {
		if ((i & 0x0f) == 0x00)
			EVERBOSE(" %08x: ", offs + i);
		EVERBOSE(" %02x", b[i]);
		if ((i & 0x0f) == 0x0f)
			EVERBOSE("\n");
	}
	EVERBOSE("\n");
}

static uint64_t tdiff_us(struct timespec *et, struct timespec *st)
{
	uint64_t	td;

	if (st->tv_nsec > et->tv_nsec) {
		td = (uint64_t) (1000000000 + et->tv_nsec);
		et->tv_sec--;
	} else td = (uint64_t)et->tv_nsec;
	td -= (uint64_t)st->tv_nsec;
	td = td / 1000;
	td += (uint64_t)(et->tv_sec - st->tv_sec) * 1000000;
	return td;
}

/* update tl if t is less than tl */
static void time_low(struct timespec *tl, struct timespec *t)
{
	if ((uint32_t)t->tv_sec < (uint32_t)tl->tv_sec) {
		tl->tv_sec = t->tv_sec;
		tl->tv_nsec = t->tv_nsec;
		return;
	}
	if ((uint32_t)t->tv_nsec < (uint32_t)tl->tv_nsec)
		tl->tv_nsec = t->tv_nsec;
	return;
}

/* update th if t is greater than th */
static void time_high(struct timespec *th, struct timespec *t)
{
	if ((uint32_t)t->tv_sec > (uint32_t)th->tv_sec) {
		th->tv_sec = t->tv_sec;
		th->tv_nsec = t->tv_nsec;
		return;
	}
	if ((uint32_t)t->tv_nsec > (uint32_t)th->tv_nsec)
		th->tv_nsec = t->tv_nsec;
	return;
}


/**
 * zEDC has a different cmd code for memcopy and support
 * CRC32/ADLER32.
 */
static inline int accel_is_zedc(accel_t card)
{
	return (accel_get_app_id(card) & DDCB_APPL_ID_MASK) ==
		GENWQE_APPL_ID_GZIP;
}

static int accel_memcpy(accel_t card, struct ddcb_cmd *cmd_list, int preload,
			void *dest, size_t dest_n, uint64_t out_ats_type,
			void *src,  size_t src_n,
			uint64_t in_ats_type,
			uint32_t *crc32,
			uint32_t *adler32,
			uint32_t *inp_processed,
			uint32_t *outp_returned,
			unsigned int err_inj)
{
	int rc, i;
	struct ddcb_cmd *cmd = cmd_list;
	struct asiv_memcpy *asiv;
	struct asv_memcpy *asv;

	for (i = 0; i < preload; i++) {
		ddcb_cmd_init(cmd);
		/* setup ASIV part */
		asiv = (struct asiv_memcpy *)&cmd->asiv;
		cmd->ddata_addr = 0ull;	/* FIXME */
		cmd->acfunc	= DDCB_ACFUNC_APP;	/* goto accelerator */
		cmd->cmd = ZCOMP_CMD_ZEDC_MEMCOPY;
		cmd->cmdopts	= 0x0000;	/* pass addresses not lists */
		cmd->asiv_length= 0x40 - 0x20;
		cmd->asv_length	= 0xC0 - 0x80;	/* try to absorb all */
		cmd->ats = 0x0;

		asiv->inp_buff      = __cpu_to_be64((unsigned long)src);
		asiv->inp_buff_len  = __cpu_to_be32((unsigned long)src_n);
		cmd->ats |= ATS_SET_FLAGS(struct asiv_memcpy, inp_buff,
					  in_ats_type);

		asiv->outp_buff     = __cpu_to_be64((unsigned long)dest);
		asiv->outp_buff_len = __cpu_to_be32((uint32_t)dest_n);
		cmd->ats |= ATS_SET_FLAGS(struct asiv_memcpy, outp_buff,
					  out_ats_type);

		/* Only relevant for the ZEDC variant. */
		asiv->in_adler32    = __cpu_to_be32(1);
		asiv->in_crc32      = __cpu_to_be32(0);

		/* This will surely crash the application ... */
		if (err_inj & ERR_INJ_INPUT) {
			asiv->inp_buff ^= 0xffffffffffffffffull;
			fprintf(stderr, "ERR_INJ_INPUT:  %016llx\n",
				(long long)asiv->inp_buff);
		}
		if (err_inj & ERR_INJ_OUTPUT) {
			asiv->outp_buff ^= 0xffffffffffffffffull;
			fprintf(stderr, "ERR_INJ_OUTPUT: %016llx\n",
				(long long)asiv->outp_buff);
		}
		if (err_inj & ERR_INJ_SIZE) {
			asiv->inp_buff_len ^= 0xfffffffffull;
			asiv->outp_buff_len ^= 0xffffffffull;
			fprintf(stderr, "ERR_INJ_SIZE:   %08lx/%08lx\n",
				(long)asiv->inp_buff_len,
				(long)asiv->outp_buff_len);
		}

		if (i < (preload -1))
			cmd->next_addr = (unsigned long)(cmd + 1);
		else
			cmd->next_addr = 0x0;

		cmd++;
	}

	rc = accel_ddcb_execute(card, cmd_list, NULL, NULL);

	cmd = &cmd_list[0];
	asv = (struct asv_memcpy *)&cmd->asv;
	*crc32	       = __be32_to_cpu(asv->out_crc32);
	*adler32       = __be32_to_cpu(asv->out_adler32);
	*inp_processed = __be32_to_cpu(asv->inp_processed);
	*outp_returned = __be32_to_cpu(asv->outp_returned);
	return rc;
}

static void ddcb_print_dma_err(struct _asv_runtime_dma_error *d)
{
	fprintf(stderr, " raddr: %016llx rfmt/chan/disc: %08x "
		"rdmae: %04x rsge: %04x\n"
		" waddr: %016llx wfmt/chan/disc: %08x "
		"wdmae: %04x wsge: %04x\n",
		(long long)__be64_to_cpu(d->raddr_be64),
		__be32_to_cpu(d->rfmt_chan_disccnt_be32),
		__be16_to_cpu(d->rdmae_be16),
		__be16_to_cpu(d->rsge_be16),
		(long long)__be64_to_cpu(d->waddr_be64),
		__be32_to_cpu(d->wfmt_chan_disccnt_be32),
		__be16_to_cpu(d->wdmae_be16),
		__be16_to_cpu(d->wsge_be16));
}

static void *__memcpy_thread(void *data)
{
	struct	memcpy_thread_data *pt = (struct memcpy_thread_data *)data;
	struct	memcpy_in_parms *ip = pt->ip;
	int	err = 0;
	int	errors = 0;
	int	rc, i;
	uint8_t *obuf, *obuf4k;		/* Output buffer */
	struct	ddcb_cmd *ddcb_list;
	struct	ddcb_cmd *ddcb0 = NULL;
	struct	timespec stime = { .tv_sec = 0, .tv_nsec = 0 };
	struct	timespec etime = { .tv_sec = 0, .tv_nsec = 0 };
	uint32_t	mcpy_inp_processed, mcpy_outp_returned;
	uint32_t	mcpy_crc32, mcpy_adler32;
	uint64_t  total_usec = 0;
	int	memcopies = 0;
	int	count = ip->count;
	long long bytes_copied = 0;

	/* Allocate output buffer */
	if (ip->use_sglist) {
		pt->out_ats_type = ATS_TYPE_SGL_RDWR;
		obuf4k = memalign(ip->page_size,
				  ip->data_buf_size + ip->pgoffs_o);
		if (ip->use_sglist > 1)
			accel_pin_memory(pt->accel, obuf4k,
					 ip->data_buf_size + ip->pgoffs_o, 1);
	} else	{
		pt->out_ats_type = ATS_TYPE_FLAT_RDWR;
		obuf4k = accel_malloc(pt->accel,
				      ip->data_buf_size + ip->pgoffs_o);
	}
	if ((ip->data_buf_size != 0) && (obuf4k == NULL)) {
		pr_err("Can not allocate Output Buffer\n");
		err = EX_MEMORY;
		goto __memcpy_exit_1;
	}
	memset(obuf4k, 0xff, ip->data_buf_size + ip->pgoffs_o);
	obuf = obuf4k + ip->pgoffs_o;

	/* Allocate ddcb list */
	ddcb_list = (struct ddcb_cmd *)
		malloc(ip->preload * sizeof(struct ddcb_cmd));
	if (NULL == ddcb_list) {
		pr_err("Can not allocate %d DDCB List\n", ip->preload);
		err = EX_MEMORY;
		goto __memcpy_exit_2;
	}
	VERBOSE1("Thread: %d memcopy: %p (in) to %p (out), pageoffs %d (in) "
		"%d (out), %d bytes Preload: %d\n",
		pt->thread, pt->ibuf, obuf,
		ip->pgoffs_i, ip->pgoffs_o, ip->data_buf_size, ip->preload);
	clock_gettime(CLOCK_MONOTONIC_RAW, &stime);
	pt->stime.tv_sec = stime.tv_sec;	/* Save Start Time */
	pt->stime.tv_nsec = stime.tv_nsec;	/* Save Start Time */

	for (count = 0; count < ip->count; count++) {
		if (stop_memcopying) break;
		int xerrno;

		/* preset output buffer when we check results */
		if (ip->force_cmp)
			memset(obuf, 0x55, ip->data_buf_size);

		clock_gettime(CLOCK_MONOTONIC_RAW, &stime);

		rc = accel_memcpy(pt->accel, ddcb_list, ip->preload,
				  obuf, ip->data_buf_size, pt->out_ats_type,
				  pt->ibuf, ip->data_buf_size,
				  ip->in_ats_type,
				  &mcpy_crc32, &mcpy_adler32,
				  &mcpy_inp_processed,
				  &mcpy_outp_returned,
				  ip->err_inj);
		xerrno = errno;

		clock_gettime(CLOCK_MONOTONIC_RAW, &etime);
		pt->etime.tv_sec = etime.tv_sec;	/* Save End Time */
		pt->etime.tv_nsec = etime.tv_nsec;	/* Save End Time */
		total_usec += tdiff_us(&etime, &stime);
		ddcb0 = ddcb_list;	/* i only use the 1st ddcb */

		if (rc != DDCB_OK) {
			struct _asv_runtime_dma_error *d;
			fprintf(stderr,
				"\nERR: Thread: %d MEMCOPY DDCB[%d] failed, "
				"%s (%d)\n"
				"     errno=%d %s\n",
				pt->thread, pt->memcopies,
				ddcb_strerror(rc), rc, xerrno,
				strerror(xerrno));
			fprintf(stderr, "  RETC: %03x %s ATTN: %x PROGR: %x\n"
				"  from card CRC32: %08x ADLER: %08x\n"
				"  original  CRC32: %08x ADLER: %08x\n",
				ddcb0->retc, ddcb_retc_strerror(ddcb0->retc),
				ddcb0->attn, ddcb0->progress,
				mcpy_crc32, mcpy_adler32, ip->mcpy_crc32,
				ip->mcpy_adler32);

			fprintf(stderr, "  DEQUEUE=%016llx CMPLT=%016llx "
				"DISP=%016llx\n",
				(long long)ddcb0->deque_ts,
				(long long)ddcb0->cmplt_ts,
				(long long)ddcb0->disp_ts);
			if ((ddcb0->retc == DDCB_RETC_UNEXEC) &&
			    (ddcb0->attn == 0xe007)) {
				d = (struct _asv_runtime_dma_error *)
					ddcb0->asv;
				ddcb_print_dma_err(d);
			}
			ddcb_hexdump(stderr, ddcb0->asv, sizeof(ddcb0->asv));
			err = EX_ERR_CARD;
			goto __memcpy_exit_3;
		}
		/* Check CRC and Adler */
		if ((mcpy_crc32 != ip->mcpy_crc32) ||
		    (mcpy_adler32 != ip->mcpy_adler32)) {
			fprintf(stderr, "ERR: Thread: %d CRC/ADLER does not "
				"match!\n"
				"  from card CRC32: %08x ADLER: %08x\n"
				"  original  CRC32: %08x ADLER: %08x "
				"at %d of %d loops\n",
				pt->thread, mcpy_crc32, mcpy_adler32,
				ip->mcpy_crc32, ip->mcpy_adler32, count,
				ip->count);
			errors++;
		}
		/* Was all data processed? */
		if ((ip->data_buf_size != (int)mcpy_inp_processed) ||
		    (ip->data_buf_size != (int)mcpy_outp_returned)) {
			fprintf(stderr, "ERR: Thread: %d IN/OUT sizes do "
				"not match!\n"
				"  from card IN: %08x OUT: %08x\n"
				"  original  IN: %08x OUT: %08x at %d of %d "
				"loops\n", pt->thread,
				mcpy_inp_processed, mcpy_outp_returned,
				ip->data_buf_size, ip->data_buf_size,
				count, ip->count);
			errors++;
		}
		if (ip->force_cmp || errors) {
			/* Check if data is correct  ... */
			for (i = 0; i < ip->data_buf_size; i++) {
				if (obuf[i] != pt->ibuf[i]) {
					EVERBOSE("\nERR: Thread: %d @ "
						 "offs %08x\n"
						 "  RETC: %03x %s ATTN: %x "
						 "PROGR: %x\n"
						 "  INP_PROCESSED: %08x "
						 "OUTP_RETURNED: %08x\n",
						 pt->thread, i, ddcb0->retc,
						 ddcb_retc_strerror(ddcb0->retc),
						 ddcb0->attn, ddcb0->progress,
						 mcpy_inp_processed,
						 mcpy_outp_returned);
					errors++;
					break;
				}
			}
			if (i < ip->data_buf_size) {
				int offs;
				unsigned int len;

				offs = i - 32;
				if (offs < 0) offs = 0;
				len  = MIN(64, ip->data_buf_size - offs);
				EVERBOSE("memcopy src buffer (%p):\n",
					 pt->ibuf);
				__hexdump(&pt->ibuf[offs], len, offs);
				EVERBOSE("memcopy dst buffer (%p):\n",
					 obuf);
				__hexdump(&obuf[offs], len, offs);
				errors++;
			}
		}
		if (errors) break;
		memcopies += ip->preload;
		bytes_copied += (long long)ip->preload * ip->data_buf_size;
	}

	/* write output data if requested to do so only for 1st thread (0) */
	if (0 == pt->thread) {
		if (NULL != ip->o_fp) {
			rc = fwrite(obuf, 1, ip->data_buf_size, ip->o_fp);
			if (rc != ip->data_buf_size) {
				pr_err("can not write output file !\n");
				err = EX_ERRNO;
			}
			fclose(ip->o_fp);
			ip->o_fp = NULL;
		}
	}

	/* Return data to main */
	pt->errors = errors;
	pt->memcopies = memcopies;
	pt->bytes_copied = bytes_copied;
	pt->total_usec = total_usec;

__memcpy_exit_3:
	/* free my ddcb list */
	free(ddcb_list);
__memcpy_exit_2:
	/* Free output buffer */
	if (ip->use_sglist) {
		if (ip->use_sglist > 1)
			accel_unpin_memory(pt->accel, obuf4k,
					   ip->data_buf_size + ip->pgoffs_o);
		free(obuf4k);
	} else accel_free(pt->accel, obuf4k,
			  ip->data_buf_size + ip->pgoffs_o);
	obuf4k = NULL;
__memcpy_exit_1:
	pt->err = err;
	return NULL;
}

/* Free input buffer for each Thread */
static int __memcpy_free_ibuf(struct memcpy_in_parms *ip,
			      struct memcpy_thread_data *pt)
{
	/* the last one must free ibuf */
	if (ip->use_sglist) {
		if (ip->use_sglist > 1)
			accel_unpin_memory(pt->accel, pt->ibuf4k,
					   ip->data_buf_size + ip->pgoffs_i);
		free(pt->ibuf4k);
	} else accel_free(pt->accel, pt->ibuf4k,
			  ip->data_buf_size + ip->pgoffs_i);
	pt->ibuf4k = NULL;
	return 0;
}

/* Allocate input buffer per Thread */
static int __memcpy_alloc_ibuf(struct memcpy_in_parms *ip,
			       struct memcpy_thread_data *pt)
{
	int	i;
	size_t	fread_size = 0;

	if (ip->use_sglist) {
		ip->in_ats_type = ATS_TYPE_SGL_RDWR;
		pt->ibuf4k = memalign(ip->page_size,
				      ip->data_buf_size + ip->pgoffs_i);
		if (ip->use_sglist > 1)
			accel_pin_memory(pt->accel, pt->ibuf4k,
					 ip->data_buf_size + ip->pgoffs_i, 0);
	} else {
		ip->in_ats_type = ATS_TYPE_FLAT_RD;
		pt->ibuf4k = accel_malloc(pt->accel,
					  ip->data_buf_size + ip->pgoffs_i);
	}

	if ((ip->data_buf_size != 0) && (pt->ibuf4k == NULL)) {
		pr_err("Can not allocate Input memory\n");
		return EX_MEMORY;
	}
	/* preset full input buffer */
	memset(pt->ibuf4k, 0xee, ip->data_buf_size + ip->pgoffs_i);
	pt->ibuf = pt->ibuf4k + ip->pgoffs_i;

	/* preset partial input buffer in case pgoffs_i is set */
	if (ip->fpattern) {
		fread_size = fread(pt->ibuf, 1, ip->data_buf_size,
				   ip->fpattern);
		if ((int)fread_size != ip->data_buf_size) {
			pr_err("Can not read pattern file!\n");
			return EX_ERRNO;
		}
		fclose(ip->fpattern);
	} else {
		for (i = 0; i < ip->data_buf_size; i++) /* preset inp buffer */
			pt->ibuf[i] = (uint8_t)i;
	}
	if (0 == pt->thread) {
		/* Create Adler and CRC from Input buffer, which is
		   thea same for each thread */
		ip->mcpy_adler32 = adler32(0L, Z_NULL, 0); /* start value */
		ip->mcpy_adler32 = adler32(ip->mcpy_adler32, pt->ibuf,
					   ip->data_buf_size);
		ip->mcpy_crc32 = crc32(0L, Z_NULL, 0); /* start value */
		ip->mcpy_crc32 = crc32(ip->mcpy_crc32 , pt->ibuf,
				       ip->data_buf_size);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int cmd;
	char *endptr = NULL;
	pthread_t tid;
	int thread;
	char *out_f;		/* Output File name used */
	int err_code;
	unsigned long long frequency, wtime_usec = 0, wtime_e = 0;

	/* Summ for all threads */
	long long	bytes_copied = 0;
	uint64_t	total_usec = 0;
	uint64_t	total_msec = 0;
	int		memcopies = 0;
	int		errors = 0;
	int		mib, kib;
	unsigned long	kibs, mibs;
	struct	memcpy_thread_data	*tdata;
	struct	memcpy_thread_data	*pt;
	struct	memcpy_in_parms ip;

	ip.card_no = 0;
	ip.card_type = DDCB_TYPE_GENWQE;
	ip.mode = DDCB_MODE_RDWR | DDCB_MODE_ASYNC;
	ip.quiet = false;			/* not quiet */
	ip.cpu = -1;
	ip.count = 1;
	ip.force_cmp = false;
	ip.use_sglist = 0;
	ip.preload = 1;
	ip.threads = 1;
	ip.o_fp = NULL;
	ip.fpattern = NULL;
	ip.in_ats_type = ATS_TYPE_FLAT_RD;	/* default, no SGL */
	ip.page_size = sysconf(_SC_PAGESIZE);
	ip.data_buf_size = 4096;		/* for inbuff and outbuff */
	ip.pgoffs_i = 0;
	ip.pgoffs_o = 0;
	ip.mcpy_crc32 = 0;
	ip.mcpy_adler32 = 0;
	ip.have_threads = 0;
	ip.err_inj = ERR_INJ_NONE;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			/* functions */

			/* options */
			{ "card",		required_argument, NULL, 'C' },
			{ "accelerator-type",	required_argument, NULL, 'A' },
			{ "cpu",		required_argument, NULL, 'X' },
			{ "use-sglist",		no_argument,       NULL, 'G' },
			{ "nonblocking",	no_argument,       NULL, 'n' },

			{ "bufsize",		required_argument, NULL, 's' },
			{ "patternfile",	required_argument, NULL, 'p' },
			{ "count",		required_argument, NULL, 'c' },
			{ "preload",		required_argument, NULL, 'l' },
			{ "pgoffs_i",		required_argument, NULL, 'i' },
			{ "pgoffs_o",		required_argument, NULL, 'o' },
			{ "force-compare",	required_argument, NULL, 'F' },
			{ "threads",		required_argument, NULL, 't' },
			{ "err-inject",		required_argument, NULL, 'Y' },


			/* misc/support */
			{ "version",       no_argument,       NULL, 'V' },
			{ "debug",	   no_argument,	      NULL, 'D' },
			{ "quiet",	   no_argument,	      NULL, 'q' },
			{ "verbose",	   no_argument,       NULL, 'v' },
			{ "help",	   no_argument,       NULL, 'h' },
			{ 0,		   no_argument,       NULL, 0   },
		};

		cmd = getopt_long(argc, argv, "nqGDFi:o:p:s:c:C:A:X:vVhl:t:Y:",
				long_options, &option_index);
		if (cmd == -1)	/* all params processed ? */
			break;

		switch (cmd) {
		case 'C':
			if (strcmp(optarg, "RED") == 0) {
				ip.card_no = ACCEL_REDUNDANT;
				break;
			}
			ip.card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 'A':		/* set card number */
			if (strcmp(optarg, "GENWQE") == 0) {
				ip.card_type = DDCB_TYPE_GENWQE;
				break;
			}
			if (strcmp(optarg, "CAPI") == 0) {
				ip.card_type = DDCB_TYPE_CAPI;
				break;
			}
			ip.card_type = strtol(optarg, (char **)NULL, 0);
			if ((DDCB_TYPE_GENWQE != ip.card_type) &&
				(DDCB_TYPE_CAPI != ip.card_type)) {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'X':
			ip.cpu = strtoul(optarg, (char **)NULL, 0);
			break;
		case 'G':
			ip.use_sglist++;
			break;
		case 'c':
			ip.count = strtol(optarg, (char **)NULL, 0);
			break;
		case 'i':
			ip.pgoffs_i = strtoul(optarg, &endptr, 0);
			if ((optarg && (((char *)optarg)[0] == '-'))
			    || (*endptr != '\0')) {
				pr_err("illegal input offset!\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'o':
			ip.pgoffs_o = strtoul(optarg, &endptr, 0);
			if ((optarg && (((char *)optarg)[0] == '-'))
			    || (*endptr != '\0')) {
				pr_err("illegal output offset!\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 's':
			ip.data_buf_size = str_to_num(optarg);
			break;
		case 'p':
			ip.fpattern = fopen(optarg, "rb");
			if (ip.fpattern == NULL) {
				pr_err("Pattern file %s not found!\n", optarg);
			} else {
				fseek(ip.fpattern, 0L, SEEK_END);
				ip.data_buf_size = ftell(ip.fpattern);
				fseek(ip.fpattern, 0L, SEEK_SET);
			}
			break;

		case 'l':	/* preload */
			ip.preload = strtol(optarg, (char **)NULL, 0);
			break;

		case 't':	/* threads */
			ip.threads = strtol(optarg, (char **)NULL, 0);
			break;

		case 'F':
			ip.force_cmp = true;
			break;
		case 'n':
			ip.mode |= DDCB_MODE_NONBLOCK;
			break;

		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
		case 'D':
			/* debug_flag++; *//*FIXME */
			break;
		case 'q':
			ip.quiet = true;
			break;
		case 'Y':
			ip.err_inj = strtol(optarg, (char **)NULL, 0);
			break;
		case 'v':
			verbose_flag++;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (ACCEL_REDUNDANT == ip.card_no) {
		if (1 != ip.use_sglist) {
			pr_info("Option -G set when in redundant card "
				"mode!\n");
			ip.use_sglist = 1;
		}
	}

	if (optind < argc) {	/* output file */
		out_f = argv[optind++];
		ip.o_fp = fopen(out_f, "w+");
		if (NULL == ip.o_fp) {
			pr_err("can not open output file '%s': %s\n",
			       out_f, strerror(errno));
			exit(EX_ERRNO);
		}
	}

	if (optind != argc) {   /* now it must fit */
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	if ((ip.card_type != DDCB_TYPE_CAPI) && (1 != ip.preload)) {
		printf("Note: Use Preload option only on CAPI Card !\n");
		exit(EXIT_FAILURE);
	}

	switch_cpu(ip.cpu, verbose_flag);
	if (verbose_flag > 1)
	ddcb_debug(verbose_flag - 1);

	/* Allocate Thread data */
	tdata = (struct memcpy_thread_data*)
		malloc(ip.threads * sizeof(struct memcpy_thread_data));
	if (NULL == tdata) {
		pr_err("Can not allocate memory Thread Data\n");
		exit(EX_MEMORY);
	}

	ip.stime.tv_sec = -1;;
	ip.stime.tv_nsec = -1;;
	ip.etime.tv_sec  = 0;
	ip.etime.tv_nsec = 0;

	signal(SIGINT, INT_handler);

	pt = &tdata[0];
	for (thread = 0; thread < ip.threads; thread++, pt++) {
		pt->thread = thread;
		pt->ip = &ip;		/* Set input parms */
		pt->err = 0;
		pt->errors = 0;
		pt->bytes_copied = 0;
		pt->memcopies = 0;
		pt->total_usec = 0;
		pt->tid = 0;

		pt->accel = accel_open(ip.card_no, ip.card_type, ip.mode,
			&err_code, 0, DDCB_APPL_ID_IGNORE);
		if (NULL == pt->accel) {
			pr_err("Failed to open card %u type %u (%d/%s)\n",
				ip.card_no, ip.card_type, err_code,
				accel_strerror(pt->accel, err_code));
			pt->err = EX_ERR_CARD;
			continue;
		}
		/* Alloc ibuf */
		pt->err = __memcpy_alloc_ibuf(&ip, pt);
	}
	pt = &tdata[0];
	for (thread = 0; thread < ip.threads; thread++, pt++) {
		if (0 == pt->err) {
			if (0 == pthread_create(&tid, NULL,
						&__memcpy_thread, pt)) {
				pt->tid = tid;
				ip.have_threads++;
			}
		}
	}

	pt = &tdata[0];
	for (thread = 0; thread < ip.threads; thread++) {
		if (0 == pt->tid) {		/* Skip if tid is not set */
			errors++;
			VERBOSE0("Thread: %d, tid: 0 err: %d\n",
				 thread, pt->err);
			continue;
		}
		pthread_join(pt->tid, NULL);	/* wait for good tid */
		ip.have_threads--;
		if (pt->err) {
			errors++;
			VERBOSE0("Thread: %d, err: %d\n", thread, pt->err);
		} else {
			if (false == ip.quiet) {
				kib = (int)(pt->bytes_copied / 1024);
				mib = kib / 1024;
				VERBOSE1("Thread: %d, memcopies: %d, done, "
					 "%lld bytes, %lld usec, ",
					 thread, pt->memcopies,
					 (long long)pt->bytes_copied,
					 (long long)pt->total_usec);
				/* FIXME: this is not 100 % good code,
				   i know the format_flag is bad */
				if (pt->total_usec < 100000) {
					kibs = ((pt->bytes_copied * 1000000) /
						1024) / pt->total_usec;
					VERBOSE1("%d KiB, in %lld usec, "
						 "%ld KiB/sec", kib,
						 (long long)pt->total_usec,
						 kibs);
				} else {
					total_msec = pt->total_usec / 1000;
					/* now msec */
					mibs = (pt->bytes_copied * 1000) /
						(1024 * 1024) / total_msec;
					VERBOSE1("%d MiB, in %lld msec, "
						 "%ld MiB/sec", mib,
						 (long long)total_msec, mibs);
				}
				VERBOSE1(" %d errors.\n", pt->errors);
			}
		}
		bytes_copied += pt->bytes_copied;
		memcopies += pt->memcopies;
		errors += pt->errors;
		__memcpy_free_ibuf(&ip, pt);

		if (thread == ip.threads - 1) {
			wtime_e = accel_get_queue_work_time(pt->accel);
			frequency = accel_get_frequency(pt->accel);
			wtime_usec = frequency ? wtime_e /
				(frequency/1000000) : 0;
		}

		accel_close(pt->accel);

		VERBOSE1("Thread %02d Start: %08lld - %08lld "
			 "End: %08lld - %08lld\n", thread,
			 (long long)pt->stime.tv_sec,
			 (long long)pt->stime.tv_nsec,
			 (long long)pt->etime.tv_sec,
			 (long long)pt->etime.tv_nsec);
		/* Update lowest start time */
		time_low(&ip.stime, &pt->stime);
		/* Update highest end time */
		time_high(&ip.etime, &pt->etime);

		pt->accel = NULL;
		pt++;
	}

	if (false == ip.quiet) {
		kib = (int)(bytes_copied / 1024);
		mib = kib / 1024;
		VERBOSE0("--- MEMCOPY statistics ---\n"
			"%d memcopies done, %lld bytes, ",
			 memcopies, bytes_copied);

		total_usec = tdiff_us(&ip.etime, &ip.stime);
		/* Avoid div fault */
		if (total_usec) {
			if (total_usec < 100000) {
				kibs = ((bytes_copied * 1000000) / 1024) /
					total_usec;
				VERBOSE0("%d KiB, in %lld/%lld usec, "
					 "%ld KiB/sec,", kib,
					 (long long)total_usec,
					 wtime_usec, kibs);
			} else {
				total_msec = total_usec / 1000;	/* now msec */
				mibs = (bytes_copied * 1000) /
					(1024 * 1024) / total_msec;
				VERBOSE0("%d MiB, in %lld/%lld msec, "
					 "%ld MiB/sec,", mib,
					 (long long)total_msec,
					 wtime_usec/1000, mibs);
			}
		}
		VERBOSE0(" %d errors.\n", errors);
	}

	free(tdata);
	if (errors != 0)
		exit(EX_ERR_DATA);

	exit(EXIT_SUCCESS);
}
