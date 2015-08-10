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

#ifndef __LIB_CARD_H__
#define __LIB_CARD_H__

/**
 * @file card_defs.h
 *
 * @brief Common defines for libraries. Local definitions which are
 * not exported to the outside.
 *
 * IBM Accelerator Family 'GenWQE'
 */

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */

static inline pid_t gettid(void)
{
	return (pid_t)syscall(SYS_gettid);
}

#define pr_err(fmt, ...)						\
	fprintf(stderr, "%08x.%08x %s:%u: Error: " fmt,			\
		getpid(), gettid(), __FILE__, __LINE__, ## __VA_ARGS__)

#define pr_warn(fmt, ...) do {						\
		if (_dbg_flag)						\
			fprintf(stderr, "%08x.%08x %s:%u: Warn: " fmt,	\
				getpid(), gettid(), __FILE__, __LINE__,	\
				## __VA_ARGS__);			\
	} while (0)

#define	pr_dbg(fmt, ...) do {						\
		if (_dbg_flag)						\
			fprintf(stderr, fmt, ## __VA_ARGS__);		\
	} while (0)

#define	pr_info(fmt, ...) do {						\
		if (_dbg_flag)						\
			fprintf(stderr, "%08x.%08x %s:%u: Info: " fmt,	\
				getpid(), gettid(), __FILE__, __LINE__,	\
				## __VA_ARGS__);			\
	} while (0)

#endif	/* __CARD_DEFS_H__ */
