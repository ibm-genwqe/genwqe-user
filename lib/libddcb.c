/*
 * Copyright 2014,2015 International Business Machines
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
 * Generic support to enqueue DDCB commands.
 *
 * Might be a cool thing if we could use libcard.h directly as
 * absorber. Need to do more experiments to figure out if that is
 * possible to limit changes we need to perform to get in a CAPI
 * capable version.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#define _GNU_SOURCE
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <asm/byteorder.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dirent.h>

#ifndef CONFIG_DONT_USE_INOTIFY
#include <sys/inotify.h>
#endif

#include <libddcb.h>

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(a)  (sizeof((a)) / sizeof((a)[0]))
#endif

#ifndef ABS
#  define ABS(a)	 (((a) < 0) ? -(a) : (a))
#endif

/* This is the internal structure for each Stream */
struct card_dev_t {
	int card_no;		/* card id: FIXEM do we need card_dev? */
	int card_type;		/* type of card: GenWQE, CAPI, CAPIsim, ... */
	int mode;
	void *card_data;	/* private data from underlying layer */
	int card_rc;		/* return code from lower level */
	int card_errno;		/* errno from lower level */
	struct ddcb_accel_funcs *accel;	 /* supported set of functions */
};

static struct ddcb_accel_funcs *accel_list = NULL;
int libddcb_verbose = 0;

static struct ddcb_accel_funcs *find_accelerator(int card_type)
{
	struct ddcb_accel_funcs *accel;

	for (accel = accel_list; accel != NULL; accel = accel->priv_data) {
		if (accel->card_type == card_type)
			return accel;
	}
	return NULL;
}

static const char * const retc_errlist[] = {
	[ABS(DDCB_RETC_IDLE)] = "unexecuted/untouched DDCB",
	[ABS(DDCB_RETC_PENDING)] = "pending execution",
	[ABS(DDCB_RETC_COMPLETE)] = "command complete. no error",
	[ABS(DDCB_RETC_FAULT)] =
	"application error, recoverable, please see ATTN and PROGR",
	[ABS(DDCB_RETC_ERROR)] =
	"application error, non-recoverable, please see ATTN and PROGR",
	[ABS(DDCB_RETC_FORCED_ERROR)] = "overwritten by driver",
	[ABS(DDCB_RETC_UNEXEC)] = "unexecuted/removed from queue",
	[ABS(DDCB_RETC_TERM)] = "terminated",
};

static const int retc_nerr __attribute__((unused)) = ARRAY_SIZE(retc_errlist);

const char *ddcb_retc_strerror(int errnum)
{
	if (ABS(errnum) >= retc_nerr)
		return "unknown error code";
	return retc_errlist[ABS(errnum)];
}

static const char * const ddcb_errlist[] = {
	[ABS(DDCB_ERRNO)] = "libc call went wrong",
	[ABS(DDCB_ERR_CARD)] = "problems accessing accelerator",
	[ABS(DDCB_ERR_OPEN)] = "cannot open accelerator",
	[ABS(DDCB_ERR_VERS_MISMATCH)] = "library version mismatch",
	[ABS(DDCB_ERR_INVAL)] = "illegal parameters",
	[ABS(DDCB_ERR_EXEC_DDCB)] = "ddcb execution failed",
	[ABS(DDCB_ERR_APPID)] ="application id wrong",
	[ABS(DDCB_ERR_NOTIMPL)] = "function not implemented",
	[ABS(DDCB_ERR_ENOMEM)] = "out of memory",
	[ABS(DDCB_ERR_ENOENT)] = "entry not found",
	[ABS(DDCB_ERR_IRQTIMEOUT)] = "timeout waiting on irq event",
	[ABS(DDCB_ERR_EVENTFAIL)] = "failed waiting on expected event",
};

static const int ddcb_nerr __attribute__((unused)) = ARRAY_SIZE(ddcb_errlist);

const char *ddcb_strerror(int errnum)
{
	if (ABS(errnum) >= ddcb_nerr)
		return "unknown error code";
	return ddcb_errlist[ABS(errnum)];
}

void ddcb_hexdump(FILE *fp, const void *buff, unsigned int size)
{
	unsigned int i;
	const uint8_t *b = (uint8_t *)buff;
	char ascii[17];
	char str[2] = { 0x0, };

	for (i = 0; i < size; i++) {
		if ((i & 0x0f) == 0x00) {
			fprintf(fp, " %08x:", i);
			memset(ascii, 0, sizeof(ascii));
		}
		fprintf(fp, " %02x", b[i]);
		str[0] = isalnum(b[i]) ? b[i] : '.';
		str[1] = '\0';
		strncat(ascii, str, sizeof(ascii) - 1);

		if ((i & 0x0f) == 0x0f)
			fprintf(fp, " | %s\n", ascii);
	}

	/* print trailing up to a 16 byte boundary. */
	for (; i < ((size + 0xf) & ~0xf); i++) {
		fprintf(fp, "   ");
		str[0] = ' ';
		str[1] = '\0';
		strncat(ascii, str, sizeof(ascii) - 1);

		if ((i & 0x0f) == 0x0f)
			fprintf(fp, " | %s\n", ascii);
	}

	fprintf(fp, "\n");
}

void ddcb_debug(int verbosity)
{
	libddcb_verbose = verbosity;
}

accel_t accel_open(int card_no, unsigned int card_type,
		   unsigned int mode, int *err_code,
		   uint64_t appl_id, uint64_t appl_id_mask)
{
	int rc = DDCB_OK;
	struct card_dev_t *card;
	struct ddcb_accel_funcs *accel;

	card = calloc(1, sizeof(*card));
	if (card == NULL) {
		rc = DDCB_ERR_ENOMEM;
		goto err_out;
	}

	accel = find_accelerator(card_type);
	if (accel == NULL) {
		rc = DDCB_ERR_ENOENT;
		goto err_free;
	}

	card->card_no = card_no;
	card->card_type = card_type;
	card->mode = mode;
	card->accel = accel;

	if (card->accel->card_open == NULL) {
		rc = DDCB_ERR_NOTIMPL;
		goto err_free;
	}

	card->card_data = card->accel->card_open(card_no, mode, &card->card_rc,
						 appl_id, appl_id_mask);
	if (card->card_data == NULL) {
		rc = DDCB_ERR_CARD;
		goto err_free;
	}
	if (err_code)
		*err_code = DDCB_OK;
	return card;

 err_free:
	free(card);
 err_out:
	if (err_code)
		*err_code = rc;
	return NULL;
}

int accel_close(accel_t card)
{
	int rc;
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_close == NULL)
		return DDCB_ERR_NOTIMPL;

	rc = accel->card_close(card->card_data);
	free(card);

	return rc;
}

const char *accel_strerror(accel_t card, int card_rc)
{
	struct ddcb_accel_funcs *accel;

	if (card == NULL)
		return "invalid accelerator";

	accel = card->accel;
	if (accel == NULL)
		return "invalid accelerator";

	if (accel->card_strerror == NULL)
		return NULL;

	return accel->card_strerror(card->card_data, card_rc);

}

int accel_ddcb_execute(accel_t card, struct ddcb_cmd *req,
		 int *card_rc, int *card_errno)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->ddcb_execute == NULL)
		return DDCB_ERR_NOTIMPL;

	card->card_rc = accel->ddcb_execute(card->card_data, req);
	card->card_errno = errno;

	if (card_rc != NULL)
		*card_rc = card->card_rc;
	if (card_errno != NULL)
		*card_errno = card->card_errno;
	if (card->card_rc < 0)
		return DDCB_ERR_CARD;

	return DDCB_OK;
}

uint64_t accel_read_reg64(accel_t card, uint32_t offs, int *card_rc)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL) {
		if (card_rc != NULL)
			*card_rc = DDCB_ERR_INVAL;
		return 0;
	}

	if (accel->card_read_reg64 == NULL) {
		if (card_rc != NULL)
			*card_rc = DDCB_ERR_NOTIMPL;
		return 0;
	}

	return accel->card_read_reg64(card->card_data, offs, card_rc);
}

uint32_t accel_read_reg32(accel_t card, uint32_t offs, int *card_rc)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL) {
		if (card_rc != NULL)
			*card_rc = DDCB_ERR_INVAL;
		return 0;
	}

	if (accel->card_read_reg32 == NULL) {
		if (card_rc != NULL)
			*card_rc = DDCB_ERR_NOTIMPL;
		return 0;
	}

	return accel->card_read_reg32(card->card_data, offs, card_rc);
}

int accel_write_reg64(accel_t card, uint32_t offs, uint64_t val)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_write_reg64 == NULL)
		return DDCB_ERR_NOTIMPL;

	return accel->card_write_reg64(card->card_data, offs, val);
}

int accel_write_reg32(accel_t card, uint32_t offs, uint32_t val)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_write_reg32 == NULL)
		return DDCB_ERR_NOTIMPL;

	return accel->card_write_reg32(card->card_data, offs, val);
}

uint64_t accel_get_app_id(accel_t card)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_get_app_id == NULL)
		return DDCB_ERR_NOTIMPL;

	return accel->card_get_app_id(card->card_data);
}

int accel_pin_memory(accel_t card, const void *addr, size_t size,
			 int dir)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_write_reg32 == NULL)
		return DDCB_ERR_NOTIMPL;

	return accel->card_pin_memory(card->card_data, addr, size, dir);
}

int accel_unpin_memory(accel_t card __attribute__((unused)),
			   const void *addr __attribute__((unused)),
			   size_t size __attribute__((unused)))
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_unpin_memory == NULL)
		return DDCB_ERR_NOTIMPL;

	return accel->card_unpin_memory(card->card_data, addr, size);
}

void *accel_malloc(accel_t card, size_t size)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return NULL;

	if (accel->card_malloc == NULL)
		return NULL;

	return accel->card_malloc(card->card_data, size);
}

int accel_free(accel_t card, void *ptr, size_t size)
{
	struct ddcb_accel_funcs *accel = card->accel;

	if (accel == NULL)
		return DDCB_ERR_INVAL;

	if (accel->card_free == NULL)
		return DDCB_ERR_NOTIMPL;

	return accel->card_free(card->card_data, ptr, size);
}

int ddcb_register_accelerator(struct ddcb_accel_funcs *accel)
{
	if (accel == NULL)
		return DDCB_ERR_INVAL;

	accel->priv_data = accel_list;
	accel_list = accel;
	return DDCB_OK;
}
