#ifndef __CARD_BASE_H__
#define __CARD_BASE_H__

/**
 * IBM Accelerator Family 'CGzip'
 *
 * (C) Copyright IBM Corp. 2015
 *
 * Author: Frank Haverkamp <haver@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/stringify.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/version.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/smp.h>
#include <misc/cxl.h>

#define DRV_VERSION		"0.1"
#define CGZIP_DEVNAME		"cgzip"
#define PCI_DEVICE_CGZIP	0x0602 /* 0x044c */ /* Cgzip DeviceID */
#define PCI_CLASSCODE_CGZIP	0x1200 /* Accelerator */
#define CGZIP_CARD_NO_MAX	4

#define MMIO_IMP_VERSION_REG	0x0000000ull
#define MMIO_APP_VERSION_REG	0x0000008ull
#define MMIO_AFU_CONFIG_REG	0x0000010ull
#define MMIO_AFU_STATUS_REG	0x0000018ull
#define MMIO_AFU_COMMAND_REG	0x0000020ull
#define MMIO_FRT_REG		0x0000080ull

#define MMIO_DDCBQ_START_REG	0x0000100ull
#define MMIO_DDCBQ_CONFIG_REG	0x0000108ull
#define MMIO_DDCBQ_COMMAND_REG	0x0000110ull
#define MMIO_DDCBQ_STATUS_REG	0x0000118ull
#define MMIO_DDCBQ_CID_REG	0x0000120ull /* Context ID REG */
#define MMIO_DDCBQ_WT_REG	0x0000180ull

#define MMIO_FIR_REGS_BASE	0x0001000ull /* FIR: 1000...1028 */
#define MMIO_FIR_REGS_NUM	6

#define MMIO_ERRINJ_MMIO_REG	0x0001800ull
#define MMIO_ERRINJ_GZIP_REG	0x0001808ull

#define MMIO_AGRV_REGS_BASE	0x0002000ull
#define MMIO_AGRV_REGS_NUM	16

#define MMIO_GZIP_REGS_BASE	0x0002100ull
#define MMIO_GZIP_REGS_NUM	16

#define MMIO_DEBUG_REG		0x000FF00ull

struct cgzip_dev {
	int card_idx;			/* card index 0..CARD_NO_MAX-1 */

	/* char device */
	dev_t  devnum_cgzip;		/* major/minor num card */
	struct class *class_cgzip;	/* reference to class object */
	struct device *dev;		/* for device creation */
	struct cdev cdev_cgzip;	/* char device for card */
	struct dentry *debugfs_root;	/* debugfs card root directory */
	struct dentry *debugfs_cgzip;	/* debugfs driver root directory */

	/* CAPI stuff */
	struct cxl_context *ctx;
	void __iomem *iomem;

	/* pci resources */
	struct pci_dev *pci_dev;	/* PCI device */
	void __iomem *mmio;		/* BAR-0 MMIO start */
	unsigned long mmio_len;
};

/**
 * struct cgzip_file - Information for open Cgzip devices
 * @map_list:        keep information about different type of memory.
 *                   E.g. user memory, pinned user memory, driver allocated
 *                   memory, or memory for SGLv2.
 */
struct cgzip_file {
	struct cgzip_dev *cd;
	struct file *filp;

	struct fasync_struct *async_queue;
	struct task_struct *owner;
};

#endif	/* __CARD_BASE_H__ */
