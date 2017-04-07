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
 * Specialized DDCB execution implementation.
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

#include <libddcb.h>		/* outside interface */
#include <libcard.h>		/* internal implementation */

static void *card_open(int card_no, unsigned int mode, int *card_rc,
		       uint64_t appl_id, uint64_t appl_id_mask)
{
	return genwqe_card_open(card_no, mode, card_rc, appl_id, appl_id_mask);
}

static int card_close(void *card_data)
{
	return genwqe_card_close(card_data);
}

static int ddcb_execute(void *card_data, struct ddcb_cmd *req)
{
	return genwqe_card_execute_ddcb(card_data,
					(struct genwqe_ddcb_cmd *)req);
}

static const char *_card_strerror(void *card_data __attribute__((unused)),
				  int card_rc)
{
	return card_strerror(card_rc);
}

static uint64_t card_read_reg64(void *card_data, uint32_t offs,
			     int *card_rc)
{
	return genwqe_card_read_reg64(card_data, offs, card_rc);
}

static uint32_t card_read_reg32(void *card_data, uint32_t offs,
			     int *card_rc)
{
	return genwqe_card_read_reg32(card_data, offs, card_rc);
}

static int card_write_reg64(void *card_data, uint32_t offs,
			 uint64_t val)
{
	return genwqe_card_write_reg64(card_data, offs, val);
}

static int card_write_reg32(void *card_data, uint32_t offs, uint32_t val)
{
	return genwqe_card_write_reg32(card_data, offs, val);
}

static uint64_t _card_get_app_id(void *card_data)
{
	return card_get_app_id(card_data);
}

static uint64_t _card_get_frequency(void *card_data)
{
	uint16_t speed; /*		   MHz	MHz  MHz  MHz */
	static const int speed_grade[] = { 250, 200, 166, 175 };
	uint64_t slu_unitcfg;

	slu_unitcfg = card_read_reg64(card_data, IO_SLU_UNITCFG, NULL);
	speed = (uint16_t)((slu_unitcfg >> 28) & 0x0full);
	if (speed >= ARRAY_SIZE(speed_grade))
		return 0;       /* illegal value */

	return speed_grade[speed] * (uint64_t)1000000;  /* in Hz */
}

static void card_dump_hardware_version(void *card_data, FILE *fp)
{
	uint64_t slu_unitcfg;
	uint64_t app_unitcfg;

	slu_unitcfg = card_read_reg64(card_data, IO_SLU_UNITCFG, NULL);
	app_unitcfg = card_read_reg64(card_data, IO_APP_UNITCFG, NULL);

	fprintf(fp,
		" Version Reg:        0x%016llx\n"
		" Appl. Reg:          0x%016llx\n",
		(long long)slu_unitcfg, (long long)app_unitcfg);
}

/**
 * Special formular is required to get the right time for our GenWQE
 * implementation.
 */
static uint64_t _card_get_queue_work_time(void *card_data)
{
	uint64_t queue_wtime;

	queue_wtime = card_read_reg64(card_data, IO_SLC_QUEUE_WTIME, NULL);
	return queue_wtime * 8;
}

static int card_pin_memory(void *card_data, const void *addr, size_t size,
			   int dir)
{
	return genwqe_pin_memory(card_data, addr, size, dir);
}

static int card_unpin_memory(void *card_data, const void *addr, size_t size)
{
	return genwqe_unpin_memory(card_data, addr, size);
}

static void *card_malloc(void *card_data, size_t size)
{
	return genwqe_card_malloc(card_data, size);
}

static int card_free(void *card_data, void *ptr, size_t size)
{
	return genwqe_card_free(card_data, ptr, size);
}

static int _card_dump_statistics(FILE *fp)
{
	return genwqe_dump_statistics(fp);
}

static struct ddcb_accel_funcs accel_funcs = {
	.card_type = DDCB_TYPE_GENWQE,
	.card_name = "GENWQE",

	/* functions */
	.card_open = card_open,
	.card_close = card_close,
	.ddcb_execute = ddcb_execute,
	.card_strerror = _card_strerror,
	.card_read_reg64 = card_read_reg64,
	.card_read_reg32 = card_read_reg32,
	.card_write_reg64 = card_write_reg64,
	.card_write_reg32 = card_write_reg32,
	.card_get_app_id = _card_get_app_id,
	.card_get_queue_work_time = _card_get_queue_work_time,
	.card_get_frequency = _card_get_frequency,
	.card_dump_hardware_version = card_dump_hardware_version,
	.card_pin_memory = card_pin_memory,
	.card_unpin_memory = card_unpin_memory,
	.card_malloc = card_malloc,
	.card_free = card_free,

	/* statistics */
	.dump_statistics = _card_dump_statistics,
	.num_open = 0,
	.num_close = 0,
	.num_execute = 0,
	.time_open = 0,
	.time_execute = 0,
	.time_close = 0,

	.priv_data = NULL,
};

static void genwqe_card_init(void) __attribute__((constructor));

/* constructor */
static void genwqe_card_init(void)
{
	ddcb_register_accelerator(&accel_funcs);
}
