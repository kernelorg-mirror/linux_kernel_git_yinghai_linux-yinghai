/*
 *  pci_bind.c - ACPI PCI Device Binding ($Revision: 2 $)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/acpi.h>
#include <linux/pm_runtime.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>

#define _COMPONENT		ACPI_PCI_COMPONENT
ACPI_MODULE_NAME("pci_bind");

static int acpi_pci_unbind(struct acpi_device *acpi_dev, struct device *dev)
{
	struct pci_dev *pdev = NULL;
	struct pci_bus *bus = NULL;

	if (dev_is_pci(dev)) {
		pdev = to_pci_dev(dev);
		if (pdev->subordinate)
			bus = pdev->subordinate;
	} else
			bus = to_pci_host_bridge(dev)->bus;

	if (acpi_dev) {
		device_set_run_wake(dev, false);
		if (pdev)
			pci_acpi_remove_pm_notifier(acpi_dev);
		else
			pci_acpi_remove_bus_pm_notifier(acpi_dev);
	}

	if (bus)
		acpi_pci_irq_del_prt(bus);

	return 0;
}

static int acpi_pci_bind(struct acpi_device *acpi_dev, struct device *dev)
{
	acpi_status status;
	struct pci_dev *pdev = NULL;
	struct pci_bus *bus = NULL;
	acpi_handle tmp_hdl;
	acpi_handle handle;

	if (dev_is_pci(dev)) {
		pdev = to_pci_dev(dev);
		if (pdev->subordinate)
			bus = pdev->subordinate;
		else
			bus = pdev->bus;
	} else
			bus = to_pci_host_bridge(dev)->bus;

	if (acpi_dev) {
		if (pdev)
			pci_acpi_add_pm_notifier(acpi_dev, pdev);
		else
			pci_acpi_add_bus_pm_notifier(acpi_dev, bus);
		if (acpi_dev->wakeup.flags.run_wake)
			device_set_run_wake(dev, true);
		handle = acpi_dev->handle;
	} else
		handle = DEVICE_ACPI_HANDLE(dev);

	/*
	 * Evaluate and parse _PRT, if exists.  This code allows parsing of
	 * _PRT objects within the scope of non-bridge devices.  Note that
	 * _PRTs within the scope of a PCI bridge assume the bridge's
	 * subordinate bus number.
	 *
	 * TBD: Can _PRTs exist within the scope of non-bridge PCI devices?
	 */
	status = acpi_get_handle(handle, METHOD_NAME__PRT, &tmp_hdl);
	if (ACPI_SUCCESS(status))
		acpi_pci_irq_add_prt(handle, bus);

	return 0;
}

void acpi_pci_bind_notify(struct acpi_device *acpi_dev, struct device *dev,
			  bool bind)
{
	if (dev_is_pci(dev) || dev_is_pci_host_bridge(dev)) {
		if (bind)
			acpi_pci_bind(acpi_dev, dev);
		else
			acpi_pci_unbind(acpi_dev, dev);
	}
}
