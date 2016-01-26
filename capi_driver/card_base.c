/**
 * IBM Accelerator Family 'CAPI/Gzip'
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

/*
 * Module initialization and PCIe setup. Card health monitoring and
 * recovery functionality. Character device creation and deletion are
 * controlled from here.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/err.h>
#include <linux/aer.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/device.h>
#include <linux/log2.h>

#include "card_base.h"

MODULE_AUTHOR("Frank Haverkamp <haver@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("CAPI Gzip Card");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");

static char cgzip_driver_name[] = CGZIP_DEVNAME;
static struct class *class_cgzip;
static struct dentry *debugfs_cgzip;
static struct cgzip_dev *cgzip_devices[CGZIP_CARD_NO_MAX];

/* PCI structure for identifying device by PCI vendor and device ID */
static const struct pci_device_id cgzip_device_table[] = {
	{ PCI_VDEVICE(IBM, PCI_DEVICE_CGZIP) },
	{ 0, }			/* 0 terminated list. */
};

MODULE_DEVICE_TABLE(pci, cgzip_device_table);

/**
 * cgzip_dev_alloc() - Create and prepare a new card descriptor
 *
 * Return: Pointer to card descriptor, or ERR_PTR(err) on error
 */
static struct cgzip_dev *cgzip_dev_alloc(void)
{
	unsigned int i = 0;
	struct cgzip_dev *cd;

	for (i = 0; i < CGZIP_CARD_NO_MAX; i++) {
		if (cgzip_devices[i] == NULL)
			break;
	}
	if (i >= CGZIP_CARD_NO_MAX)
		return ERR_PTR(-ENODEV);

	cd = kzalloc(sizeof(struct cgzip_dev), GFP_KERNEL);
	if (!cd)
		return ERR_PTR(-ENOMEM);

	cd->card_idx = i;
	cd->class_cgzip = class_cgzip;
	cd->debugfs_cgzip = debugfs_cgzip;
	cgzip_devices[i] = cd;

	return cd;
}

static void cgzip_dev_free(struct cgzip_dev *cd)
{
	if (!cd)
		return;

	cgzip_devices[cd->card_idx] = NULL;
	kfree(cd);
}

/**
 * cgzip_pci_setup() - Allocate PCIe related resources for our card
 */
static int cgzip_pci_setup(struct cgzip_dev *cd)
{
	struct pci_dev *pci_dev = cd->pci_dev;

	pci_set_master(pci_dev);
	return 0;
}

/**
 * cgzip_pci_remove() - Free PCIe related resources for our card
 */
static void cgzip_pci_remove(struct cgzip_dev *cd)
{
	struct pci_dev *pci_dev = cd->pci_dev;

	dev_err(&pci_dev->dev, "[%s]\n", __func__);
}

static void afu_print_status(struct cgzip_dev *cd)
{
	int i, rc;
	unsigned int offs;
	struct pci_dev *pci_dev = cd->pci_dev;

	for (offs = 0; offs < 16; offs += 4) {
		u32 val32;

		rc = pci_read_config_dword(pci_dev, offs, &val32);
		dev_info(&pci_dev->dev,
			 " pci_read_config_dword[%02x]: %08x\n",
			 offs, val32);
	}

	dev_info(&pci_dev->dev, " Version Reg:        0x%016llx\n",
		readq_be(cd->iomem + MMIO_IMP_VERSION_REG));

	dev_info(&pci_dev->dev, " Appl. Reg:          0x%016llx\n",
		readq_be(cd->iomem + MMIO_APP_VERSION_REG));

	dev_info(&pci_dev->dev, " Afu Config Reg:     0x%016llx\n",
		readq_be(cd->iomem + MMIO_AFU_CONFIG_REG));

	dev_info(&pci_dev->dev, " Afu Status Reg:     0x%016llx\n",
		readq_be(cd->iomem + MMIO_AFU_STATUS_REG));

	dev_info(&pci_dev->dev, " Afu Cmd Reg:        0x%016llx\n",
		readq_be(cd->iomem + MMIO_AFU_COMMAND_REG));

	dev_info(&pci_dev->dev, " Free Run Timer:     0x%016llx\n",
		readq_be(cd->iomem + MMIO_FRT_REG));

	dev_info(&pci_dev->dev, " DDCBQ Start Reg:    0x%016llx\n",
		readq_be(cd->iomem + MMIO_DDCBQ_START_REG));

	dev_info(&pci_dev->dev, " DDCBQ Conf Reg:     0x%016llx\n",
		readq_be(cd->iomem + MMIO_DDCBQ_CONFIG_REG));

	dev_info(&pci_dev->dev, " DDCBQ Cmd Reg:      0x%016llx\n",
		readq_be(cd->iomem + MMIO_DDCBQ_COMMAND_REG));

	dev_info(&pci_dev->dev, " DDCBQ Stat Reg:     0x%016llx\n",
		readq_be(cd->iomem + MMIO_DDCBQ_STATUS_REG));

	dev_info(&pci_dev->dev, " DDCBQ Context ID:   0x%016llx\n",
		readq_be(cd->iomem + MMIO_DDCBQ_CID_REG));

	dev_info(&pci_dev->dev, " DDCBQ WT Reg:       0x%016llx\n",
		readq_be(cd->iomem + MMIO_DDCBQ_WT_REG));

	for (i = 0; i < MMIO_FIR_REGS_NUM; i++) {
		unsigned long addr = MMIO_FIR_REGS_BASE + (u64)(i * 8);
		u64 reg = readq_be(cd->iomem + addr);

		dev_info(&pci_dev->dev, " FIR Reg [%08llx]: 0x%016llx\n",
			(long long)addr, (long long)reg);
	}
}

static irqreturn_t cgzip_irq_handler(int irq, void *data)
{
	struct cgzip_dev *cd = (struct cgzip_dev *)data;
	struct pci_dev *pci_dev = cd->pci_dev;

	dev_info(&pci_dev->dev, "CGzip Interrupt\n");
	afu_print_status(cd);

	return IRQ_HANDLED;
}

/**
 * cgzip_probe() - Device initialization
 * @pdev:	PCI device information struct
 *
 * Callable for multiple cards. This function is called on bind.
 *
 * Return: 0 if succeeded, < 0 when failed
 */
static int cgzip_probe(struct pci_dev *pci_dev, const struct pci_device_id *id)
{
	int rc;
	struct cgzip_dev *cd;

	pr_err("[%s] pci_dev=%p\n", __func__, pci_dev);

	cd = cgzip_dev_alloc();
	if (IS_ERR(cd)) {
		rc = PTR_ERR(cd);
		dev_err(&pci_dev->dev, "err: could not alloc mem %d!\n",
			rc);
		return rc;
	}

	dev_set_drvdata(&pci_dev->dev, cd);
	cd->pci_dev = pci_dev;

	rc = cgzip_pci_setup(cd);
	if (rc < 0) {
		dev_err(&pci_dev->dev,
			"err: problems with PCI setup rc=%d\n", rc);
		goto out_free_dev;
	}

	cd->ctx = cxl_dev_context_init(pci_dev);
	if (IS_ERR(cd->ctx)) {
		rc = PTR_ERR(cd->ctx);
		dev_err(&pci_dev->dev,
			"err: problems with cxl_dev_context_init rc=%d\n", rc);
		goto out_pci_remove;
	}

	cd->iomem = cxl_psa_map(cd->ctx);
	if (IS_ERR(cd->iomem)) {
		rc = PTR_ERR(cd->iomem);
		dev_err(&pci_dev->dev,
			"err: problems with cxl_psa_map rc=%d\n", rc);
		goto out_release_context;
	}

	rc = cxl_allocate_afu_irqs(cd->ctx, 1);
	if (unlikely(rc)) {
		dev_err(&pci_dev->dev,
			"%s: call to allocate_afu_irqs failed rc=%d!\n",
			__func__, rc);
		goto out_psa_unmap;
	}

	rc = cxl_map_afu_irq(cd->ctx, 1, cgzip_irq_handler, cd, "cxl-cgzip");
	if (unlikely(rc <= 0)) {
		dev_err(&pci_dev->dev,
			"%s: IRQ 1 (DDCB_QUEUE) map failed!\n", __func__);
		goto out_free_afu_irqs;
	}

	rc = cxl_start_context(cd->ctx, 0ull, NULL);
	if (rc != 0) {
		dev_err(&pci_dev->dev,
			"err: problems with cxl_start_context rc=%d\n", rc);
		goto out_cxl_unmap_afu_irq;
	}

	afu_print_status(cd);

	return 0;

 out_cxl_unmap_afu_irq:
	cxl_unmap_afu_irq(cd->ctx, 1, cd);
 out_free_afu_irqs:
	cxl_free_afu_irqs(cd->ctx);
 out_psa_unmap:
	cxl_psa_unmap(cd->iomem);
	cd->iomem = NULL;
 out_release_context:
	cxl_release_context(cd->ctx);
 out_pci_remove:
	cgzip_pci_remove(cd);
 out_free_dev:
	cgzip_dev_free(cd);
	return rc;
}

/**
 * cgzip_remove() - Called when device is removed (hot-plugable)
 *
 * Or when driver is unloaded respecitively when unbind is done.
 */
static void cgzip_remove(struct pci_dev *pci_dev)
{
	struct cgzip_dev *cd = dev_get_drvdata(&pci_dev->dev);

	pr_err("[%s] pci_dev=%p\n", __func__, pci_dev);

	cxl_stop_context(cd->ctx);

	cxl_unmap_afu_irq(cd->ctx, 1, cd);
	cxl_free_afu_irqs(cd->ctx);
	cxl_psa_unmap(cd->iomem);
	cd->iomem = NULL;
	cxl_release_context(cd->ctx);
	cgzip_pci_remove(cd);
	cgzip_dev_free(cd);
}

static struct pci_driver cgzip_driver = {
	.name	  = cgzip_driver_name,
	.id_table = cgzip_device_table,
	.probe	  = cgzip_probe,
	.remove	  = cgzip_remove,
};

/**
 * cgzip_init_module() - Driver registration and initialization
 */
static int __init cgzip_init_module(void)
{
	int rc;

	pr_err("[%s]\n", __func__);

	class_cgzip = class_create(THIS_MODULE, CGZIP_DEVNAME);
	if (IS_ERR(class_cgzip)) {
		pr_err("[%s] create class failed\n", __func__);
		return -ENOMEM;
	}

	debugfs_cgzip = debugfs_create_dir(CGZIP_DEVNAME, NULL);
	if (!debugfs_cgzip) {
		pr_err("[%s] create debugfs failed\n", __func__);
		rc = -ENOMEM;
		goto err_out;
	}

	rc = pci_register_driver(&cgzip_driver);
	if (rc != 0) {
		pr_err("[%s] pci_reg_driver (rc=%d)\n", __func__, rc);
		goto err_out0;
	}

	return rc;

 err_out0:
	debugfs_remove(debugfs_cgzip);
 err_out:
	class_destroy(class_cgzip);
	return rc;
}

/**
 * cgzip_exit_module() - Driver exit
 */
static void __exit cgzip_exit_module(void)
{
	pr_err("[%s]\n", __func__);

	pci_unregister_driver(&cgzip_driver);
	debugfs_remove(debugfs_cgzip);
	class_destroy(class_cgzip);
}

module_init(cgzip_init_module);
module_exit(cgzip_exit_module);
