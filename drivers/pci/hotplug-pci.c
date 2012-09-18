/* Core PCI functionality used only by PCI hotplug */

#include <linux/pci.h>
#include <linux/export.h>
#include "pci.h"

int __ref pci_hp_add_bridge(struct pci_dev *dev)
{
	struct pci_bus *parent = dev->bus;
	int pass, busnr = parent->busn_res.start;

	for (pass = 0; pass < 2; pass++)
		busnr = pci_scan_bridge(parent, dev, busnr, pass);
	if (!dev->subordinate)
		return -1;

	return 0;
}
EXPORT_SYMBOL_GPL(pci_hp_add_bridge);
