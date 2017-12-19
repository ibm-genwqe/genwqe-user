/*
 * Copyright 2017, International Business Machines
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
#include <malloc.h>
#include <time.h>
#include <signal.h>
#include <zlib.h>
#include <time.h>
#include <sys/time.h>
#include <asm/byteorder.h>

#include <sched.h>

#include "libddcb.h"
#include "genwqe_tools.h"
#include "force_cpu.h"
#include "libcard.h"
#include "memcopy_ddcb.h"

int verbose_flag = 0;
static int debug_flag = 0;

#define DEFAULT_DATA_BUF_SIZE (2 * 1024 * 1024) // 2 MB Buffer

static const char *version = GIT_VERSION;

static uint64_t get_us(void)
{
	uint64_t t;
	struct timespec now;	

	clock_gettime(CLOCK_MONOTONIC_RAW, &now);
	t = now.tv_sec * 1000000 + now.tv_nsec / 1000;
	return t;
}

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s [-h] [-v, --verbose] [-C, --card <cardno>|RED]\n"
	       "\t-C, --card <cardno> use cardno for operation (default 0)\n"
               "\t-A, --accel = GENWQE | CAPI. (CAPI only for ppc64le)\n"
	       "\t-V, --version show Software Version\n"
	       "\t-X, --cpu <only run on this CPU number>\n"
	       "\t-D, --debug <create extended debug data on failure>\n"
	       "\t-G, --use-sglist use the scatter gather list support\n"
	       "\t-c, --check-result] check result against the software\n"
	       "\t-s, --bufsize <bufsize/default is 4KiB>\n"
	       "\t-a, --adler32 use adler32 instead of crc32\n"
	       "\tFILE...\n"
	       "\n"
	       "This utility sends memcopy/checksum DDCBs to the application\n"
	       "chip unit. The CRC32 is compatible to zlib. The UNIX program\n"
	       "cksum is using a different variation of the algorithm.\n\n",
	       prog);
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

static int accel_cksum(accel_t accel,
			    struct ddcb_cmd *cmd,
			    void *src, void *dest, size_t n,
			    uint32_t *crc32,
			    uint32_t *adler32,
			    uint32_t *inp_processed,
			    int use_sglist)
{
	int rc;
	struct asiv_memcpy *asiv;
	struct asv_memcpy *asv;
	uint64_t ats_type;

	ddcb_cmd_init(cmd);
	cmd->ddata_addr = 0ull;
	cmd->acfunc	= DDCB_ACFUNC_APP;	/* goto accelerator */
	cmd->cmd	= ZCOMP_CMD_ZEDC_MEMCOPY;
	cmd->cmdopts	= 0x0000;		/* use memcopy */
	cmd->asiv_length= 0x40 - 0x20;
	cmd->asv_length	= 0xC0 - 0x80;		/* try to absorb all */

	/* setup ASIV part */
	asiv = (struct asiv_memcpy *)&cmd->asiv;
	asiv->inp_buff      = __cpu_to_be64((unsigned long)src);
	asiv->inp_buff_len  = __cpu_to_be32((uint32_t)n);
	asiv->outp_buff     = __cpu_to_be64((unsigned long)dest);
	asiv->outp_buff_len = __cpu_to_be32((uint32_t)n);
	asiv->in_adler32    = __cpu_to_be32(*adler32);
	asiv->in_crc32      = __cpu_to_be32(*crc32);

	if (use_sglist)
		ats_type = ATS_TYPE_SGL_RD;
	else	ats_type = ATS_TYPE_FLAT_RD;
	cmd->ats = ATS_SET_FLAGS(struct asiv_memcpy, inp_buff, ats_type);
	if (use_sglist)
		ats_type = ATS_TYPE_SGL_RDWR;
	else	ats_type = ATS_TYPE_FLAT_RDWR;
	cmd->ats |= ATS_SET_FLAGS(struct asiv_memcpy, outp_buff, ats_type);

	if (verbose_flag) {
		fprintf(stderr, "ATS: 0x%llx use_sglist: %d\n", (long long)cmd->ats, use_sglist);
		fprintf(stderr, "Src:  %p\n", src);
		fprintf(stderr, "Dest: %p\n", dest);
		fprintf(stderr, "Len: 0x%x\n", (uint32_t)n);
	}

	if (verbose_flag > 1) {
		fprintf(stderr, "\n Dump Data @ %p\n", cmd);
		ddcb_hexdump(stderr, cmd, sizeof(struct ddcb_cmd));
	}
	cmd->disp_ts    = get_us();             /* @ 0x30 SW Usage */
	rc = accel_ddcb_execute(accel, cmd, NULL, NULL);
	cmd->disp_ts    = get_us() - cmd->disp_ts;
	if (verbose_flag > 1) {
		fprintf(stderr, "\n Dump Data @ %p\n", cmd);
		ddcb_hexdump(stderr, cmd, sizeof(struct ddcb_cmd));
	}

	asv = (struct asv_memcpy *)&cmd->asv;
	*crc32	       = __be32_to_cpu(asv->out_crc32);
	*adler32       = __be32_to_cpu(asv->out_adler32);
	*inp_processed = __be32_to_cpu(asv->inp_processed);

	if (verbose_flag)
		fprintf(stderr, "  crc32=%u adler32=%u inp_processed=%u in %lld usec\n",
			*crc32, *adler32, *inp_processed, (long long)cmd->disp_ts);

	return rc;
}

static int process_in_file(accel_t accel, const char *in_f,
			   uint8_t *ibuf, void *obuf, int ibuf_size,
			   int check_result,
			   int use_sglist,
                           int use_adler)
{
	int rc, size_f;
	struct stat st;
	FILE *i_fp;
	uint32_t crc = 0, m_crc32 = 0; /* defined start value of 0 */
	uint32_t m_adler32 = 1;	       /* defined start value of 1 */
	uint32_t m_inp_processed;
	struct ddcb_cmd cmd;
	int xerrno;

	if (check_result)
		crc = crc32(0L, Z_NULL, 0); /* start value */


	if (stat(in_f, &st) == -1) {
		fprintf(stderr, "err: stat on input file (%s)\n",
			strerror(errno));
			exit(EX_ERRNO);
	}
	size_f = st.st_size;

	i_fp = fopen(in_f, "r");
	if (!i_fp) {
		pr_err("err: can't open input file %s: %s\n", in_f,
			strerror(errno));
		exit(EX_ERRNO);
	}

	while (size_f) {
		int tocopy = MIN(ibuf_size, size_f);

		rc = fread(ibuf, tocopy, 1, i_fp);
		if (rc != 1) {
			pr_err("err: can't read input file %s: %s\n", in_f,
				strerror(errno));
			exit(EX_ERRNO);
		}

		if (check_result)
			crc = crc32(crc, ibuf, tocopy); /* software */

		rc = accel_cksum(accel, &cmd, ibuf, obuf, tocopy, /* hardware */
				      &m_crc32, &m_adler32, &m_inp_processed,
				      use_sglist);
		xerrno = errno;

		/* Did the ioctl succeed? */
		if (rc != DDCB_OK) {
			struct asv_runtime_dma_error *d;

			fprintf(stderr,
				"\nerr: CKSUM DDCB failed, %s (%d)\n"
				"     errno=%d %s\n", card_strerror(rc),
				rc, xerrno, strerror(xerrno));

			fprintf(stderr, "  RETC: %03x %s ATTN: %x PROGR: %x\n"
				"  from card CRC32: %08x ADLER: %08x\n"
				"  DEQUEUE=%016llx CMPLT=%016llx DISP=%016llx\n",
				cmd.retc, retc_strerror(cmd.retc),
				cmd.attn, cmd.progress, m_crc32, m_adler32,
				(long long)cmd.deque_ts,
				(long long)cmd.cmplt_ts,
				(long long)cmd.disp_ts);

			if ((cmd.retc == DDCB_RETC_UNEXEC) &&
			    (cmd.attn == 0xe007)) {
				d = (struct asv_runtime_dma_error *)cmd.asv;
				fprintf(stderr,
					" raddr: %016llx rfmt/chan/disc: %08x "
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
			ddcb_hexdump(stderr, cmd.asv, sizeof(cmd.asv));
			exit(EXIT_FAILURE);
		} else
			pr_info("  RETC: %03x %s ATTN: %x PROGR: %x\n"
				"  from card CRC32: %08x ADLER: %08x\n"
				"  DEQUEUE=%016llx CMPLT=%016llx DISP=%016llx\n",
				cmd.retc, retc_strerror(cmd.retc),
				cmd.attn, cmd.progress, m_crc32, m_adler32,
				(long long)cmd.deque_ts,
				(long long)cmd.cmplt_ts,
				(long long)cmd.disp_ts);

		size_f -= tocopy;
	}

	if (use_adler)
		printf("%u %llu %s\n", m_adler32, (long long)st.st_size, in_f);
	else
		printf("%u %llu %s\n", m_crc32, (long long)st.st_size, in_f);

	if ((check_result) && (m_crc32 != crc)) {
		fprintf(stderr, "err: CRCs do not match %u != %u\n",
			m_crc32, crc);
	}

	fclose(i_fp);
	return 0;
}


int main(int argc, char *argv[])
{
	int card_no = 0, err_code;
	accel_t accel;
	uint8_t *ibuf, *obuf;
	unsigned int page_size = sysconf(_SC_PAGESIZE);
	const char *in_f = NULL;
	int cpu = -1;
	int card_type = DDCB_TYPE_GENWQE;
	int check_result = 0;
	int use_sglist = 0;
	int use_adler = 0;
	int data_buf_size = DEFAULT_DATA_BUF_SIZE;

	while (1) {
		int ch;
		int option_index = 0;

		static struct option long_options[] = {
			/* functions */

			/* options */
			{ "card",	   required_argument, NULL, 'C' },
			{ "accel",         required_argument, NULL, 'A' },
			{ "cpu",	   required_argument, NULL, 'X' },
			{ "use-sglist",	   no_argument,       NULL, 'G' },
			{ "use-adler32",   no_argument,       NULL, 'a' },
			{ "check-result",  no_argument,       NULL, 'c' },

			{ "bufsize",	   required_argument, NULL, 's' },

			/* misc/support */
			{ "version",       no_argument,       NULL, 'V' },
			{ "debug",	   no_argument,	      NULL, 'D' },
			{ "verbose",	   no_argument,       NULL, 'v' },
			{ "help",	   no_argument,       NULL, 'h' },
			{ 0,		   no_argument,       NULL, 0   },
		};

		ch = getopt_long(argc, argv, "acC:X:Gs:A:vDVh",
				 long_options, &option_index);
		if (ch == -1)	/* all params processed ? */
			break;

		switch (ch) {
		/* which card to use */
		case 'C':
			if (strcmp(optarg, "RED") == 0) {
				card_no = GENWQE_CARD_REDUNDANT;
				break;
			}
			card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 'A':
			if (strcmp(optarg, "GENWQE") == 0) {
				card_type = DDCB_TYPE_GENWQE;
				break;
			}
			if (strcmp(optarg, "CAPI") == 0) {
				card_type = DDCB_TYPE_CAPI;
				break;
			}
			/* use numeric card_type value */
			card_type = strtol(optarg, (char **)NULL, 0);
			if ((DDCB_TYPE_GENWQE != card_type) &&
				(DDCB_TYPE_CAPI != card_type)) {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		case 'X':
			cpu = strtoul(optarg, (char **)NULL, 0);
			break;
		case 'G':
			use_sglist++;
			break;
		case 'a':
			use_adler = 1;
			break;
		case 'c':
			check_result++;
			break;
		case 's':
			data_buf_size = str_to_num(optarg);
			break;

		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'V':
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
		case 'D':
			debug_flag++;
			break;
		case 'v':
			verbose_flag++;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	switch_cpu(cpu, verbose_flag);
	ddcb_debug(verbose_flag - 1);
	genwqe_card_lib_debug(verbose_flag);
	if (ACCEL_REDUNDANT == card_no) {
		if (1 != use_sglist) {
			pr_info("I have to set Option -G set when in "
				"redundant card mode!\n");
			use_sglist = 1;
		}
	}

	accel = accel_open(card_no, card_type, DDCB_MODE_RDWR | DDCB_MODE_ASYNC,
		&err_code, 0, DDCB_APPL_ID_IGNORE);
	if (accel == NULL) {
		printf("Err: (card: %d type: %d) Faild to open card:%s/%d; %s\n",
			card_no, card_type,
			card_strerror(err_code), err_code, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (use_sglist) {
		ibuf = memalign(page_size, data_buf_size);
		obuf = memalign(page_size, data_buf_size);
		if (use_sglist > 1) {
			accel_pin_memory(accel, ibuf, data_buf_size, 0);
			accel_pin_memory(accel, obuf, data_buf_size, 0);
		}
	} else {
		ibuf = accel_malloc(accel, data_buf_size);
		obuf = accel_malloc(accel, data_buf_size);
	}

	if ((ibuf == NULL) || (obuf == NULL)) {
		pr_err("cannot allocate memory\n");
		exit(EXIT_FAILURE);
	}

	while (optind < argc) {	/* input file */
		in_f = argv[optind++];
		process_in_file(accel, in_f, ibuf, obuf, data_buf_size,
			check_result, use_sglist, use_adler);
	}

	if (use_sglist) {
		if (use_sglist > 1) {
			accel_unpin_memory(accel, ibuf, data_buf_size);
			accel_unpin_memory(accel, obuf, data_buf_size);
		}
		free(ibuf);
		free(obuf);
	} else {
		accel_free(accel, ibuf, data_buf_size);
		accel_free(accel, obuf, data_buf_size);
	}

	accel_close(accel);
	exit(EXIT_SUCCESS);
}
