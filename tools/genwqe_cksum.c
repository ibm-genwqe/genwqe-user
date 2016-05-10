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
#include <malloc.h>
#include <time.h>
#include <signal.h>
#include <zlib.h>
#include <sys/time.h>
#include <asm/byteorder.h>

#include <sched.h>

#include "genwqe_tools.h"
#include "force_cpu.h"
#include "libcard.h"
#include "memcopy_ddcb.h"

int verbose_flag = 0;
static int debug_flag = 0;

static int DATA_BUF_SIZE = 4096 * 512;
static int use_sglist = 0;
static int use_adler32 = 0;
static int check_result = 0;
static const char *version = GENWQE_LIB_VERS_STRING;

/**
 * @brief	prints valid command line options
 *
 * @param prog	current program's name
 */
static void usage(const char *prog)
{
	printf("Usage: %s [-h] [-v, --verbose] [-C, --card <cardno>|RED]\n"
	       "\t[-V, --version]\n"
	       "\t[-X, --cpu <only run on this CPU number>]\n"
	       "\t[-D, --debug <create extended debug data on failure>]\n"
	       "\t[-G, --use-sglist use the scatter gather list support]\n"
	       "\t[-c, --check-result] check result against the software\n"
	       "\t[-s, --bufsize <bufsize/default is 4KiB>]\n"
	       "\t[-a, --adler32] use adler32 instead of crc32\n"
	       "\t[-i, --pgoffs_i <offs>] byte offset for input buffer\n"
	       "\t[FILE]...\n"
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

static int genwqe_card_cksum(card_handle_t card,
			    struct genwqe_ddcb_cmd *cmd,
			    void *src, size_t n,
			    uint32_t *crc32,
			    uint32_t *adler32,
			    uint32_t *inp_processed,
			    struct genwqe_debug_data *debug_data)
{
	int rc;
	struct asiv_memcpy *asiv;
	struct asv_memcpy *asv;

	genwqe_ddcb_cmd_init(cmd);
	cmd->ddata_addr = (unsigned long)debug_data;
	cmd->acfunc	= DDCB_ACFUNC_APP;	/* goto accelerator */
	cmd->cmd	= ZCOMP_CMD_ZEDC_MEMCOPY;
	cmd->cmdopts	= 0x0001;		/* discard output for cksum */
	cmd->asiv_length= 0x40 - 0x20;
	cmd->asv_length	= 0xC0 - 0x80;		/* try to absorb all */

	/* setup ASIV part */
	asiv = (struct asiv_memcpy *)&cmd->asiv;
	asiv->inp_buff      = __cpu_to_be64((unsigned long)src);
	asiv->inp_buff_len  = __cpu_to_be32((uint32_t)n);
	asiv->outp_buff     = __cpu_to_be64(0);
	asiv->outp_buff_len = __cpu_to_be32(0);
	asiv->in_adler32    = __cpu_to_be32(*adler32);
	asiv->in_crc32      = __cpu_to_be32(*crc32);

	if (use_sglist) {
		cmd->ats = __cpu_to_be64(
			ATS_SET_FLAGS(struct asiv_memcpy, inp_buff,
				      ATS_TYPE_SGL_RD));
	} else {
		cmd->ats = __cpu_to_be64(
			ATS_SET_FLAGS(struct asiv_memcpy, inp_buff,
				      ATS_TYPE_FLAT_RD));
	}

	rc = genwqe_card_execute_ddcb(card, cmd);

	asv = (struct asv_memcpy *)&cmd->asv;
	*crc32	       = __be32_to_cpu(asv->out_crc32);
	*adler32       = __be32_to_cpu(asv->out_adler32);
	*inp_processed = __be32_to_cpu(asv->inp_processed);

	if (verbose_flag)
		fprintf(stderr, "  crc32=%u adler32=%u inp_processed=%u\n",
			*crc32, *adler32, *inp_processed);

	return rc;
}

static int process_in_file(card_handle_t card, const char *in_f,
			   uint8_t *ibuf, int ibuf_size)
{
	int rc, size_f;
	struct stat st;
	FILE *i_fp;
	uint32_t crc = 0, m_crc32 = 0; /* defined start value of 0 */
	uint32_t m_adler32 = 1;	       /* defined start value of 1 */
	uint32_t m_inp_processed;
	struct genwqe_ddcb_cmd cmd;
	struct genwqe_debug_data debug_data;
	int xerrno;

	if (check_result)
		crc = crc32(0L, Z_NULL, 0); /* start value */

	memset(&debug_data, 0, sizeof(debug_data));

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
			crc = crc32(crc, ibuf, tocopy);	/* software */

		rc = genwqe_card_cksum(card, &cmd, ibuf, tocopy, /* hardware */
				      &m_crc32, &m_adler32, &m_inp_processed,
				      debug_flag ? &debug_data : NULL);
		xerrno = errno;

		if (debug_flag && verbose_flag)
			genwqe_print_debug_data(stdout, &debug_data,
						GENWQE_DD_ALL);

		/* Did the ioctl succeed? */
		if (rc != GENWQE_OK) {
			struct asv_runtime_dma_error *d;

			fprintf(stderr,
				"\nerr: CKSUM DDCB failed, %s (%d)\n"
				"     errno=%d %s\n", card_strerror(rc),
				rc, xerrno, strerror(xerrno));

			if (debug_flag && !verbose_flag)
				genwqe_print_debug_data(stdout, &debug_data,
							GENWQE_DD_ALL);

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
			genwqe_hexdump(stderr, cmd.asv, sizeof(cmd.asv));
			exit(EXIT_FAILURE);
		}

		size_f -= tocopy;
	}

	if (use_adler32)
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
	card_handle_t card;
	uint8_t *ibuf, *ibuf4k;
	unsigned int page_size = sysconf(_SC_PAGESIZE);
	const char *in_f = NULL;
	int cpu = -1;
	int pgoffs_i = 0;

	while (1) {
		int ch;
		int option_index = 0;

		static struct option long_options[] = {
			/* functions */

			/* options */
			{ "card",	   required_argument, NULL, 'C' },
			{ "cpu",	   required_argument, NULL, 'X' },
			{ "use-sglist",	   no_argument,       NULL, 'G' },
			{ "use-adler32",   no_argument,       NULL, 'a' },
			{ "check-result",  no_argument,       NULL, 'c' },

			{ "bufsize",	   required_argument, NULL, 's' },
			{ "pgoffs_i",      required_argument, NULL, 'i' },

			/* misc/support */
			{ "version",       no_argument,       NULL, 'V' },
			{ "debug",	   no_argument,	      NULL, 'D' },
			{ "verbose",	   no_argument,       NULL, 'v' },
			{ "help",	   no_argument,       NULL, 'h' },
			{ 0,		   no_argument,       NULL, 0   },
		};

		ch = getopt_long(argc, argv, "acC:X:Gs:i:vDVh",
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
		case 'X':
			cpu = strtoul(optarg, (char **)NULL, 0);
			break;
		case 'G':
			use_sglist++;
			break;
		case 'a':
			use_adler32 = 1;
			break;
		case 'c':
			check_result++;
			break;

		case 'i':
			pgoffs_i = strtol(optarg, (char **)NULL, 0);
			break;
		case 's':
			DATA_BUF_SIZE = str_to_num(optarg);
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
	genwqe_card_lib_debug(verbose_flag);

	card = genwqe_card_open(card_no, GENWQE_MODE_RDWR, &err_code,
				0x475a4950, GENWQE_APPL_ID_MASK);
	if (card == NULL) {
		printf("err: genwqe card: %s/%d; %s\n",
		       card_strerror(err_code), err_code, strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (use_sglist) {
		ibuf4k = memalign(page_size, DATA_BUF_SIZE + pgoffs_i);
		if (use_sglist > 1) {
			genwqe_pin_memory(card, ibuf4k, DATA_BUF_SIZE +
					 pgoffs_i, 0);
		}
	} else {
		ibuf4k = genwqe_card_malloc(card, DATA_BUF_SIZE + pgoffs_i);
	}
	if (DATA_BUF_SIZE != 0 && ibuf4k == NULL) {
		pr_err("cannot allocate memory\n");
		exit(EXIT_FAILURE);
	}
	ibuf = ibuf4k + pgoffs_i;

	while (optind < argc) {	/* input file */
		in_f = argv[optind++];
		process_in_file(card, in_f, ibuf, DATA_BUF_SIZE);
	}

	if (use_sglist) {
		if (use_sglist > 1) {
			genwqe_unpin_memory(card, ibuf4k, DATA_BUF_SIZE +
					   pgoffs_i);
		}
		free(ibuf4k);
	} else {
		genwqe_card_free(card, ibuf4k, DATA_BUF_SIZE + pgoffs_i);
	}

	genwqe_card_close(card);
	exit(EXIT_SUCCESS);
}
