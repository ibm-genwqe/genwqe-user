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

/*
 * Capi Maintenence tool deamon 2
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/timeb.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <json-c/json.h>	/* Need json-c-devel.ppc64le */

#include <libddcb.h>
#include <libcxl.h>
#include "afu_regs.h"

static const char *version = GIT_VERSION;
static int verbose = 0;
static FILE *fd_out;

#define VERBOSE0(fmt, ...) do {					\
		fprintf(fd_out, fmt, ## __VA_ARGS__);		\
	} while (0)

#define VERBOSE1(fmt, ...) do {					\
		if (verbose > 0)				\
			fprintf(fd_out, fmt, ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE2(fmt, ...) do {					\
		if (verbose > 1)				\
			fprintf(fd_out, fmt, ## __VA_ARGS__);	\
	} while (0)

#define VERBOSE3(fmt, ...) do {					\
		if (verbose > 2)				\
			fprintf(fd_out, fmt, ## __VA_ARGS__);	\
	} while (0)

typedef union ivr_u {
	uint64_t reg64;
	struct {
		uint8_t build_count:4;
		uint8_t res2:4;
		uint8_t day;
		uint8_t month;
		uint8_t year;
		uint16_t freq;
		uint16_t res1;
	} data;
} IVR;

typedef union avr_u {
	uint64_t reg64;
	struct {
		uint32_t aid;		/* fix 0x475a4950 */
		uint8_t aida;		/* fix 0x03 */
		uint8_t release2;
		uint16_t release1;
	} data;
} AVR;

enum CARD_STATE {DO_CARD_OPEN,
		COLLECT_STATIC_DATA,
		COLLECT_TRANS_DATA,	/* Normal state */
		DO_CARD_CLOSE,
		CARD_CLOSED,
		CARD_FAIL};		/* Fail */

struct card_data_s {
	pthread_t tid;		/* My Thread id */
	int card;		/* -1 for not used, or 0,1,2,3 */
	struct cxl_afu_h *afu_h;/* The AFU handle */
	uint64_t wed;		/* This is a dummy only for attach */
	int run_delay;		/* Time in msec to wait */
	int open_delay;		/* Time in sec to wait if in close->open state */
	int fail_delay;		/* Time in sec to wait if in fail->open state */
	int fail_cnt;		/* Fail counter */
	char qstat[512];	/* Info for each Context */
	int act;		/* Active Contexts */
	int max_ctx;		/* Max # of Contexts */
	int card_status;	/* Status for Card */
	uint16_t release1;	/* Card Release 1 */
	uint8_t release2;	/* Card Release 2 */
	uint16_t build_year;
	uint8_t build_month;
	uint8_t build_day;
	uint8_t build_count;
	int old_frt;		/* Free run timer in msec */
	int old_wload;		/* Work Load in msec */
	int load;
};

#define	MAX_CAPI_CARDS	2
struct cgzipd_data_s {
	pthread_t servtid;	/* Server Thread ID */
	int port;		/* TCP Port number for listen */
	int delay;		/* Delay time in sec (1 sec default) */
	bool daemon;		/* TRUE if forked */
	bool quiet;		/* False or true -q option */
	pid_t pid;
	pid_t my_sid;		/* for sid */
	struct card_data_s *pcard[MAX_CAPI_CARDS];
};

struct client_data_s {
	int socket;
	struct cgzipd_data_s *cgzipd_data;
	pthread_t tid;
};

static struct cgzipd_data_s *cgzipd_data;

/*	Expect min this Release or higher */
#define	MIN_REL_VERSION	0x0601

#if 0
static int mmio_write(struct cxl_afu_h *afu_h,
		int ctx,
		uint32_t offset,
		uint64_t data)
{
	int rc = -1;
	uint32_t offs = (ctx * MMIO_CTX_OFFSET) + offset;

	VERBOSE3("[%s] Enter, Offset: 0x%x data: 0x%016llx\n",
		__func__, offs, (long long)data);
	rc = cxl_mmio_write64(afu_h, offs, data);
	VERBOSE3("[%s] Exit, rc = %d\n", __func__, rc);
	return rc;
}
#endif

static int mmio_read(struct cxl_afu_h *afu_h,
		int ctx,
		uint32_t offset,
		uint64_t *data)
{
	int rc = -1;
	uint32_t offs = (ctx * MMIO_CTX_OFFSET) + offset;

	VERBOSE3("[%s] Enter CTX: %d Offset: 0x%x\n",
		__func__, ctx, offs);
	rc = cxl_mmio_read64(afu_h, offs, data);
	VERBOSE3("[%s] Exit rc: %d data: 0x%016llx\n",
		__func__, rc, (long long)*data);
	return rc;
}

/* Return true if card Software Release is OK */
static bool check_app(struct cxl_afu_h *afu_h, uint16_t min_rel)
{
	int	rc;
	AVR	avr;

	/* Get MMIO_APP_VERSION_REG */
	rc = mmio_read(afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_APP_VERSION_REG, &avr.reg64);
	if (0 != rc)
		return false;
	if (0x475a4950 != avr.data.aid)
		return false;
	if (0x03 != avr.data.aida) 		/* Check II */
		return false;
	if (avr.data.release1 >= min_rel)	/* need >= min_rel */
		return true;
	return false;
}

/*
 * Open AFU Master Device
 */
static int card_open(struct card_data_s *cd)
{
	int rc = 0;
	char device[64];
	long api_version, cr_device, cr_vendor;
	uint64_t wed = 0;	/* Dummy */

	sprintf(device, "/dev/cxl/afu%d.0m", cd->card);
	VERBOSE1("[%s] Card: %d Open Device: %s\n",
		__func__, cd->card, device);
	cd->afu_h = cxl_afu_open_dev(device);
	if (NULL == cd->afu_h) {
		perror("cxl_afu_open_dev()");
		rc = -1;
		VERBOSE0("[%s] Card: %d Error rc: %d\n",
			__func__, cd->card, rc);
		goto err_afu_free;
	}

	/* Check if the compiled in API version is compatible with the
	   one reported by the kernel driver */
	rc = cxl_get_api_version_compatible(cd->afu_h, &api_version);
	if (rc != 0) {
		perror("cxl_get_api_version_compatible()");
		rc = -2;
		goto err_afu_free;
	}
	if (api_version != CXL_KERNEL_API_VERSION) {
		VERBOSE0(" [%s] Card: %d ERR: incompatible API version: %ld/%d\n",
			 __func__, cd->card, api_version, CXL_KERNEL_API_VERSION);
		rc = -2;
		goto err_afu_free;
	}

	/* Check vendor id */
	rc = cxl_get_cr_vendor(cd->afu_h, 0, &cr_vendor);
	if (rc != 0) {
		perror("cxl_get_cr_vendor()");
		rc = -3;
		goto err_afu_free;
	}
	if (cr_vendor != CGZIP_CR_VENDOR) {
		VERBOSE0(" [%s] Card: %d ERR: vendor_id: %ld/%dn",
			 __func__, cd->card, (unsigned long)cr_vendor,
			 CGZIP_CR_VENDOR);
		rc = -3;
		goto err_afu_free;
	}

	/* Check device id */
	rc = cxl_get_cr_device(cd->afu_h, 0, &cr_device);
	if (rc != 0) {
		perror("cxl_get_cr_device()");
		rc = -4;
		goto err_afu_free;
	}
	if (cr_device != CGZIP_CR_DEVICE) {
		VERBOSE0(" [%s] Card: %d ERR: device_id: %ld/%d\n",
			 __func__, cd->card, (unsigned long)cr_device,
			 CGZIP_CR_VENDOR);
		rc = -4;
		goto err_afu_free;
	}

	rc = cxl_afu_attach(cd->afu_h,
		(__u64)(unsigned long)(void *)&wed);
	if (0 != rc) {
		perror("cxl_afu_attach()");
		rc = -6;
		goto err_afu_free;
	}

	rc = cxl_mmio_map(cd->afu_h, CXL_MMIO_BIG_ENDIAN);
	if (rc != 0) {
		perror("cxl_mmio_map()");
		cxl_afu_free(cd->afu_h);
		rc = -7;
		goto err_afu_free;
	}
	if (false == check_app(cd->afu_h, MIN_REL_VERSION)) {
		VERBOSE0("[%s] Card: %d Err: Wrong Card Release. Need >= 0x%02x\n",
			__func__, cd->card, MIN_REL_VERSION);
		cxl_mmio_unmap(cd->afu_h);
		cxl_afu_free(cd->afu_h);
		rc = -8;
	}

 err_afu_free:
	if (0 != rc)
		cd->afu_h = NULL;
	VERBOSE1("[%s] Card: %d Exit rc: %d handle: %p\n",
		__func__, cd->card, rc, cd->afu_h);
	return rc;
}

static int card_close(struct cxl_afu_h *afu_h)
{
	VERBOSE1("[%s] Enter\n", __func__);
	if (NULL == afu_h)
		return -1;

	cxl_mmio_unmap(afu_h);
	cxl_afu_free(afu_h);
	VERBOSE1("[%s] Exit\n", __func__);
	return 0;
}

static uint8_t hex2dec(uint8_t hex)
{
	uint8_t b1, b2;

	b1 = hex & 0xf;
	b2 = (hex >> 4) & 0xf;
	return (b2*10+b1);
}

/*
 * Function:	collect_static_data()
 * 		Parms: card_data_s Ptr to card data
 * 		Updates Version and some other data from card
 * 		Collecting Static data.
 */
static int collect_static_data(struct card_data_s *cd)
{
	int rc;
	IVR ivr;
	AVR avr;

	VERBOSE1("[%s] Enter Card: %d\n", __func__, cd->card);
	rc = mmio_read(cd->afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_APP_VERSION_REG, &avr.reg64);
	if (0 == rc) {
		cd->release1 = avr.data.release1;
		cd->release2 = avr.data.release2;
		rc = mmio_read(cd->afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_IMP_VERSION_REG, &ivr.reg64);
		if (0 == rc) {
			cd->build_year = (uint16_t)hex2dec(ivr.data.year) + 2000;
			cd->build_month = hex2dec(ivr.data.month);
			cd->build_day = hex2dec(ivr.data.day);
			cd->build_count = ivr.data.build_count;
		} else {
			cd->build_year = 0;
			cd->build_month = 0;
			cd->build_day = 0;
			cd->build_count  = 0;
		}
	} else	{
		cd->release1 = 0xdead;
		cd->release2 = 0xff;
	}
	VERBOSE1("[%s] Card: %d Exit rc: %d\n", __func__, cd->card, rc);
	return rc;
}

/*
 * Function:	collect_trans_data()
 * 	Parms: card_data_s Ptr to card data
 * 	Updates qstat array for all contexts
 * 	Collecting transient data.
 */
static int collect_trans_data(struct card_data_s *cd)
{
	int	gsel, bsel = 0, ctx = 0, rc = 0;
	uint64_t gmask = 0, qstat_reg;
	uint16_t cseq, lseq, qnfe;
	uint8_t qstat;
	char flag;
	int act = 0;
	uint64_t new_wload, new_frt;
	int wload, frt;

	memset(cd->qstat, 'N', 512);			/* State: Not Active */
	for (gsel = 0; gsel < MMIO_CASV_REG_NUM; gsel++) {
		mmio_read(cd->afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_CASV_REG + (gsel * 8), &gmask);
		if (0 == gmask)
			continue;		/* No bit set, Skip */

		for (bsel = 0; bsel < MMIO_CASV_REG_CTX; bsel++) {
			if (0 == (gmask & (1ull << bsel)))
				continue;	/* Skip */

			ctx = (gsel * MMIO_CASV_REG_CTX) + bsel;	/* Active */

			rc = mmio_read(cd->afu_h, ctx+1, MMIO_DDCBQ_STATUS_REG, &qstat_reg);
			if (0 == (qstat_reg & 0xffffffff00000000ull)) {
				VERBOSE3("AFU[%d:%03d] master skip\n",
					cd->card, ctx);
				cd->qstat[ctx] = 'M';	/* State: I am Master */
				act++;
				continue;	/* Skip Master */
			}
			cseq = (uint16_t)(qstat_reg >> 48ull);	/* Currect sequence */
			lseq = (uint16_t)(qstat_reg >> 32ull);	/* Last sequence */
			qnfe = (uint16_t)(qstat_reg >> 8);	/* Context Non Fatal Error Bits */
			qstat = (uint8_t)(qstat_reg & 0xff);	/* Context Status */

			/* Generate W for Waiting, I for Idle and R for Running */
			flag = 'W';			/* State: Waiting to get executed */
			if ((lseq + 1 ) == cseq)
				flag = 'I';		/* State: Idle, nothing to do */
			else if (0x30 == qstat)		/* if Bits 4 + 5 on ? */
					flag = 'R';	/* State: Running */
			if (qnfe)
				flag = 'E';		/* Error */
			cd->qstat[ctx] = flag;
			act++;
			VERBOSE3("[%s] Card: %d CTX: %d Status: %c\n", __func__, cd->card, ctx, flag);
		}
	}
	cd->max_ctx = ctx + 1;	/* Save Latest context i found */
	cd->act = act;		/* Save the number of active contexts */
	/* Get the Workload */
	cd->load = 0;		/* Clear */
	mmio_read(cd->afu_h, MMIO_MASTER_CTX_NUMBER,
		0x90, &new_wload);
	if (-1ull != new_wload) {
		mmio_read(cd->afu_h, MMIO_MASTER_CTX_NUMBER,
			MMIO_FRT_REG, &new_frt);
		new_wload = (int)(new_wload / 250000);	/* in msec now */
		wload = new_wload - cd->old_wload;	/* Delta */
		cd->old_wload = new_wload;		/* Save old */

		new_frt = (int)(new_frt / 250000);	/* in msec now */
		frt = new_frt - cd->old_frt;		/* Delta */
		cd->old_frt = new_frt;			/* Save old */
		cd->load = (wload *100) / frt;
		VERBOSE2("[%s] Card %d Work Load: %d%%\n",
			__func__, cd->card, cd->load);
	}
	return rc;
}

/*
 * Function: card_thread()
 * 	Parms: Ptr to card_data
 * 	Execute Card State Machine
 */
static void *card_thread(void *data)
{
	struct card_data_s *cd = (struct card_data_s*)data;
	int rc = 0;
	int delay;
	enum CARD_STATE state = DO_CARD_OPEN;
	bool execute_sm = true;	/* Execute State Machine */

	VERBOSE1("[%s] Enter Card: %d\n", __func__, cd->card);
	while (execute_sm) {
		delay = cd->run_delay;
		VERBOSE2("[%s] Card: %d Current State: %d\n",
			__func__, cd->card, state);
		switch(state) {
		case DO_CARD_OPEN:
			if (0 == card_open(cd)) {
				state = COLLECT_STATIC_DATA;
				delay = 0;	/* Continue */
				cd->fail_cnt = 0;
			} else	{
				delay = cd->open_delay;
				cd->fail_cnt++;
				if (2 == cd->fail_cnt)
					state = CARD_FAIL;
			}
			break;
		case COLLECT_STATIC_DATA:
			if (0 == collect_static_data(cd)) {
				state = COLLECT_TRANS_DATA;
				delay = 200;
			} else {
				state = DO_CARD_CLOSE;
				delay = cd->fail_delay;
			}
			break;
		case COLLECT_TRANS_DATA:	/* Normal State */
			rc = collect_trans_data(cd);
			if (0 != rc)
				state = DO_CARD_CLOSE;
			break;
		case DO_CARD_CLOSE:
			rc = card_close(cd->afu_h);
			if (0 == rc) {
				state = CARD_CLOSED;
				cd->fail_cnt = 0;
			} else {
				delay = cd->fail_delay;
				cd->fail_cnt++;
				if (2 == cd->fail_cnt)
					state = CARD_FAIL;
			}
			break;
		case CARD_CLOSED:
			state = DO_CARD_OPEN;
			delay = cd->open_delay;
			break;
		case CARD_FAIL:
			VERBOSE0("[%s] Card: %d FAIL\n", __func__, cd->card);
			memset(cd->qstat, 'E', 512);	/* State: Error */
			state = CARD_FAIL;
			delay = cd->fail_delay;		/* delay 20 sec */
			execute_sm = false;		/* Terminate */
			rc = 1;
			break;
		}
		cd->card_status = (int)state;
		VERBOSE2("[%s] Card: %d Next State: %d Delay: %d msec\n",
			__func__, cd->card, state, delay);
		usleep(delay * 1000);
	}
	VERBOSE0("[%s] Card: %d Exit rc: %d\n",
		__func__, cd->card, rc);
	pthread_exit(&rc);
}

static struct card_data_s *create_card_thread(
		struct  cgzipd_data_s *cg,
		int card)
{
	int rc = 0;
	pthread_t tid;
	struct card_data_s *pc = NULL;

	VERBOSE1("[%s] Enter Card: %d delay: %d\n",
		__func__, card, cg->delay);
	cg->pcard[card] = NULL;
	pc = malloc(sizeof(struct card_data_s));
	if (NULL == pc)
		goto create_card_thread_exit;
	pc->card = card;
	pc->run_delay = cg->delay;	/* form -i Parm */
	pc->fail_delay = 20000;		/* 20 sec */
	pc->open_delay = 10000;		/* 10 sec */
	pc->wed = 0;
	pc->afu_h = NULL;
	rc = pthread_create(&tid, NULL, card_thread, pc);
	if (0 != rc) {
		free(pc);
		pc = NULL;
	} else  pc->tid = tid;
create_card_thread_exit:
	VERBOSE1("[%s] Exit pc: %p\n", __func__, pc);
	return pc;
}


static void do_exit(struct cgzipd_data_s *cg)
{
	struct card_data_s *pc;
	void *res = NULL;

	pc = cg->pcard[0];
	if (pc->tid) {
		VERBOSE1("[%s] Wait for Card 0 Thread\n",
			__func__);
		pthread_cancel(pc->tid);
		pthread_join(pc->tid, &res);
		pc->tid = 0;
	}
	pc = cg->pcard[1];
	if (pc->tid) {
		VERBOSE1("[%s] Wait for Card 1 Thread\n",
			__func__);
		pthread_cancel(pc->tid);
		pthread_join(pc->tid, &res);
		pc->tid = 0;
	}

	VERBOSE1("[%s] Free Card's\n", __func__);
	if (cg->pcard[0])
		free(cg->pcard[0]);
	if (cg->pcard[1])
		free(cg->pcard[1]);
	return;
}

static void sig_handler(int sig)
{
	struct cgzipd_data_s *cg = cgzipd_data;

	VERBOSE0("Sig Handler Signal: %d %p\n", sig, cg);
	do_exit(cg);
	fflush(fd_out);
	fclose(fd_out);
	VERBOSE0("Sig Handler Exit\n");
	exit(EXIT_SUCCESS);
}

static void json_object_add_card(json_object *jobj,
		struct card_data_s *card_data)
{
	char msg[1024];

	json_object *jcard = json_object_new_object();
	if (0 == card_data->card)
		json_object_object_add(jobj, "card0", jcard);
	else	json_object_object_add(jobj, "card1", jcard);
	memcpy(msg, card_data->qstat, card_data->max_ctx);
	msg[card_data->max_ctx] = 0;
	json_object *jctx = json_object_new_string(msg);
	json_object_object_add(jcard, "ctx", jctx);
	json_object *jstatus = json_object_new_int(card_data->card_status);
	json_object_object_add(jcard, "status", jstatus);
	sprintf(msg, "%04x:%x",
		card_data->release1, card_data->release2);
	json_object *jver = json_object_new_string(msg);
	json_object_object_add(jcard, "fpga-build", jver);
	sprintf(msg, "%d-%d-%d (Build# %d)",
		card_data->build_year, card_data->build_month,
		card_data->build_day, card_data->build_count);
	json_object *jdate = json_object_new_string(msg);
	json_object_object_add(jcard, "fpga-build-date", jdate);
	json_object *jact = json_object_new_int(card_data->act);
	json_object_object_add(jcard, "attached", jact);
	json_object *jload = json_object_new_int(card_data->load);
	json_object_object_add(jcard, "load", jload);
	return;
}

/* Child Process */
static void *ServNewSock(void *args)
{
	int rc = 0;
	int len;
	char msg[2048];
	char hostname[256];
	struct client_data_s *client_data = (struct client_data_s*)args;
	int sock = client_data->socket;
	struct cgzipd_data_s *cg = client_data->cgzipd_data;
	struct card_data_s *card_data;	/* For Card Data */
	struct  timeb now;
	uint64_t now_ms;

	VERBOSE1("[%s] Enter Sock: %d\n", __func__, sock);

	pthread_detach(pthread_self());
	hostname[255] = 0;
	gethostname(hostname, 256);
	struct hostent *h = gethostbyname(hostname);
	json_object *jobj = json_object_new_object();
	while (1) {
		/* Add Hostname */
		json_object *jhost = json_object_new_string(h->h_name);
		json_object_object_add(jobj, "host", jhost);
		/* Add Time */
		ftime(&now);
		now_ms = now.time * 1000 + now.millitm;
		json_object *jts = json_object_new_int64(now_ms);
		json_object_object_add(jobj,"ts", jts);
		/* Card 0 */
		card_data = cg->pcard[0];
		json_object_add_card(jobj, cg->pcard[0]);
		/* Card 1 */
		card_data = cg->pcard[1];
		json_object_add_card(jobj, card_data);

		len = snprintf(msg, 2048, "%s\n",
			json_object_to_json_string(jobj));
		VERBOSE1("[%s] write(%d, JSON , %d)\n", __func__, sock,  len);
		rc = send(sock, msg, len, MSG_CONFIRM);
		usleep(cg->delay * 1000);
	}
	json_object_put(jobj);
	VERBOSE1("[%s] EXIT rc: %d Sock: %d\n", __func__, rc, sock);
	close(sock);
	free(client_data);
	pthread_exit(NULL);
}

static void *SockServ(void *args)
{
	int rc = 0;
	int ServSock, NewSock;
	struct sockaddr_in server, client;
	pthread_t CliTid;
	int sinSize;
	struct cgzipd_data_s *cg = (struct cgzipd_data_s*)args;
	struct client_data_s *client_data;
	int yes = 1;

	VERBOSE1("[%s] Enter On Port: %d\n", __func__, cg->port);
	ServSock = socket(AF_INET , SOCK_STREAM , 0);
	if(ServSock < 0) {
		VERBOSE0("[%s] ERROR: socket() failed\n", __func__);
		rc = 1;
		goto __exit_server_thread1;
	}
	setsockopt(ServSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
	//Prepare the sockaddr_in structure
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(cg->port);
	if (bind(ServSock,
			(struct sockaddr *)&server ,
			sizeof(server)) < 0) {
		VERBOSE0("[%s] bind() failed\n", __func__);
		close(ServSock);
		goto __exit_server_thread1;
	}
	VERBOSE1("[%s] listen(%d, ..)\n", __func__, ServSock);
	if (listen(ServSock, 5) != 0) {
		VERBOSE0("[%s] listen(%d, ..) failed\n", __func__, ServSock);
		close(ServSock);
		goto __exit_server_thread1;
	}

	sinSize = sizeof(struct sockaddr_in);
	while (0 == rc) {
		VERBOSE1("[%s] Wait for accept(%d....)\n",
			__func__, ServSock);
		NewSock = accept(ServSock,
				(struct sockaddr *)&client,
				(socklen_t*)&sinSize);
		VERBOSE1("[%s] Accept on Sock: %d from: %s\n",
			__func__, NewSock, inet_ntoa(client.sin_addr));
		if (NewSock < 0) {
			VERBOSE0("[%s] ERROR on accept(%d...)\n", __func__, ServSock);
			continue;
		}
		client_data = malloc(sizeof(struct client_data_s));
                client_data->socket = NewSock;
                client_data->cgzipd_data = cg;
                rc = pthread_create(&CliTid, NULL, ServNewSock, client_data);
                if (0 != rc) {
                        VERBOSE0("[%s] Can not create worker thread", __func__);
                        free(client_data);
                        close(NewSock);
                } else
			client_data->tid = CliTid;	/* Save Tid */
	}
	close(ServSock);
__exit_server_thread1:
	VERBOSE1("[%s] Exit rc: %d\n", __func__, rc);
	pthread_exit(NULL);
}

static void help(char *prog)
{
	printf("NAME\n\n");
	printf("SYNOPSIS\n      %s [OPTION]\n\n", prog);
	printf("DESCRIPTION\n");
	printf("       Debug Tool to gather informations for CAPI Gzip Cards.\n");
	printf("\t-p, --port <num>	tcp port to listen (default is 6000)\n"
	       "\t-V, --version         Print Version number\n"
	       "\t-h, --help		This help message\n"
	       "\t-q, --quiet		No output at all\n"
	       "\t-v, --verbose         verbose mode, up to -vvv\n"
	       "\t-i, --interval <num>	Poll Interval in msec (default 1000 msec)\n"
	       "\t-d, --daemon		Start in Daemon mode (forked)\n"
	       "\t-f, --log-file <file> Log File name when running in -d "
	       "(daemon)\n");
	printf("Example how to use:\n");
	printf("\tStart: <%s -vv> in a terminal 1\n", prog);
	printf("\tStart: <telnet localhost 6000> in terminal 2.\n");
	printf("\tThe Terminal window will print a JSON string with following informations\n\n");
	printf("\t host: <the hostname where <%s> is running\n", prog);
	printf("\t ts:    Host time stamp in msec (now)\n");
	printf("\t card0: Info for CAPI Gzip Card 0\n");
	printf("\t   ctx: <MEIRW...> for all context ids (up to 512) for this card\n");
	printf("\t         M=Master. E=Error, I=IDLE, R=Running, W=Waiting\n");
	printf("\t   status:          5=error, 2=normal\n");
	printf("\t   fpga-build:      string for build number\n");
	printf("\t   fpga-build-date: string for build date\n");
	printf("\t   attached: Number of howm nay context id are in ctx list\n");
	printf("\t   load:     Average load for GZIP Card.\n");
	printf("\t card1: Info for CAPI Gzip Card 1\n");
	printf("\t        Same data as for Card 0\n");
}

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
	int rc = EXIT_SUCCESS;
	int ch;
	char *log_file = NULL;
	struct cgzipd_data_s *cg;
	struct card_data_s *pc;
	sigset_t new;

	cg = malloc(sizeof(struct cgzipd_data_s));
	if (NULL == cg) {
		printf("[%s] Can not malloc data\n", argv[0]);
		exit(1);
	}
	cgzipd_data = cg;	/* Save to Global */
	fd_out = stdout;	/* Default */

	cg->port = 6000;	/* default port to listen */
	cg->quiet = false;	/* Default */
	cg->delay = 1000;	/* Default, 1000 msec delay time */
	cg->daemon = false;	/* Not in Daemon mode */

	rc = EXIT_SUCCESS;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{ "port",	required_argument, NULL, 'p' },
			{ "version",	no_argument,	   NULL, 'V' },
			{ "quiet",	no_argument,	   NULL, 'q' },
			{ "help",	no_argument,	   NULL, 'h' },
			{ "verbose",	no_argument,	   NULL, 'v' },
			{ "interval",	required_argument, NULL, 'i' },
			{ "daemon",	no_argument,	   NULL, 'd' },
			{ "log-file",	required_argument, NULL, 'f' },
			{ 0,		0,		   NULL,  0  }
		};
		ch = getopt_long(argc, argv, "p:f:i:Vqhvd",
			long_options, &option_index);
		if (-1 == ch)
			break;
		switch (ch) {
		case 'p':	/* Listen on tcp Port */
			cg->port = strtoul(optarg, NULL, 0);
			break;
		case 'V':	/* --version */
			printf("%s\n", version);
			exit(EXIT_SUCCESS);
			break;
		case 'q':	/* --quiet */
			cg->quiet = true;
			break;
		case 'h':	/* --help */
			help(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		case 'v':	/* --verbose */
			verbose++;
			break;
		case 'i':	/* --interval */
			cg->delay = strtoul(optarg, NULL, 0);
			break;
		case 'd':	/* --daemon */
			cg->daemon = true;
			break;
		case 'f':	/* --log-file */
			log_file = optarg;
			break;
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (cg->daemon) {
		if (NULL == log_file) {
			fprintf(stderr, "Please Provide log file name (-f) "
				"if running in daemon mode !\n");
			exit(EXIT_FAILURE);
		}
	}
	if (log_file) {
		fd_out = fopen(log_file, "w+");
		if (NULL == fd_out) {
			fprintf(stderr, "Can not create/append to file %s\n",
				log_file);
			exit(EXIT_FAILURE);
		}
	}
	signal(SIGCHLD,SIG_IGN);	/* ignore child */
	signal(SIGTSTP,SIG_IGN);	/* ignore tty signals */
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);
	signal(SIGHUP,sig_handler);	/* catch -1 hangup signal */
	signal(SIGINT, sig_handler);	/* Catch -2 */
	signal(SIGTERM,sig_handler);	/* catch -15 kill signal */

	if (cg->daemon) {
		cg->pid = fork();
		if (cg->pid < 0) {
			printf("[%s] Fork() failed\n",argv[0]);
			exit(EXIT_FAILURE);
		}
		if (cg->pid > 0) {
			printf("[%s] Child Pid is %d Parent exit here\n",
			       argv[0], cg->pid);
			exit(EXIT_SUCCESS);
		}
		if (chdir("/")) {
			fprintf(stderr, "Can not chdir to / !!!\n");
			exit(EXIT_FAILURE);
		}
		umask(0);
		/* set new session */
		cg->my_sid = setsid();
		printf("[%s] Child sid: %d from pid: %d\n",
		       argv[0], cg->my_sid, cg->pid);

		if(cg->my_sid < 0)
			exit(EXIT_FAILURE);

		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	/* Create 2 Cards */
	pc = create_card_thread(cg, 0);
	if (pc) {
		cg->pcard[0] = pc;
		pc = create_card_thread(cg, 1);
		if (pc)
			cg->pcard[1] = pc;
		else cg->pcard[1] = NULL;
	} else cg->pcard[0] = NULL;

	rc = cxl_mmio_install_sigbus_handler();
	if (rc != 0) {
		VERBOSE0("Err: Install cxl sigbus_handler rc=%d\n", rc);
		goto main_exit;
	}

	sigemptyset (&new);
        sigaddset(&new, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &new, NULL) != 0) {
		VERBOSE0("Unable to mask SIGPIPE");
		goto main_exit;
        }
	if (pthread_create(&cg->servtid, NULL, SockServ, cg) < 0) {
		VERBOSE0("Could not create server thread");
		goto main_exit;
        }
	VERBOSE1("[%s] Wait for Join\n", __func__);
        pthread_join(cg->servtid, NULL);

main_exit:
	do_exit(cg);

	fflush(fd_out);
	fclose(fd_out);
	if (cg)
		free(cg);
	exit(rc);
}
