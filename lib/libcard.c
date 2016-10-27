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
 * Generic support to enqueue DDCB commands. Card maintenace functions
 * for bitstream and VPD support as well as register access for
 * debugging purposes.
 *
 * The DDCB commands can contain references to user memory. There are
 * two different ways to describe memory within DDCBs. The first one
 * is raw contignous memory allocated with help of the driver. Since
 * some OSes do cannot guaranteee more than one memory page
 * (e.g. 4KiB) contigously, there is a 2nd way to describe a memory
 * reference. This is done by passing the virtual user-space address
 * to the device driver and instructing it to build up a scatter
 * gather list, which describes the data. This can be done dynamically
 * or optimized by previously pinning the memory area used for data
 * processing. The unpinning is done when the file-descriptor is
 * closed or when the unpin function is called.
 *
 * When the card handle is opened with the GENWQE_MODE_ASYNC flag set,
 * the library will enable SIGIO generation by device driver. This is
 * needed to recover gracefully when a card died.
 *
 * In addition the library will start a health checking pthread, which
 * checks periodically every 1 sec if the file descriptors to use the
 * card are still usable. If not the broken file descriptors are
 * closed. When the last descriptor is closed the library will stop
 * the health thread.
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

#include <sys/inotify.h>

#include <linux/genwqe/genwqe_card.h>

#include "card_defs.h"
#include "libcard.h"

//#define CONFIG_USE_SIGNAL
#undef CONFIG_USE_SIGNAL

#define CRC32_POLYNOMIAL	0x20044009
#define	MAX_GENWQ_CARDS	16
#define	MAX_VFUNCTIONS	16
#define	MAX_FUNC_NUM	(MAX_GENWQ_CARDS * MAX_VFUNCTIONS)
#define INVALID_FD	-1
#define CONFIG_RETRY_TIMEOUT	30	/* timeout in sec in Multi Card Mode */

enum dev_state {DEV_ALLOC, DEV_HAVE_FD, DEV_REQ_CLOSE, DEV_FREE};

/* This is the internal structure for each Stream */
struct card_dev_t {
	int  card_no;		   /* The Card# or GENWQE_CARD_REDUNDANT */
	enum dev_state dev_state;  /* The state of this Stream */
	int  mode;		   /* GENWQE_MODE_ASYNC | GENWQE_MODE_RDWR |
				    * GENWQE_MODE_NONBLOCK
				    */
	int  omode;
	int  fd_s;		   /* fd for Single mode */
	int  drv_rc;		   /* driver return codes */
	int  drv_errno;		   /* driver errno */
	uint64_t slu_id;	   /* service layer version */
	uint64_t app_id;	   /* app version */
	struct fd_node *m_fd_ptr;  /* Ptr. to current fd in Multi fd list */
	struct card_dev_t *next;   /* Next Card (Single Mode only) */
	struct card_dev_t *prev;   /* Prev. Card (Single Mode only) */
	struct card_dev_t *verify; /* My address again to verify */
};

enum genwqe_fd_state {CARD_CLOSED, CARD_OPEN};

enum inotify_ev {INOTIFY_IDLE, INOTIFY_ATTRIB};

struct lib_data_t {
	uint32_t crc32_tab[256];	/* CRC32 calculation table */
	pthread_t thread_id;		/* Thread id of Health thread or -1 */
	sem_t	health_sem;		/* Sem to post healt thread */
	int	thread_rc;
	pthread_mutex_t fds_mutex;	/* Lock Mutex */
	int	fd_s_count;		/* # of open fd's in card_dev_t */
	int	fd_m_count;		/* # of fd's in fd_m_list */
	int	m_mode_save;		/* Mode when i open the device
					 * in Multi Mode
					 */

	/* State of each card */
	enum genwqe_fd_state genwqe_state[MAX_FUNC_NUM];

#if defined(CONFIG_USE_SIGNAL)
	struct sigaction oldact;	/* Used for sigio */
	struct sigaction newact;	/* Used for sigio */
#endif

	/* more data for inotify */
	int	inotify_rc;		/* rc form inotify_thread */
	int	inotify_fd;		/* fd form inotify_create */
	int	inotify_wd;		/* from inotify_add_watch and delete */
	int	inotify_card;		/* the decimal card num (0..256) */
	pthread_t	inotify_tid;	/* tid form _inotify_thread */
	enum	inotify_ev inotify_event;	/* IDLE or CREATE or DELETE */
};

/* This is a list of fd's when i run in Multi (Redundant) Mode */
struct fd_node {
	int card_num;		/* The Card Number */
	int m_fd;		/* fd for Multi (Redundant) mode */
	struct fd_node *next;
	struct fd_node *prev;
};

static struct lib_data_t lib_data;
static struct dev_card_t *s_dev_head = NULL;	/* Head for Single Mode */
static struct dev_card_t *m_dev_head = NULL;	/* Head for Multi Mode */
static struct fd_node *__fd_m_list = NULL;

/* statistics */
#define NUM_CARDS 16 /* max number of GenWQE cards in system */
static unsigned int card_completed_ddcbs[NUM_CARDS] = { 0, };
static unsigned int card_retried_ddcbs[NUM_CARDS] = { 0, };

#if defined(CONFIG_USE_SIGNAL)
static unsigned int card_health_signal = 0;
#endif

static int _dbg_flag;

static const char * const card_errlist[] = {
	[ABS(GENWQE_OK)] = "success",
	[ABS(GENWQE_ERRNO)] = "system error, please see errno",
	[ABS(GENWQE_ERR_CARD)] =
	"problem detected with card, please see errno and returned data",
	[ABS(GENWQE_ERR_OPEN)] = "could not get card handle",
	[ABS(GENWQE_ERR_VERS_MISMATCH)] = "libzcard version mismatch",
	[ABS(GENWQE_ERR_INVAL)] = "invalid parameter",
	[ABS(GENWQE_ERR_FLASH_VERIFY)] = "verification of flash failed",
	[ABS(GENWQE_ERR_FLASH_READ)] = "reading flash failed",
	[ABS(GENWQE_ERR_FLASH_UPDATE)] = "updating card failed",
	[ABS(GENWQE_ERR_GET_STATE)] = "cannot get state of card",
	[ABS(GENWQE_ERR_SIM)] = "simulation of card had a problem",
	[ABS(GENWQE_ERR_EXEC_DDCB)] =
	"error on ddcb execution occurred, please see errno and returned data",
	[ABS(GENWQE_ERR_PINNING)] = "memory buffer pinning error, see errno",
	[ABS(GENWQE_ERR_TESTMODE)] = "problem in testmode",
	[ABS(GENWQE_ERR_APPID)] = "not supported application id",
};

static const int card_nerr __attribute__((unused)) = ARRAY_SIZE(card_errlist);

const char *card_strerror(int errnum)
{
	if (ABS(errnum) >= card_nerr)
		return NULL;
	return card_errlist[ABS(errnum)];
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

const char *retc_strerror(int errnum)
{
	if (ABS(errnum) >= retc_nerr)
		return NULL;
	return retc_errlist[ABS(errnum)];
}

static int __genwqe_card_get_state(int fd, enum genwqe_card_state *state)
{
	return ioctl(fd, GENWQE_GET_CARD_STATE, state);
}

static int __mode_2_omode(int mode)
{
	int	omode = 0;

	// Create Open mode from mode
	if (mode & GENWQE_MODE_RDONLY)
		omode |= O_RDONLY;
	if (mode & GENWQE_MODE_WRONLY)
		omode |= O_WRONLY;
	if (mode & GENWQE_MODE_RDWR)
		omode |= O_RDWR;
	if (mode & GENWQE_MODE_NONBLOCK)
		omode |= O_NONBLOCK;

	/* Remove this checking, FASYNC will be set later on with fcntl */
	//if (mode & GENWQE_MODE_ASYNC)
		//omode |= FASYNC;
	return omode;
}

static int __genwqe_dev_open(int card_no, int mode)
{
	int fd;
	int omode;
	char card_dev[256]; // temp dev name

	omode = __mode_2_omode(mode);
	snprintf(card_dev, sizeof(card_dev) - 1, CARD_DEVICE, card_no);
	fd = open(card_dev, omode);
	if (fd < 0)
		return INVALID_FD;

#if defined(CONFIG_USE_SIGNAL)
	if (GENWQE_MODE_ASYNC & mode) {
		int  oflags;

		/*
		 * Set FASYNC flag to catch the SIGIO when a card gets
		 * removed.
		 */
		fcntl(fd, F_SETOWN, getpid());
		oflags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, oflags | FASYNC);
	}
#endif
	pr_info("__genwqe_dev_open: %s OK fd: %d (omode: 0x%x mode: 0x%x)\n",
		card_dev, fd, omode, mode);
	return fd;
}

static struct fd_node *__fd_m_new(struct fd_node *parent,
		int fd, int card)
{
	struct fd_node *node;

	node = malloc(sizeof(struct fd_node));
	if (node) {
		node->m_fd = fd;
		node->card_num = card;
		node->next = NULL;
		node->prev = parent;
		if (parent)
			parent->next = node;
	} else pr_err("malloc failed\n");
	return node;
}

static void __fd_m_head_all(void);

static void __fd_m_add(int fd, int card)
{
	struct fd_node *head;

	if (NULL == __fd_m_list) {
		__fd_m_list = __fd_m_new(NULL, fd, card);
		/* i am adding the first fd to my list, set pointer to head */
		__fd_m_head_all();
	} else {
		head = __fd_m_list;
		while (NULL != head->next)
			head = head->next;
		__fd_m_new(head, fd, card);
	}
	return;
}

static void __fd_m_del(int fd)
{
	struct fd_node *this = __fd_m_list;

	while (this) {
		if (this->m_fd == fd) {
			if ((NULL == this->next) && (NULL == this->prev)) {
				/* Erase last */
				__fd_m_list = NULL;
			} else if (NULL == this->next) {
				/* Erase tail */
				this->prev->next = NULL;
			} else if (NULL == this->prev) {
				/* Erase 1st */
				this->next->prev = NULL;
				__fd_m_list = this->next;
			} else {
				/* Erase middle */
				this->prev->next = this->next;
				this->next->prev = this->prev;
			}
			free(this);
			return;
		}
		this = this->next;
	}
	pr_err("fd: %d not found in fd_m_list: %p\n", fd, __fd_m_list);
	return;
}

/*
 * Function: __fd_m_head_all()
 *
 * @brief: This function is called in the case whe a fd fails in Multi
 * fd mode. The card will then be removed from the list and it can be
 * tat any device using the fd_list is trying to get the next fd and
 * this fd is removed in the meantime. I go over all devices and reset
 * then m_fd_ptr to the head of the List.
 */
static void __fd_m_head_all(void)
{
	struct card_dev_t *dev = (struct card_dev_t *)m_dev_head;

	while (dev) {
		dev->m_fd_ptr = __fd_m_list;	// Set Head
		dev = dev->next;
	}
}

static int __fd_m_head(struct card_dev_t *dev)
{
	struct fd_node *now;
	int    fd = INVALID_FD;

	now = __fd_m_list;
	dev->m_fd_ptr = now;	// Set Head
	if (now)
		fd = now->m_fd;
	pr_info("__fd_m_head at: %p fd: %d\n", now, fd);
	return fd;
}

/*
 * Note: fds_mutex must be held. Get a fd in multi fd (Redundant)
 * Mode and increment to next fd.
 */
static int __fd_m_get_and_inc(struct card_dev_t *dev, int *card_num)
{
	struct fd_node *now, *next;
	int    fd = INVALID_FD;		// Set to INVALID

	now = dev->m_fd_ptr;		// Get current Position of fd list
	if (now) {
		fd = now->m_fd;		// Take this fd
		if (card_num)
			*card_num = now->card_num;

		next = now->next;	// Next
		if (NULL == next)	// Check for end
			next = __fd_m_list;	// Reset to Head
		dev->m_fd_ptr = next;	// and save
	}
	return fd;
}

static int __fd_get(struct card_dev_t *dev, int *card_num)
{
	struct lib_data_t *ld = &lib_data;
	int	fd;

	pthread_mutex_lock(&ld->fds_mutex);
	if (GENWQE_CARD_REDUNDANT == dev->card_no)
		fd = __fd_m_get_and_inc(dev, card_num);
	else {
		fd = dev->fd_s;	// Normal Mode, return fd_s
		if (card_num)
			*card_num = dev->card_no;
	}
	pthread_mutex_unlock(&ld->fds_mutex);
	return fd;
}

static int __m_open_add(int card_no, int mode)
{
	int fd;

	fd = __genwqe_dev_open(card_no, mode);
	if (INVALID_FD != fd) {
		__fd_m_add(fd, card_no);
		return 1;	// Good
	}
	return 0;		// Can not Open
}

/*
 * Function: __genwqe_filter()
 *
 *	@brief	Filter for scandir as helper function for __m_open_all()
 *	@parm	Ptr. to name in dev
 *	@return	1 if name matches any of my genwqe devices
 */
static int __genwqe_filter(const struct dirent *name)
{
	if (0 == strncmp(name->d_name, GENWQE_DEVNAME, 6))
		return 1;
	return 0;
}

/*
 * Function: __m_open_all()
 *
 *	@brief	opens all genwqe cards
 *	@param	Pointer to dev
 *	@return	Number of opend fd's or 0 for bad.
 */
static int __m_open_all(struct lib_data_t *ld)
{
	int	found_cards = 0;
	int	card_no;
	int	n, rc;
	struct	dirent **namelist;

	n = scandir("/dev", &namelist, __genwqe_filter, NULL);
	if (n < 0)
		return 0;
	while (n--) {
		rc = sscanf(namelist[n]->d_name,
			    GENWQE_DEVNAME"%u_card", &card_no);
		if ((1 == rc) && (card_no >= 0) && (card_no < 256)) {
			switch (ld->genwqe_state[card_no]) {
			case CARD_CLOSED:	// Try to Open
				if (__m_open_add(card_no, ld->m_mode_save)) {
					ld->fd_m_count++;
					ld->genwqe_state[card_no] = CARD_OPEN;
					found_cards++;
				}
				break;
			case CARD_OPEN:		// card is already open
				found_cards++;
				break;
			default:
				break;
			}
		}
		free(namelist[n]);
	}
	free(namelist);
	return found_cards;
}

/*
 * Function: __node_create()
 *
 *	Creates (allocates) memory for a new node
 */
static struct card_dev_t *__node_create(int card_no, int mode)
{
	struct card_dev_t *new_dev;

	new_dev = malloc(sizeof(struct card_dev_t));
	if (new_dev) {
		new_dev->card_no = card_no;
		new_dev->dev_state = DEV_ALLOC;
		/* Add Data to Node */
		new_dev->mode = mode;
		new_dev->omode = __mode_2_omode(mode);
		new_dev->slu_id = 0;
		new_dev->app_id = 0;
		new_dev->fd_s = INVALID_FD; /* Set Single fd to Invalid */
		new_dev->m_fd_ptr = NULL;
		new_dev->next = NULL;
		new_dev->prev = NULL;
		new_dev->verify = new_dev;
	} else
		pr_err("Malloc failed for card %d\n", card_no);

	return new_dev;
}

/*
 * Function __node_add()
 *
 *     allocates a new card object. Its only a control
 *     Block with a fd for one card. The new Object will ba added
 *     to the end of the list.
 */
static struct card_dev_t *__node_add(int card_no,
		void **head,
		int mode)
{
	struct card_dev_t *parent, *new_dev;

	new_dev = __node_create(card_no, mode);
	if (NULL == new_dev)
		return NULL;

	parent = (struct card_dev_t*)*head;
	if (NULL == *head)
		*head = (void*)new_dev;
	else {
		while (NULL != parent->next)
			parent = parent->next;
		parent->next = new_dev;
	}
	new_dev->prev = parent;
	return new_dev;
}

/*
 * Function:	__node_delete()
 *
 *	called form __s_node_delete and __m__node_delete()
 *	Deletes a node form List.
 */
static void __node_delete(struct card_dev_t *node, void **head)
{
	if (node->verify != node) {
		pr_err("Invalid Dev: %p to delete.\n", node);
		return;
	}
	node->dev_state = DEV_FREE;
	if ((NULL == node->next) && (NULL == node->prev)) {
		/* Delete Last Element clears also root_node */
		*head = NULL;
	} else if (NULL == node->next) {
		/* Delete Tail Element, no change on root_node */
		node->prev->next = NULL;
	} else if (NULL == node->prev) {
		/* Delete Head Element, need to change root_node */
		node->next->prev = NULL;
		*head = (void*)node->next;
	} else {
		/* something in the middle, root_node stays */
		node->prev->next = node->next;
		node->next->prev = node->prev;
	}
	free(node);
}

/* ------------------------- START of Health Function's -------------------- */

#if defined(CONFIG_USE_SIGNAL)
/**
 * FIXME The next task we need to solve is to figure out which
 * file-descriptor is actually broken, when we are receiving SIGIO.
 * This descriptor must than be closed and not used again e.g. set to
 * -1. For recovery it might be reopened using an alternate card, or
 * when the currently unusable card should reappear after successful
 * recovery.
 */
static void __health_sa_sigaction(int sig, siginfo_t *si, void *data)
{
	struct lib_data_t *ld = &lib_data;  /* global variable */

	pr_warn("[%s] sig=%d si=%p data=%p si_fd=%d si_code=%d\n"
		"  FIXME The next task we need to solve is to figure\n"
		"  out which file-descriptor is actually broken, when\n"
		"  we are receiving SIGIO.\n\n",
		__func__, sig, si, data, si->si_fd, si->si_code);
	card_health_signal++;
	sem_post(&ld->health_sem);
}
#endif

/*
 * Function:	__inotify_handle()
 *	runs in health thread and adds a new card
 *	after the ATTRIB notification was send to me.
 */
static void __inotify_handle(struct lib_data_t *ld)
{
	int	card;

	if (INOTIFY_ATTRIB == ld->inotify_event) {
		card = ld->inotify_card;
		pr_info("%s Open Card: %d\n", __func__, card);
		if (__m_open_add(card, ld->m_mode_save)) {
			pr_info("%s Open Card: %d OK\n", __func__, card);
			ld->fd_m_count++;
			ld->genwqe_state[card] = CARD_OPEN;
			ld->inotify_event = INOTIFY_IDLE; // go back to IDLE
		}
	}
}

/* Helper function to check the multi fd list */
static int __mhealth_check(struct lib_data_t *ld)
{
	int fd, card_no;
	struct card_dev_t *dev, *dev_next;
	enum genwqe_card_state card_state;
	enum genwqe_fd_state state;
	struct fd_node *fd_list, *fd_list_next;

	pr_info("%s Enter %d open Fd's.\n", __func__, ld->fd_m_count);

	__inotify_handle(ld);	// handle events from inotify

	/* Delete pending Close dev's */
	dev = (struct card_dev_t *)m_dev_head;
	while (dev) {
		dev_next = dev->next;
		pr_info("%s Dev: %p State: %d\n", __func__,
			dev, dev->dev_state);
		if (DEV_REQ_CLOSE == dev->dev_state)
			__node_delete(dev, (void*)&m_dev_head);
		dev = dev_next;
	}
	/* Check if all entries in the dev list are gone */
	if (NULL == m_dev_head) {
		/* If so, i go over my list of the fds and close this
		   as well */
		fd_list = __fd_m_list;
		while (fd_list) {
			fd_list_next = fd_list->next;
			card_no	     = fd_list->card_num;
			fd	     = fd_list->m_fd;
			pr_info("Close: %p Card: %d fd: %d\n",
				fd_list, card_no, fd);
			close(fd);
			ld->genwqe_state[card_no] = CARD_CLOSED;
			__fd_m_del(fd);	/* Remove from List */
			ld->fd_m_count--;
			fd_list = fd_list_next;
		}
		pr_info("%s Close Exit Count: %d (Must be 0 !)\n",
			__func__, ld->fd_m_count);
		return 0;
	}
	/* take all Open fd's in list and check if they are alive */
	fd_list = __fd_m_list;
	while (fd_list) {
		fd_list_next = fd_list->next;
		fd	     = fd_list->m_fd;
		card_no	     = fd_list->card_num;
		state	     = ld->genwqe_state[card_no];
		if (CARD_OPEN == state) {
			__genwqe_card_get_state(fd, &card_state);
			if (GENWQE_CARD_USED != card_state) {
				pr_info("%s delete from List: %p Card: %d "
					"fd: %d\n",
					__func__, fd_list, card_no, fd);
				__fd_m_del(fd);	   /* Remove from List */
				__fd_m_head_all(); /* Reset all Devs to head
						    * fd list
						    */
				close(fd);	   /* Close */
				ld->genwqe_state[card_no] = CARD_CLOSED;
				ld->fd_m_count--;
			}
		}
		fd_list = fd_list_next;
	}
	pr_info("%s EXIT: %p fd_list: %p with %d Entry's.\n",
		__func__, m_dev_head, __fd_m_list, ld->fd_m_count);
	if (m_dev_head)
		return 1;	// Keep Going
	return 0;		// Exit
}

/*
 * Function: __shealth_check()
 *
 *	 Helper function to check the Single fd list
*/
static int __shealth_check(struct lib_data_t *ld)
{
	struct card_dev_t *dev, *dev_next;
	enum genwqe_card_state card_state;
	int	fd;

	/* Process Single Mode Chain */
	dev = (struct card_dev_t*)s_dev_head;
	while (dev) {
		dev_next = dev->next;
		fd = dev->fd_s;
		pr_info("%s: Node: %p fd: %d State: %d\n",
			__func__, dev, fd, dev->dev_state);
		if(DEV_REQ_CLOSE == dev->dev_state) {
			 __node_delete(dev, (void*)&s_dev_head);
			if (INVALID_FD != fd) {
				close(fd);
				ld->fd_s_count--;
			}
		} else {
			if (INVALID_FD != fd) {
				__genwqe_card_get_state(fd, &card_state);
				if (GENWQE_CARD_USED != card_state) {
					__node_delete(dev, (void*)&s_dev_head);
					close(fd);
					ld->fd_s_count--;
				}
			}
		}
		dev = dev_next;
	}
	if (s_dev_head)
		return 1;
	return 0;
}

#if defined(CONFIG_USE_SIGNAL)
static void __inotify_termination_handler(int signum)
{
	/* struct lib_data_t *ld = &lib_data; */  /* global variable */
	pr_info("%s Signum: %d \n", __func__, signum);
}
#endif

/*
 * Function: __inotify_handle_event()
 *	Called from:	__inotify_thread()
 *	Handels data from inotify read.
 */
static void __inotify_handle_event(int len, char *buf, struct lib_data_t *ld)
{
	struct inotify_event *ie;
	int	i, card, n;

	i = 0;
	pr_info("__inotify_handle_event %d\n", len);
	while (i < len) {
		ie = (struct inotify_event*) &buf[i];
		if ((ie->mask & IN_ATTRIB) && (ie->len > 0)) {
			n = sscanf(ie->name,
				   GENWQE_DEVNAME"%d", &card);
			if (1 == n) {
				/* Make sure that the new card */
				/* was gone before adding back in */
				if (CARD_CLOSED ==
				    ld->genwqe_state[card]) {
					/* Create was done, ATTRIB, */
					/* was set, Post Health Sem to
					   Open again */
					ld->inotify_card = card;
					ld->inotify_event = INOTIFY_ATTRIB;

					/* post __inotify_handle */
					usleep(50000);
					/* !!! need some delay */

					pr_info("%s Start Health "
						"Thread for new "
						"Card: %s\n",
						__func__, ie->name);
					sem_post(&ld->health_sem);
				}
			}
		}
		i += sizeof(struct inotify_event) + ie->len;
	}
}

/*
 * Function: __inotify_thread()
 *	This thread waits for Delete and Create events for
 *	genwqe*_card in /dev
 */
static void *__inotify_thread(void *data)
{
	int	len, rc;
	struct lib_data_t *ld = (struct lib_data_t *)data;
	char buf[sizeof(struct inotify_event) + PATH_MAX];
	fd_set rfds;
	sigset_t sig_empty_mask;

#if defined(CONFIG_USE_SIGNAL)
	struct sigaction action;
	sigset_t sigmask;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigmask, NULL);

	action.sa_handler = __inotify_termination_handler;
	sigemptyset( &action.sa_mask );
	action.sa_flags = 0;
	sigaction(SIGUSR1, &action, NULL); /* set SIGUSR1 to kill me */

	sigemptyset(&sig_empty_mask);
#endif

	while (1) { /* Exit because of sig handler */
		FD_ZERO(&rfds);
		FD_SET(ld->inotify_fd, &rfds);	/* Set fd */
		rc = pselect(FD_SETSIZE, &rfds, NULL, NULL, NULL,
			     &sig_empty_mask);
		if (rc > 0) {
			len = read(ld->inotify_fd, buf, sizeof(buf));
			if (-1 == len) {
				/* Read fails, set rc and Exit */
				ld->inotify_rc = 100;
				break;
			}
			__inotify_handle_event(len, buf, ld);
		} else {
			if (rc < 0) {
				/* EINTR: Select was killed by SIGUSR1 */
				ld->inotify_rc = 200;
				break;
			}
		}
	}
	pr_info("%s exit fd: %d wd: %d\n", __func__, ld->inotify_fd,
		ld->inotify_wd);
	pthread_exit(&ld->inotify_rc);
}

/*
 * Function: __inotify_create()
 *	This functions creates the inotify event handler thread
 *	only for multiple mode
 */
static void __inotify_create(struct lib_data_t *ld)
{
	int	fd, wd;

	if ((pthread_t)-1 != ld->inotify_tid)
		return;			// already Running
	fd = inotify_init();
	if (fd < 0) {
		pr_err("Failed to initialize inotify instance %d\n", errno);
		return;
	}
	/* i use ATTRIB to watch only*/
	wd = inotify_add_watch(fd, "/dev", IN_ATTRIB);
	if (wd < 0) {
		pr_err("Failed to add inotify watch. %d\n", errno);
		return;
	}
	ld->inotify_event = INOTIFY_IDLE;
	ld->inotify_fd = fd;
	ld->inotify_wd = wd;
	/* Create thread */
	if (0 == pthread_create(&ld->inotify_tid, NULL, &__inotify_thread, ld))
		return;
	pr_err("%s failed!\n", __func__);
	return;
}

static void __fixup_fd_lists(struct lib_data_t *ld)
{
	pr_info("%s fd_s_count: %d fd_m_count: %d\n",
		__func__, ld->fd_s_count, ld->fd_m_count);
	/* Check m_list if there, only check s_list if no m_list */
	if (m_dev_head)
		__mhealth_check(ld);
	else if (s_dev_head)
		__shealth_check(ld);
}

/*
 * Function: __health_thread()
 *	runs every 10 second or when it gets a post from
 *	sighandler or inotify. The thread ends if there is no
 *	fd left open.
 */
static void *__health_thread(void *data)
{
	struct lib_data_t *ld = (struct lib_data_t *)data;

	while (1) {
		/* int rc; */
		struct timespec ts;

		/* INOTIFY: Block, inotify and signal handler will post me */
		if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
			perror("clock_gettime");

		ts.tv_sec += 4;
		sem_timedwait(&ld->health_sem, &ts);

		/*
		 * fprintf(stderr, "sem_timedwait ... returned %d: %s\n",
		 *         rc, rc == -1 ? strerror(errno) : "OK");
		 */

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		pthread_mutex_lock(&ld->fds_mutex);
		__fixup_fd_lists(ld);
		pthread_mutex_unlock(&ld->fds_mutex);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	}


	pr_info("%s exit S: %p:%d M: %p:%d\n", __func__,
		s_dev_head, ld->fd_s_count, m_dev_head, ld->fd_m_count);

	pthread_exit(&ld->thread_rc);
}

/**
 * @brief Install signal handler for SIGIO to enable us to react on
 * problems with one of the card. In case of problems we need to close
 * any open filedescriptor for those cards in trouble, to enable them
 * to go through our recovery processes which involves the associated
 * devices to go away and come back when the problem got resolved.
 *
 * If the file descriptors of a broken card are not closed 4 sec after
 * receiving the SIGIO, the process will be killed with a SIGKILL.
 *
 * Start healthchecking if usage is more than 0. We do not want to
 * waste resources if there are no cards in use.
 */
static int __health_thread_start(struct lib_data_t *ld)
{
	int rc;

	if ((pthread_t)-1 != ld->thread_id)
		return 0;	// Thread already running

	rc = sem_init(&ld->health_sem, 0, 0);
	if (0 != rc)
		goto err_out;

	rc = pthread_create(&ld->thread_id, NULL, &__health_thread, ld);
	if (0 != rc)
		goto err_out;

#if defined(CONFIG_USE_SIGNAL)
	sigemptyset(&ld->newact.sa_mask);
	ld->newact.sa_sigaction = __health_sa_sigaction;
	ld->newact.sa_flags = SA_SIGINFO;
	if (0 == sigaction(SIGIO, &ld->newact, &ld->oldact))
		return 0;
#endif
	return 0;

 err_out:
	ld->thread_id = -1;
	pr_err("%s failed rc=%d\n", __func__, rc);
	return -1;
}

/* ---------------------------- END of Health Function's ------------------- */

static void __card_get_app(struct card_dev_t *dev)
{
	/* Read and save SLU_ID and APP_ID */
	dev->slu_id = genwqe_card_read_reg64(dev, IO_SLU_UNITCFG, NULL);
	dev->app_id = genwqe_card_read_reg64(dev, IO_APP_UNITCFG, NULL);
}

/*
 * Function:	__genwqe_open_one()
 *
 *	open one genwqe card
 */
static int __genwqe_open_one(struct card_dev_t *dev)
{
	int card_no_masked = dev->card_no & GENWQE_TESTMODE_MASK;
	int fd;
	struct lib_data_t *ld = &lib_data;

	fd = __genwqe_dev_open(card_no_masked, dev->mode);
	dev->drv_errno = errno;
	if (INVALID_FD != fd) {
		dev->drv_rc = fd;
		dev->fd_s = fd;
		__card_get_app(dev);
		ld->fd_s_count++;
		return fd;
	}
	return INVALID_FD;
}

/*
 * Function: __genwqe_open_all()
 *
 *	@brief open all genwqe cards
 *	@param Pointer to dev
 *	@return Number of opend fd's or INVALID for bad.
 */
static int __genwqe_open_all(struct card_dev_t *dev)
{
	int cards;
	struct lib_data_t *ld = &lib_data;
	int fd = INVALID_FD;

	/* I make sure that all opens do have the same mode for open */
	if (-1 == ld->m_mode_save)
		ld->m_mode_save = dev->mode;	/* Save mode in case i
						   need to reopen */
	else if (ld->m_mode_save != dev->mode)
		dev->mode = ld->m_mode_save;	/* Keep Old mode and
						   overwrite */

	cards = __m_open_all(ld);
	if (cards) {
		fd = __fd_m_head(dev);	// Set to Head
		pr_info("%s %d Cards with %d fd's, use fd: %d first.\n",
			__func__, cards, ld->fd_m_count, fd);
		dev->fd_s = fd;		/* Take fd and save to dev */
		__card_get_app(dev);	/* Get SLU and APP form my 1st Card */

		__inotify_create(ld);
	}
	return fd;
}

/**
 * Check correctness of the application id. This function must not be
 * verbose. It already returns a meaningful return code to indicate
 * that the id was not right.
 */
static int __card_check_app(struct card_dev_t *dev,
		uint64_t app_id, uint64_t mask)
{
	if ((dev->app_id & mask) != (app_id & mask)) {
		pr_info("Wrong AppID: %016llx Expect: %016llx Mask: %016llx "
			"on fd %d\n", (unsigned long long)dev->app_id,
			(unsigned long long)app_id,
			(unsigned long long)mask,
			dev->fd_s);
		return GENWQE_ERR_APPID;
	}
	return GENWQE_OK;
}

/**
 * @brief	enable or disable debug outputs from library.
 *		pr_info() is activated or disabled
 *
 * @param onoff	if 0 -> no outputs
 *
 * @return -
 */
void genwqe_card_lib_debug(int onoff)
{
	_dbg_flag = onoff;
}

/**
 * @brief	setup CRC32 table (crc32_tab) for fast calculation
 */
static void ddcb_setup_crc32(struct lib_data_t *d)
{
	int i, j;
	uint32_t crc;

	for (i = 0;  i < 256;  i++) {
		crc = i << 24;
		for (j = 0;  j < 8;  j++) {
			if (crc & 0x80000000)
				crc = (crc << 1) ^ CRC32_POLYNOMIAL;
			else
				crc = (crc << 1);
		}
		d->crc32_tab[i] = crc;
	}
}

/**
 * @brief	generate 32-bit crc as required for DDCBs
 *		polynomial = x^32 + x^29 + x^18 + x^14 + x^3 + 1
 *		- example:
 *		  4 bytes 0x01 0x02 0x03 0x04 with init = 0xffffffff
 *		  should result in a crc32 of 0xf33cb7d3
 *
 * @param	buff	pointer to data buffer
 * @param	len	leongth of data for calculation
 * @param	init	initial crc (0xffffffff at start)
 *
 * @return	crc32 checksum in big endian format !
 */
uint32_t genwqe_ddcb_crc32(uint8_t *buff, size_t len, uint32_t init)
{
	int i;
	uint32_t crc;

	crc = init;
	while (len--) {
		i = ((crc >> 24) ^ *buff++) & 0xFF;
		crc = (crc << 8) ^ lib_data.crc32_tab[i];
	}
	return crc;
}

int genwqe_get_drv_rc(card_handle_t dev)
{
	return dev->drv_rc;
}

int genwqe_get_drv_errno(card_handle_t dev)
{
	return dev->drv_errno;
}

int genwqe_card_get_state(card_handle_t dev, enum genwqe_card_state *state)
{
	int fd;

	if (NULL == dev)
		return GENWQE_ERR_INVAL;

	fd = __fd_get(dev, NULL);
	dev->drv_rc = __genwqe_card_get_state(fd, state);
	if (0 == dev->drv_rc)
		return GENWQE_OK;
	return GENWQE_ERR_GET_STATE;
}

/**
 * @brief	reads 64-bit register number 'offs' from GENWQE card
 * @param offs	mmio offset as defined in genwqe_io.h
 * @return	register content
 */
uint64_t genwqe_card_read_reg64(card_handle_t dev, uint32_t offs, int *rc)
{
	struct genwqe_reg_io io;

	if (rc)
		*rc = GENWQE_ERR_CARD;
	if (NULL == dev)
		return GENWQE_ERR_INVAL;

	io.num = offs;
	io.val64 = 0;
	dev->drv_rc = ioctl(dev->fd_s, GENWQE_READ_REG64, &io);
	dev->drv_errno = errno;
	if (dev->drv_rc < 0)
		 io.val64 = 0;
	else {
		if (rc)		/* FIXME Strange? ... */
			*rc = GENWQE_OK;
	}
	return io.val64;
}

/**
 * @brief	reads 32-bit register number 'offs' from GENWQE card
 * @param offs	mmio offset as defined in genwqe_io.h
 * @return	register content
 */
uint32_t genwqe_card_read_reg32(card_handle_t dev, uint32_t offs, int *rc)
{
	struct genwqe_reg_io io;

	if (rc)
		*rc = GENWQE_ERR_CARD;
	if (NULL == dev)
		return GENWQE_ERR_INVAL;
	/* i am only allowing this in not redundant mode */
	io.num = offs;		/* register offset for 32-bit pointer */
	io.val64 = 0;
	dev->drv_rc = ioctl(dev->fd_s, GENWQE_READ_REG32, &io);
	dev->drv_errno = errno;
	if (dev->drv_rc < 0)
		io.val64 = 0;
	else {
		if (rc)		/* FIXME Strange? ... */
			*rc = GENWQE_OK;
	}
	return (uint32_t)io.val64;
}

/**
 * @brief	writes 64-bit register number 'offs' with value 'val'
 * @param offs	32-bit mmio offset as defined in genwqe_io.h
 * @return	register content
 */
int genwqe_card_write_reg64(card_handle_t dev, uint32_t offs, uint64_t val)
{
	struct genwqe_reg_io io;

	if (NULL == dev)
		return GENWQE_ERR_INVAL;
	/* i am only allowing this in not redundant mode */
	io.num = offs;
	io.val64 = val;
	dev->drv_rc = ioctl(dev->fd_s, GENWQE_WRITE_REG64, &io);
	dev->drv_errno = errno;
	if (0 == dev->drv_rc)
		return GENWQE_OK;
	return GENWQE_ERR_CARD;
}
#define	MAX_GENWQ_CARDS	16
#define	MAX_VFUNCTIONS	16
#define	MAX_FUNC_NUM	(MAX_GENWQ_CARDS * MAX_VFUNCTIONS)
/**
 * @brief	writes 32-bit register number 'offs' with value 'val'
 * @param card [in] card handle
 * @param offs [in] 32-bit mmio offset as defined in genwqe_io.h
 * @return	register content
 */
int genwqe_card_write_reg32(card_handle_t dev, uint32_t offs, uint32_t val)
{
	struct genwqe_reg_io io;

	if (NULL == dev)
		return GENWQE_ERR_INVAL;
	/* i am only allowing this in not redundant mode */
	io.num = (__u64)offs;
	io.val64 = (__u64)val;
	dev->drv_rc = ioctl(dev->fd_s, GENWQE_WRITE_REG32, &io);
	dev->drv_errno = errno;
	if (0 == dev->drv_rc)
		return GENWQE_OK;
	return GENWQE_ERR_CARD;
}

/**
 * @brief	initialization of the Genwqe card and the GENWQE library
 *
 * allocates and presets required memory, sets version numbers
 * and opens a card device.
 *
 * @param card_no card number to use if > 0,
 *                GENWQE_CARD_REDUNDANT pick free card and recover if
 *                card is unavailable.
 * @param mode    select different characteristics e.g. use SIGIO, ...
 * @return 0 if success
 */
card_handle_t genwqe_card_open(int card_no, int mode, int *err_code,
			       uint64_t card_app_id, uint64_t card_app_id_mask)
{
	card_handle_t dev;
	struct lib_data_t *ld = &lib_data;
	int fd;
	int rc;

	pthread_mutex_lock(&ld->fds_mutex);
	pr_info("%s Enter Card: %d\n", __func__, card_no);

	if (GENWQE_CARD_REDUNDANT == card_no)
		dev = __node_add(card_no, (void*)&m_dev_head, mode);
	else	dev = __node_add(card_no, (void*)&s_dev_head, mode);

	__health_thread_start(ld);	/* Needs Mutex protection */

	if (NULL == dev) {
		pthread_mutex_unlock(&ld->fds_mutex);
		if (err_code)
			*err_code = GENWQE_ERRNO;
		return NULL;
	}
	if (GENWQE_CARD_REDUNDANT == card_no)
		fd = __genwqe_open_all(dev);
	else	fd = __genwqe_open_one(dev);

	/* Check if i do have an fd */
	if (INVALID_FD != fd) {
		rc = __card_check_app(dev, card_app_id, card_app_id_mask);
		if (err_code)
			*err_code = rc;
		if (GENWQE_OK == rc) {
			dev->dev_state = DEV_HAVE_FD;
			pr_info("%s Exit Card: %d Dev: %p\n", __func__,
				card_no, dev);
			pthread_mutex_unlock(&ld->fds_mutex);
			return dev;
		}
	}
	if (err_code)
		*err_code = GENWQE_ERR_OPEN;
	pr_info("%s Err Dev: %p Card: %d fd: %d\n", __func__,
		dev, card_no, fd);
	genwqe_card_close(dev);
	pthread_mutex_unlock(&ld->fds_mutex);
	return NULL;
}

/**
 * @brief	end GENWQE library accesses
 * close all open files, free memory
 *
 * @param card	pointer to the opened device descriptor
 *
 * @return	GENWQE_OK if everything is ok.
 */
int genwqe_card_close(card_handle_t dev)
{
	struct lib_data_t *ld = &lib_data;
	int rc = GENWQE_ERR_INVAL;

	if (dev) {
		if (dev->verify == dev) {
			dev->dev_state = DEV_REQ_CLOSE;
			pr_info("%s Request %p fd: %d\n",
				__func__, dev, dev->fd_s);
			sem_post(&ld->health_sem);
			rc = GENWQE_OK;
		}
	}
	return rc;
}

/**
 * @brief	retrieve operating systems file handle
 *
 * @param card	pointer to the opened device descriptor
 *
 * @return	file handle
 */
int genwqe_card_fileno(card_handle_t dev)
{
	int fd = GENWQE_ERR_INVAL;

	if (dev)
		fd = __fd_get(dev, NULL);
	return fd;
}

/**
 * @brief Prepare buffer to do DMA transactions. The driver will
 * create DMA mappings for this buffer and will allocate memory to
 * hold and sglist which describes the buffer. When executing DDCBs
 * the driver will use the cached entry before it tries to dynamically
 * allocate a new one. The intend is to speed up performance. The
 * resources are freed on device close or when calling the unpin
 * function.
 *
 * @param [in] dev      .dev handle
 * @param [in] addr      user space address of memory buffer
 * @param [in] size      size of user space memory buffer
 * @param [in] direction 0: read/1: read and write
 * @return	         GENWQE_LIB_OK on success or negative error code.
 */
int genwqe_pin_memory(card_handle_t dev, const void *addr, size_t size,
		     int direction)
{
	int rc, fd;
	struct genwqe_mem m;

	m.addr = (unsigned long)addr;
	m.size = size;
	m.direction = direction;

	pr_info("pin:   addr=%016lx size=%08lx dir=%d Card=%p ",
		(unsigned long)addr, (unsigned long)size, direction, dev);
	if (dev) {
		if (dev == dev->verify) {
			fd = __fd_get(dev, NULL);
			pr_info("Card %d\n", dev->card_no);
			dev->drv_rc = rc = ioctl(fd, GENWQE_PIN_MEM, &m);
			dev->drv_errno = errno;
			if (0 == rc)
				return GENWQE_OK;
		}
	}
	pr_err("Dev: %p Fault: %d addr=%p size=%lld dir=%d\n", dev,
	       dev->drv_errno, addr, (long long)size, direction);
	return GENWQE_ERR_PINNING;
}

int genwqe_unpin_memory(card_handle_t dev, const void *addr, size_t size)
{
	int rc, fd;
	struct genwqe_mem m;

	m.addr = (unsigned long)addr;
	m.size = size;
	m.direction = 0;

	pr_info("unpin: addr=%016lx size=%08lx card=%p",
			(unsigned long)addr, (unsigned long)size, dev);
	if (dev) {
		if (dev == dev->verify) {
			fd = __fd_get(dev, NULL);
			pr_info("Card %d fd %d\n", dev->card_no, fd);
			dev->drv_rc = rc = ioctl(fd, GENWQE_UNPIN_MEM, &m);
			dev->drv_errno = errno;
			if (0 == rc)
				return GENWQE_OK;
		}
	}
	pr_err("Dev: %p Fault: %d addr=%p size=%lld\n", dev,
	       dev->drv_errno, addr, (long long)size);
	return GENWQE_ERR_PINNING;
}

static int __genwqe_card_execute(card_handle_t dev,
				 struct genwqe_ddcb_cmd *req, int func)
{
	int	rc, fd, fd2, card_num;
	struct	genwqe_ddcb_cmd *cmd;
	struct	timeval ts, te;	/* Start and End time */
	struct lib_data_t *ld = &lib_data;

	if (NULL == dev)
		return GENWQE_ERR_EXEC_DDCB;
	if (dev != dev->verify)
		return GENWQE_ERR_EXEC_DDCB;

	gettimeofday(&ts, NULL);
	fd = __fd_get(dev, &card_num);
	cmd = req;
	while (cmd != NULL) {
	retry:			/* wait until DDCB is processed */
		rc = ioctl(fd, func, cmd);
		dev->drv_errno = errno;
		dev->drv_rc = rc;
		if (rc < 0) {
			/*
			 * Check all filedescriptors and close the
			 * non-working ones. Retrying makes only sense
			 * with a valid list of working cards. If this
			 * is not done, it happened that we retried
			 * with an card in trouble ...
			 */
			sem_post(&ld->health_sem);

			if (GENWQE_CARD_REDUNDANT == dev->card_no) {
				/*
				 * We can try to use next card in case
				 * of Busy or error if Multi mode was
				 * enabled.
				 */
				gettimeofday(&te, NULL);
				if ((te.tv_sec - ts.tv_sec) >
				    CONFIG_RETRY_TIMEOUT) { /* Timeout */
					pr_warn("%s exit Timeout fault: %d "
						"fd: %d\n",
						__func__, errno, fd);
					return GENWQE_ERR_EXEC_DDCB;
				}

				/* next fd from queue */
				fd2 = __fd_get(dev, &card_num);
				if (fd2 != fd)	     /* if there is a new fd */
					fd = fd2;    /* swap to new fd */
				else
					usleep(1000000);/* no fd in queue */

				card_retried_ddcbs[card_num]++;
				goto retry;	     /* and retry again */
			}
			if (errno == EBUSY) {
				card_retried_ddcbs[card_num]++;
				goto retry;
			}
			pr_err("%s exit fault: %d fd: %d rc: %d card_no: %d\n",
			       __func__, errno, fd, rc, dev->card_no);

			return GENWQE_ERR_EXEC_DDCB;
		}
		card_completed_ddcbs[card_num]++;
		cmd = (struct genwqe_ddcb_cmd *)(unsigned long)cmd->next_addr;
	}

	return GENWQE_OK;
}

/**
 * @brief	Execute a DDCB request with no DMA buffer translations.
 * @param card	handle returned from 'card_open()'
 * @param req	req describes the DDCB which should be executed.
 */
int genwqe_card_execute_raw_ddcb(card_handle_t dev,
				 struct genwqe_ddcb_cmd *req)
{
	return __genwqe_card_execute(dev, req, GENWQE_EXECUTE_RAW_DDCB);
}

/**
 * @brief	Execute a DDCB request with automatic DMA buffer translations.
 * @param card	handle returned from 'card_open()'
 * @param req	req describes the DDCB which should be executed.
 */
int genwqe_card_execute_ddcb(card_handle_t dev,
			     struct genwqe_ddcb_cmd *req)
{
	return __genwqe_card_execute(dev, req, GENWQE_EXECUTE_DDCB);
}

/**
 * @brief Get contiguous DMAable memory (usage instead of SG list)
 *
 * Allocating memory via the driver will always result in page alinged
 * memory. Since this is a feature, we use memalign to mimic the same
 * for simulation mode.
 */
void *genwqe_card_malloc(card_handle_t dev, size_t size)
{
	void *buf;

	if (NULL == dev)
		return NULL;

	if (dev != dev->verify)
		return NULL;

	if (GENWQE_CARD_REDUNDANT == dev->card_no)
		return NULL;

	if (INVALID_FD != dev->fd_s) /* normal operation */
		buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   dev->fd_s, 0);
	else {			/* simulation mode */
		unsigned int page_size = sysconf(_SC_PAGESIZE);
		buf = memalign(page_size, size);
	}

	if (buf == MAP_FAILED) {
		pr_err("%s size %d errno: %d/%s\n", __func__, (int)size,
		       errno, strerror(errno));
		return NULL;
	}

	return buf;
}

/**
 * @brief Free driver/device allocated DMAable memory.
 */
int genwqe_card_free(card_handle_t dev __attribute__((unused)),
		     void *ptr, size_t size)
{
	int rc;

	if (NULL == dev)
		return GENWQE_ERR_INVAL;
	if (GENWQE_CARD_REDUNDANT == dev->card_no)
		return GENWQE_ERR_INVAL;

	if (INVALID_FD != dev->fd_s) { /* normal operation */
		rc = munmap(ptr, size);
		if (rc == -1) {
			pr_err("%s: %p Size: %d Errno: %d\n",
				__func__, ptr, (int)size, errno);
			return GENWQE_ERRNO;
		}
	} else	/* simulation mode */
		free(ptr);
	return 0;
}

void *genwqe_card_alloc_scb(card_handle_t card, size_t size)
{
	void *scb;

	scb = genwqe_card_malloc(card, size);
	if (scb == NULL)
		return NULL;

	memset(scb, 0, size);
	return scb;
}

/**
 * card_set_ats_flags() - Set ATS flags correctly for data/pointers
 *                         at offset offs.
 *
 * Each 4-bit in the ATS array corresponds to 8 bytes in the scb. The
 * 1st ATS bits describe the ATS array itself and must therefore be
 * plain data read-only: ATS_TYPE_DATA. The remaining bits can
 * identify plan data read-only or rw, sgl version 1 or sgl version 2,
 * or even a scb read-only or rw itself. Recursion avoidance when
 * parsing this is likely a good idea to avoid loops.
 */
int genwqe_card_set_ats_flags(void *scb, size_t size, size_t offs, int flags)
{
	uint8_t *ats_array = scb;    /* ATS fields start at the beginning */
	unsigned int ats, idx;
	const uint8_t mask[2] = { 0xf0, 0x0f };

	if (((unsigned long)scb % 8) || /* 8 byte start addr alignment */
	    (size % 8)		     || /* 8 byte size alignment required */
	    (offs % 8)		     || /* 8 byte offset alignment required */
	    (offs > size - 8))
		return GENWQE_ERR_INVAL; /* offset must not exceed size */

	/*
	 * Let's try to represent ATS[n] a byte array. Each
	 * byte/8-bits contain 2 4-bit entries in this case.
	 * IBM bit notation requires starting at the MSB first.
	 * This should result in the following example:
	 *
	 * offs    mask                                             ATS IDX
	 * -----------------------------------------------------------------
	 *    0   0xf000_0000_0000_0000 0x0000_0000_0000_0000 ...  0   0
	 *    8   0x0f00_0000_0000_0000 0x0000_0000_0000_0000 ...  0   1
	 *   16   0x00f0_0000_0000_0000 0x0000_0000_0000_0000 ...  1   0
	 *   24   0x000f_0000_0000_0000 0x0000_0000_0000_0000 ...  1   1
	 *   32   0x0000_f000_0000_0000 0x0000_0000_0000_0000 ...  2   0
	 *   40   0x0000_0f00_0000_0000 0x0000_0000_0000_0000 ...  2   1
	 *         ...
	 */
	ats = offs / 16;
	idx = (offs / 8) & 0x1;
	ats_array[ats] &= ~mask[idx];	/* while out previous setting */
	if (idx == 0)			/* shift flags to correct position */
		ats_array[ats] |= (0x0f & flags) << 4;
	else    ats_array[ats] |= (0x0f & flags);

	return GENWQE_OK;
}

int genwqe_card_free_scb(card_handle_t card, void *scb, size_t size)
{
	return genwqe_card_free(card, scb, size);
}

static void __hexdump(FILE *fp, const void *buff, unsigned int size)
{
	unsigned int i;
	const uint8_t *b = (uint8_t *)buff;

	for (i = 0; i < size; i++) {
		if ((i & 0x0f) == 0x00)
			fprintf(fp, " %p: ", &b[i]);
		fprintf(fp, " %02x", b[i]);
		if ((i & 0x0f) == 0x0f)
			fprintf(fp, "\n");
	}
	fprintf(fp, "\n");
}

/**
 * @brief		DDCB dump function
 *
 * @param fp		error device
 * @param buff		DDCB buffer
 * @param size		size of bytes to dump
 */
void genwqe_hexdump(FILE *fp, const void *buff, unsigned int size)
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

static int __genwqe_flash_read(card_handle_t dev,
			       char partition,
			       uint8_t *buf,
			       int buflen,
			       uint16_t *retc,
			       uint16_t *attn,
			       uint32_t *progr)
{
	struct genwqe_bitstream load;

	if (NULL == dev)
		return GENWQE_ERR_CARD;
	if (GENWQE_CARD_REDUNDANT == dev->card_no)
		return GENWQE_ERR_CARD;

	memset(&load, 0, sizeof(load));
	load.target_addr = 0x0;	/* addr to start flashing */
	load.uid = 0x01;	/* get data from host */
	load.partition = (uint8_t)partition; /* '0', '1', 'v' */
	load.data_addr = (unsigned long)buf; /* vaddr of data to flash */
	load.size = buflen;

	memset(buf, 0, buflen); /* ensure buffer is filled up with 0s */
	dev->drv_rc = ioctl(dev->fd_s, GENWQE_SLU_READ, &load);
	dev->drv_errno = errno;

	/* copy potential  results even before we check the return code */
	if (retc)
		*retc  = load.retc;
	if (attn)
		*attn  = load.attn;
	if (progr)
		*progr = load.progress;

	if (dev->drv_rc != 0)
		return GENWQE_ERR_CARD;

	return GENWQE_OK;
}

int genwqe_flash_read(card_handle_t dev, struct card_upd_params *upd)
{
	int rc, fd, buflen;
	uint8_t *buf;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	/* we need page aligned start and length */
	buflen = (upd->flength + page_size) & ~(page_size - 1);
	buf = memalign(page_size, buflen);
	if (!buf)
		return GENWQE_ERRNO;

	/* open image file */
	fd = open(upd->fname, O_EXCL|O_CREAT|O_WRONLY|O_TRUNC, 0644);
	if (fd < 0) {
		rc = GENWQE_ERR_FLASH_READ;
		goto err_exit;
	}

	rc = __genwqe_flash_read(dev, upd->partition, buf, buflen,
				 &upd->retc, &upd->attn,
				 &upd->progress);
	if (rc < 0)
		goto err_exit;

	rc = (int)write(fd, buf, (size_t)upd->flength);
	close(fd);

	if (rc != (int)upd->flength) {
		rc = GENWQE_ERR_FLASH_READ;
		goto err_exit;
	}
	rc = GENWQE_OK;

 err_exit:
	free(buf);	/* buffer is needed until DDCB is processed */
	return rc;
}

static int __genwqe_flash_update(card_handle_t card,
				 char partition,
				 const uint8_t *buf,
				 int buflen,
				 uint16_t *retc,
				 uint16_t *attn,
				 uint32_t *progr)
{
	struct genwqe_bitstream load;

	memset(&load, 0, sizeof(load));
	load.target_addr = 0x0; /* addr to start flashing */
	load.uid = 0x01;    /* get data from host */
	load.partition = (uint8_t)partition; /* '0', '1', 'v' */
	load.data_addr = (unsigned long)buf; /* vaddr of data to flash */
	load.size = buflen;

	card->drv_rc = ioctl(card->fd_s, GENWQE_SLU_UPDATE, &load);
	card->drv_errno = errno;

	/* copy potential  results even before we check the return code */
	if (retc)
		*retc  = load.retc;
	if (attn)
		*attn  = load.attn;
	if (progr)
		*progr = load.progress;

	if (card->drv_rc < 0)
		return GENWQE_ERR_CARD;

	return GENWQE_OK;
}

int genwqe_flash_update(card_handle_t dev, struct card_upd_params *upd,
		       int verify)
{
	int rc = GENWQE_OK;
	struct stat filestat;
	struct genwqe_bitstream load;
	uint8_t *buf;
	int fd, buflen;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	if (NULL == dev)
		return GENWQE_ERR_INVAL;

	if (GENWQE_CARD_REDUNDANT == dev->card_no)
		return GENWQE_ERR_INVAL; /* Cannot do this in Redundant mode */

	memset(&load, 0, sizeof(load));
	upd->flength = 0;

	fd = open(upd->fname, O_RDONLY);
	if (fd < 0)
		return GENWQE_ERRNO;

	rc = fstat(fd, &filestat);
	if (rc < 0) {
		close(fd);
		return GENWQE_ERRNO;
	}

	upd->flength = filestat.st_size;
	/* setup page aligned buffer for image data */

	/* we need page aligned start and length */
	buflen = (filestat.st_size + page_size) & ~(page_size - 1);

	buf = memalign(page_size, 2 * buflen);
	if (!buf) {
		close(fd);
		return GENWQE_ERRNO;
	}

	memset(buf, 0, 2 * buflen); /* ensure buffer is filled up with 0s */
	load.slu_id = upd->slu_id;
	load.app_id = upd->app_id;
	load.target_addr = 0x0;	/* addr to start flashing */
	load.uid = 0x01;	/* get data from host */
	load.partition = (uint8_t)upd->partition; /* '0', '1', 'v' */
	load.data_addr = (unsigned long)buf; /* vaddr of data to flash */
	load.size = filestat.st_size; /* size of data to flash */

	/* read image file */
	rc = (int)read(fd, (void *)(unsigned long)load.data_addr,
		       (size_t)load.size);
	close(fd);
	if (rc != (int)load.size) {
		free(buf);
		return GENWQE_ERRNO;
	}

	/* checksum across complete file */
	load.crc = genwqe_ddcb_crc32((void *)(unsigned long)load.data_addr,
				     load.size, (uint32_t)-1);

	dev->drv_rc = rc = ioctl(dev->fd_s, GENWQE_SLU_UPDATE, &load);
	dev->drv_errno = errno;

	/* copy potential  results even before we check the return code */
	upd->retc = load.retc;
	upd->attn = load.attn;
	upd->progress = load.progress;

	if (rc < 0) {
		free(buf);
		return GENWQE_ERRNO;
	}

	if (verify) {
		unsigned int i;
		uint8_t *vbuf = buf + buflen; /* we allocated more space */

		rc = __genwqe_flash_read(dev, upd->partition, vbuf, buflen,
				&upd->retc, &upd->attn, &upd->progress);
		if (rc < 0) {
			free(buf);
			return rc;
		}
		for (i = 0; i < upd->flength; i++) {
			if (buf[i] != vbuf[i]) {
				pr_err("compare mismatch offs %d:\n", i);
				__hexdump(stderr, &buf[i], 32);
				pr_err("read:\n");
				__hexdump(stderr, &vbuf[i], 32);
				free(buf);
				return GENWQE_ERR_FLASH_VERIFY;
			}
		}
	}
	pr_info("%s update done\n", __func__);
	free(buf);	/* buffer is needed until DDCB is processed */
	return rc;
}

int genwqe_read_vpd(card_handle_t card, genwqe_vpd *vpd)
{
	int rc;
	uint16_t retc, attn;
	uint32_t progr;
	size_t buflen;
	uint8_t *buf;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	if (NULL == card)
		return GENWQE_ERR_INVAL;

	if (GENWQE_CARD_REDUNDANT == card->card_no)
		return GENWQE_ERR_INVAL; /* Cannot do this in Redundant mode */

	/* buffer for __genwqe_flash_read() must be page aligned */
	buflen = (sizeof(*vpd) + page_size) & ~(page_size - 1);
	buf = memalign(page_size, buflen);
	if (!buf)
		return GENWQE_ERRNO;

	rc = __genwqe_flash_read(card, 'v', buf, buflen, &retc, &attn, &progr);
	if (rc < 0) {
		pr_err("reading VPD failed retc=%03x attn=%x progr=%x "
			"rc=%d drv_rc=%d drv_errno=%d\n", retc, attn, progr,
			rc, card->drv_rc, card->drv_errno);
		goto err_exit;
	}
	memcpy(vpd, buf, sizeof(*vpd));
err_exit:
	free(buf);
	return rc;
}

int genwqe_write_vpd(card_handle_t card, const genwqe_vpd *vpd)
{
	int rc;
	uint16_t retc, attn;
	uint32_t progr;
	size_t buflen;
	uint8_t *buf;
	unsigned int page_size = sysconf(_SC_PAGESIZE);

	if (NULL == card)
		return GENWQE_ERR_INVAL;

	if (GENWQE_CARD_REDUNDANT == card->card_no)
		return GENWQE_ERR_INVAL; /* Cannot do this in Redundant mode */

	/* buffer for __genwqe_flash_read() must be page aligned */
	buflen = (sizeof(*vpd) + page_size) & ~(page_size - 1);
	buf = memalign(page_size, buflen);
	if (!buf)
		return GENWQE_ERRNO;

	memcpy(buf, vpd, sizeof(*vpd));
	rc = __genwqe_flash_update(card, 'v', buf, buflen,
			&retc, &attn, &progr);
	if (rc < 0) {
		pr_err("writing VPD failed retc=%03x attn=%x progr=%x "
			"rc=%d drv_rc=%d drv_errno=%d\n", retc, attn, progr,
			rc, card->drv_rc, card->drv_errno);
	}
	free(buf);
	return rc;
}

/**
 * @brief		extended error handling
 *			print versions and dump DDCB data
 */
void genwqe_print_debug_data(FILE *fp, struct genwqe_debug_data *debug_data,
			     int flags)
{
	if (debug_data == NULL)
		return;

	if (flags & GENWQE_DD_IDS)
		fprintf(fp, "driver:%s SLU/APP: %016llx.%016llx\n\n",
			debug_data->driver_version,
			(long long)debug_data->slu_unitcfg,
			(long long)debug_data->app_unitcfg);

	if (flags & GENWQE_DD_DDCB_BEFORE) {
		fprintf(fp, "ddcb before processing:\n");
		genwqe_hexdump(fp, &debug_data->ddcb_before,
			       sizeof(debug_data->ddcb_before));
	}

	if (flags & GENWQE_DD_DDCB_PREVIOUS) {
		fprintf(fp, "ddcb previous:\n");
		genwqe_hexdump(fp, &debug_data->ddcb_prev,
			       sizeof(debug_data->ddcb_prev));
	}

	if (flags & GENWQE_DD_DDCB_PROCESSED) {
		fprintf(fp, "ddcb processed:\n");
		genwqe_hexdump(fp, &debug_data->ddcb_finished,
			       sizeof(debug_data->ddcb_finished));
	}
}

static void libcard_init(void) __attribute__((constructor));
static void libcard_exit(void) __attribute__((destructor));

/* constructor */
static void libcard_init(void)
{
	int rc, i;
	struct lib_data_t *ld = &lib_data;

	ddcb_setup_crc32(ld);

	rc = pthread_mutex_init(&ld->fds_mutex, NULL);
	if (rc != 0)
		pr_err("initializing mutex failed!\n");

	/* Init the rest so i do not need to call memset */
	ld->thread_rc = 0;
	ld->thread_id = -1;	// No tid
	ld->fd_m_count = 0;	// No Multi fd's in list
	ld->fd_s_count = 0;	// No Single fd's in List
	ld->m_mode_save = -1;	// Not Set yet

	for (i = 0; i < MAX_FUNC_NUM; i++)	// Clear out Multi List
		ld->genwqe_state[i] = CARD_CLOSED;

	m_dev_head = NULL;
	s_dev_head = NULL;

	/* some more for inotify */
	ld->inotify_rc = 1;	/* Set to some other value than 0 */
	ld->inotify_tid = -1;	/* No thread id */
	ld->inotify_event = INOTIFY_IDLE;
}

/* destructor */
static void libcard_exit(void)
{
	struct lib_data_t *ld = &lib_data;
	card_handle_t dev;

	pr_info("%s Enter (s:%p m:%p fd:%p)\n",
		__func__, s_dev_head, m_dev_head, __fd_m_list);

	pthread_mutex_lock(&ld->fds_mutex);
	dev = (struct card_dev_t *)s_dev_head;
	while (dev) {
		pr_info("Request Single List: %p to close.\n", dev);
		dev->dev_state = DEV_REQ_CLOSE;
		dev = dev->next;
	}
	dev = (struct card_dev_t *)m_dev_head;
	while (dev) {
		pr_info("Request Multi List: %p to close.\n", dev);
		dev->dev_state = DEV_REQ_CLOSE;
		dev = dev->next;
	}
	pthread_mutex_unlock(&ld->fds_mutex);

	if (ld->inotify_tid != (pthread_t)-1) {
		/* Send kill Signal to inotify thread */
		pthread_cancel(ld->inotify_tid);
		/* and wait to Join */
		pthread_join(ld->inotify_tid, NULL);
		ld->inotify_tid = -1;
		inotify_rm_watch(ld->inotify_fd, ld->inotify_wd);
	}
	if (ld->thread_id != (pthread_t)-1) {
		/* Send kill Signal to inotify thread */
		pthread_cancel(ld->thread_id);
		/* and wait to Join */
		pthread_join(ld->thread_id, NULL);
		ld->thread_id = -1;
	}

	pthread_mutex_lock(&ld->fds_mutex);
	__fixup_fd_lists(ld);
	pthread_mutex_unlock(&ld->fds_mutex);

	pthread_mutex_destroy(&ld->fds_mutex);
	pr_info("%s EXIT (s:%p m:%p fd:%p)\n",
		__func__, s_dev_head, m_dev_head, __fd_m_list);
}

/*
 ** @brief		Overwrite slu id in Card control block
 *
 * @param card		the Pointer to the card device
 * @param slu_id	Slu id value for overwrite
 */
void card_overwrite_slu_id(card_handle_t dev, uint64_t slu_id)
{
	if (dev)
		dev->slu_id = slu_id;
}

/*
 ** @brief		Overwrite appl id in card control block
 *
 * @param card		the pointer to card device
 * @param app_id	Value of app id for Overwrite
 */
void card_overwrite_app_id(card_handle_t dev, uint64_t app_id)
{
	if (dev)
		dev->app_id = app_id;
}

/*
 ** @brief		Get Card appl id from card control block
 *
 * @param card		the pointer to card device
 * @return		App ID from this dev (e.g. 0x00000002475a4950)
 */
uint64_t card_get_app_id(card_handle_t dev)
{
	if (dev)
		return dev->app_id;
	return 0;
}

int genwqe_dump_statistics(FILE *fp)
{
	int card_num;

	fprintf(fp, "GenWQE card statistics\n");
	for (card_num = 0; card_num < NUM_CARDS; card_num++) {
		if ((card_completed_ddcbs[card_num] == 0) &&
		    (card_retried_ddcbs[card_num] == 0))
			continue;
		fprintf(fp,
			"  genwqe%u_card completed DDCBs: %5d retried: %5d\n",
			card_num, card_completed_ddcbs[card_num],
			card_retried_ddcbs[card_num]);
	}
#if defined(CONFIG_USE_SIGNAL)
	fprintf(fp,"  Health SIGIO:    %d\n", card_health_signal);
#endif
	return 0;
}
