#include <linux/kernel.h>
#include <linux/pci.h>

static ssize_t
pcie_link_disable_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return sprintf(buf, "%u\n", pcie_link_disable_get(pdev));
}
static ssize_t
pcie_link_disable_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	unsigned long val;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	pcie_link_disable_set(pdev, val);

	return count;
}

struct device_attribute pcie_link_disable_attr =
		__ATTR(pcie_link_disable, 0644,
		       pcie_link_disable_show, pcie_link_disable_store);

static struct attribute *pci_dev_pcie_dev_attrs[] = {
	&pcie_link_disable_attr.attr,
	NULL,
};

static umode_t pci_dev_pcie_attrs_are_visible(struct kobject *kobj,
						struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (!pci_is_pcie(pdev))
		return 0;

	if (a == &pcie_link_disable_attr.attr)
		if ((pdev->pcie_type != PCI_EXP_TYPE_ROOT_PORT) &&
		    (pdev->pcie_type != PCI_EXP_TYPE_DOWNSTREAM))
			return 0;

	return a->mode;
}

struct attribute_group pci_dev_pcie_attr_group = {
	.is_visible = pci_dev_pcie_attrs_are_visible,
	.attrs	    = pci_dev_pcie_dev_attrs,
};
