#ifndef __LIB_CARD_H__
#define __LIB_CARD_H__

/*+-------------------------------------------------------------------+
**| IBM Confidential                                                  |
**|                                                                   |
**| Licensed Internal Code Source Materials                           |
**|                                                                   |
**| © Copyright IBM Corp. 2014                                        |
**|                                                                   |
**| The source code for this program is not published or otherwise    |
**| divested of its trade secrets, irrespective of what has been      |
**| deposited with the U.S. Copyright Office.                         |
**+-------------------------------------------------------------------+
**/

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
