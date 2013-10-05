/*
 * IOAPIC/IOxAPIC/IOSAPIC driver
 *
 * Copyright (C) 2009 Fujitsu Limited.
 * (c) Copyright 2009 Hewlett-Packard Development Company, L.P.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * This driver manages PCI I/O APICs added by hotplug after boot.  We try to
 * claim all I/O APIC PCI devices, but those present at boot were registered
 * when we parsed the ACPI MADT, so we'll fail when we try to re-register
 * them.
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <acpi/acpi_bus.h>

struct acpi_pci_ioapic {
	acpi_handle	root_handle;
	u32		gsi_base;
	struct pci_dev *pdev;
	struct list_head list;
};

static LIST_HEAD(ioapic_list);
static DEFINE_MUTEX(ioapic_list_lock);

static acpi_status setup_res(struct acpi_resource *acpi_res, void *data)
{
	struct resource *res;
	struct acpi_resource_address64 addr;
	acpi_status status;
	unsigned long flags;
	u64 start, end;

	status = acpi_mem_addr_resource_to_address64(acpi_res, &addr);
	if (!ACPI_SUCCESS(status))
		return AE_OK;

	if (addr.resource_type == ACPI_MEMORY_RANGE) {
		if (addr.info.mem.caching == ACPI_PREFETCHABLE_MEMORY)
			return AE_OK;
		flags = IORESOURCE_MEM;
	} else
		return AE_OK;

	start = addr.minimum + addr.translation_offset;
	end = addr.maximum + addr.translation_offset;

	res = data;
	res->flags = flags;
	res->start = start;
	res->end = end;

	return AE_OK;
}

static void handle_ioapic_add(acpi_handle handle, struct pci_dev **pdev,
				 u32 *pgsi_base)
{
	acpi_status status;
	unsigned long long gsb;
	struct pci_dev *dev;
	u32 gsi_base;
	int ret;
	char *type;
	struct resource r;
	struct resource *res = &r;
	char objname[64];
	struct acpi_buffer buffer = {sizeof(objname), objname};

	*pdev = NULL;
	*pgsi_base = 0;

	status = acpi_evaluate_integer(handle, "_GSB", NULL, &gsb);
	if (ACPI_FAILURE(status) || !gsb)
		return;

	dev = acpi_get_pci_dev(handle);
	if (!dev || !pci_resource_len(dev, 0)) {
		struct acpi_device_info *info;
		char *hid = NULL;

		status = acpi_get_object_info(handle, &info);
		if (ACPI_FAILURE(status))
			goto exit_put;
		if (info->valid & ACPI_VALID_HID)
			hid = info->hardware_id.string;
		if (!hid || strcmp(hid, "ACPI0009")) {
			kfree(info);
			goto exit_put;
		}
		kfree(info);
		memset(res, 0, sizeof(*res));
		acpi_walk_resources(handle, METHOD_NAME__CRS, setup_res, res);
		if (!res->flags)
			goto exit_put;
	}

	acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	gsi_base = gsb;
	type = "IOxAPIC";
	if (dev) {
		ret = pci_enable_device(dev);
		if (ret < 0)
			goto exit_put;

		pci_set_master(dev);

		if (dev->class == PCI_CLASS_SYSTEM_PIC_IOAPIC)
			type = "IOAPIC";

		if (pci_resource_len(dev, 0)) {
			if (pci_request_region(dev, 0, type))
				goto exit_disable;

			res = &dev->resource[0];
		}
	}

	if (acpi_register_ioapic(handle, res->start, gsi_base)) {
		if (dev)
			goto exit_release;
		return;
	}

	pr_info("%s %s %s at %pR, GSI %u\n",
		dev ? dev_name(&dev->dev) : "", objname, type,
		res, gsi_base);

	*pdev = dev;
	*pgsi_base = gsi_base;
	return;

exit_release:
	pci_release_region(dev, 0);
exit_disable:
	pci_disable_device(dev);
exit_put:
	pci_dev_put(dev);
}

static void handle_ioapic_remove(acpi_handle handle, struct pci_dev *dev,
				  u32 gsi_base)
{
	acpi_unregister_ioapic(handle, gsi_base);

	if (!dev)
		return;

	pci_release_region(dev, 0);
	pci_disable_device(dev);
	pci_dev_put(dev);
}

static acpi_status register_ioapic(acpi_handle handle, u32 lvl,
					void *context, void **rv)
{
	acpi_handle root_handle = context;
	struct pci_dev *pdev;
	u32 gsi_base;
	struct acpi_pci_ioapic *ioapic;

	handle_ioapic_add(handle, &pdev, &gsi_base);
	if (!gsi_base)
		return AE_OK;

	ioapic = kzalloc(sizeof(*ioapic), GFP_KERNEL);
	if (!ioapic) {
		pr_err("%s: cannot allocate memory\n", __func__);
		handle_ioapic_remove(root_handle, pdev, gsi_base);
		return AE_OK;
	}
	ioapic->root_handle = root_handle;
	ioapic->pdev = pdev;
	ioapic->gsi_base = gsi_base;

	mutex_lock(&ioapic_list_lock);
	list_add(&ioapic->list, &ioapic_list);
	mutex_unlock(&ioapic_list_lock);

	return AE_OK;
}

void acpi_pci_ioapic_add(struct acpi_pci_root *root)
{
	acpi_status status;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, root->device->handle,
				     (u32)1, register_ioapic, NULL,
				     root->device->handle,
				     NULL);
	if (ACPI_FAILURE(status))
		pr_err("%s: register_ioapic failure - %d", __func__, status);
}

void acpi_pci_ioapic_remove(struct acpi_pci_root *root)
{
	struct acpi_pci_ioapic *ioapic, *tmp;

	mutex_lock(&ioapic_list_lock);
	list_for_each_entry_safe(ioapic, tmp, &ioapic_list, list) {
		if (root->device->handle != ioapic->root_handle)
			continue;
		list_del(&ioapic->list);
		handle_ioapic_remove(ioapic->root_handle, ioapic->pdev,
					ioapic->gsi_base);
		kfree(ioapic);
	}
	mutex_unlock(&ioapic_list_lock);
}
