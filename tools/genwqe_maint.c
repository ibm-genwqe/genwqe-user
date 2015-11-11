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

/*
 * Genwqe Capi Card Master Maintenence tool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <stdbool.h>

#include <libddcb.h>
#include "genwqe_tools.h"

static const char *version = GIT_VERSION;
int	verbose_flag = 0;

struct my_sig_data {
	accel_t	card;
	bool	verbose;
	FILE	*fd_out;
};

/* Return true if card Software Release is OK */
static bool check_app(accel_t card)
{
	uint64_t data;

	/* Check Application Version for Version higher than 0403 */
	/* Register 8 does have following layout for the 64 bits */
	/* RRRRFFIINNNNNNNN */
	/* RRRR     == 16 bit Software Release (0404) */
	/* FF       ==  8 bit Software Fix Level on card (01) */
	/* II       ==  8 bit Software Interface ID (03) */
	/* NNNNNNNN == 32 Bit Function (475a4950) = (GZIP) */
	data = accel_get_app_id(card);
	data = data >> 32;
	if (0x03 == (data & 0xff)) {
		/* Check 16 bits Release */
		data = data >> 16;
		if (data > 0x0500)	/* CAPI directed mode starts at 0500 */
			return true;
	}
	return false;
}

static FILE *fd_out;
static bool deamon = false;
static accel_t card;
static pid_t	my_sid;

static void sig_handler(int sig)
{

	fprintf(fd_out, "Sig Handler Signal: %d SID: %d\n", sig, my_sid);
	if (card)
		accel_close(card);
	if (deamon) {
		if (fd_out) {
			fflush(fd_out);
			fclose(fd_out);
		}
	}
	exit(0);
}

static void help(char *prog)
{
	printf("Usage: %s [-CvhVd] [-f file] [-t runtime] [-i delay]\n"
		"\t-C, --card <num>	Card to use (default 0)\n"
		"\t-V, --version	Print Version number\n"
		"\t-h, --help		This help message\n"
		"\t-q, --quiet		No output at all\n"
		"\t-v, --verbose	verbose mode, up to -vvv\n"
		"\t-t, --runtime <num>	Run time in sec (default 1 sec) (forground)\n"
		"\t-i, --interval <num>	Interval time in sec (default 1 sec)\n"
		"\t-d, --deamon		Start in Deamon process (background)\n"
		"\t-f, --log-file <file> Log File name when running in -d (deamon)\n",
		prog);
	return;
}

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
	bool quiet = false;
	int	card_no = 0;
	int rc;
	int ch;
	int	err_code;
	pid_t	pid;
	char *log_file = NULL;
	int delay = 1;		/* Default 10 sec delay */
	int interval = 1;	/* Default 1 sec interval */
	int mode = DDCB_MODE_WR |
		DDCB_MODE_MASTER |
		DDCB_MODE_MASTER_F_CHECK;	/* turn FIR Check on */

	fd_out = stdout;
	rc = EXIT_SUCCESS;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "card",       required_argument, NULL, 'C' },
			{ "version",    no_argument,       NULL, 'V' },
			{ "quiet",      no_argument,       NULL, 'q' },
			{ "help",       no_argument,       NULL, 'h' },
			{ "verbose",    no_argument,       NULL, 'v' },
			{ "runtime",    required_argument, NULL, 't' },
			{ "interval",   required_argument, NULL, 'i' },
			{ "deamon",     no_argument,       NULL, 'd' },
			{ "log-file",   required_argument, NULL, 'f' },
			{ 0,            no_argument,       NULL,  0  },
		};
		ch = getopt_long(argc, argv, "C:f:t:i:Vqhvd",
			long_options, &option_index);
		if (-1 == ch)
			break;
		switch (ch) {
		case 'C':	/* --card */
			card_no = strtol(optarg, (char **)NULL, 0);
			break;
		case 'V':	/* --version */
			fprintf(fd_out, "%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'q':	/* --quiet */
			quiet = true;
			break;
		case 'h':	/* --help */
			help(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'v':	/* --verbose */
			verbose_flag++;
			break;
		case 't':	/* --runtime */
			delay = strtoul(optarg, NULL, 0);
			break;
		case 'i':	/* --interval */
			interval = strtoul(optarg, NULL, 0);
			mode |= (interval << 28) & DDCB_MODE_MASTER_DT;
			break;
		case 'd':	/* --deamon */
			deamon = true;
			break;
		case 'f':	/* --log-file */
			log_file = optarg;
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (deamon) {
		if (NULL == log_file) {
			printf("Please Provide log file name (-f) if running "
				"in deamon mode !\n");
			exit(1);
		}
	}
	if (log_file) {
		fd_out = fopen(log_file, "w+");
		if (NULL == fd_out) {
			printf("Can not create/append to file %s\n", log_file);
			exit(1);
		}
	}
	signal(SIGCHLD,SIG_IGN);	/* ignore child */
	signal(SIGTSTP,SIG_IGN);	/* ignore tty signals */
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);
	signal(SIGHUP,sig_handler);	/* catch -1 hangup signal */
	signal(SIGINT, sig_handler);	/* Catch -2 */
	signal(SIGKILL,sig_handler);	/* catch -9 kill signal */
	signal(SIGTERM,sig_handler);	/* catch -15 kill signal */

	if (deamon) {
		pid = fork();
		if (pid < 0) {
			printf("Fork() failed\n");
			exit(1);
		}
		if (pid > 0) {
			printf("Child Pid is %d Parent exit here\n", pid);
			exit(0);
		}
		if (chdir("/")) {
			printf("Can not chdir to / !!!\n");
			exit(1);
		}
		umask(0);
		//set new session
		my_sid = setsid();
		printf("Child sid: %d from pid: %d\n", my_sid, pid);
		if(my_sid < 0)
			exit(1);
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}
	ddcb_debug(verbose_flag);
	if (!quiet && verbose_flag)
		fprintf(fd_out, "Open CAPI Card: %d\n", card_no);
	card = accel_open(card_no, DDCB_TYPE_CAPI, mode,
			  &err_code, 0, DDCB_APPL_ID_IGNORE);
	if (NULL == card) {
		fprintf(fd_out, "Err: failed to open Master Context for CAPI Card: %u\n",
                        card_no),
		fprintf(fd_out, "\tcheck Permissions in /dev/cxl/* or kernel log.\n");
                exit(EXIT_FAILURE);
	}

	if (false == check_app(card)) {
		fprintf(fd_out, "Err: Wrong Card Appl ID. Need to have > 0500\n");
		accel_close(card);
                exit(EXIT_FAILURE);
	}
	if (deamon) {
		while(1) {
			fprintf(fd_out, "Deamon Pid %d still running, Log file: %s\n",
				my_sid, log_file);
			sleep(60);	/* Nothing to da */
		}
	} else {
		sleep(delay);
	}
	accel_close(card);
	if (!quiet && verbose_flag)
		fprintf(fd_out, "Close Capi Card: %d\n", card_no);
	exit(rc);
}
