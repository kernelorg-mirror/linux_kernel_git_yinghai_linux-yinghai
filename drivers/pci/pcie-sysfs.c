#include <linux/kernel.h>
#include <linux/pci.h>

static struct attribute *pci_dev_pcie_dev_attrs[] = {
	NULL,
};

static umode_t pci_dev_pcie_attrs_are_visible(struct kobject *kobj,
						struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pci_is_pcie(pdev))
		return 0;

	return a->mode;
}

struct attribute_group pci_dev_pcie_attr_group = {
	.is_visible = pci_dev_pcie_attrs_are_visible,
	.attrs	    = pci_dev_pcie_dev_attrs,
};
