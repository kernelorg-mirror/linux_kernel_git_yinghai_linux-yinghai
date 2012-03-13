/* Core PCI functionality used only by PCI hotplug */

#include <linux/pci.h>
#include <linux/export.h>
#include "pci.h"

int __ref pci_hp_add_bridge(struct pci_dev *dev)
{
	struct pci_bus *parent = dev->bus;
	int pass, busnr = parent->secondary;

	for (pass = 0; pass < 2; pass++)
		busnr = pci_scan_bridge(parent, dev, busnr, pass);
	if (!dev->subordinate)
		return -1;

	return 0;
}
EXPORT_SYMBOL(pci_hp_add_bridge);

unsigned int __devinit pci_do_scan_bus(struct pci_bus *bus)
{
	unsigned int max;

	max = pci_scan_child_bus(bus);

	/*
	 * Make the discovered devices available.
	 */
	pci_bus_add_devices(bus);

	return max;
}
EXPORT_SYMBOL(pci_do_scan_bus);
