#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>
#include "pci.h"

int pci_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct pci_dev *pdev;

	if (!dev)
		return -ENODEV;

	pdev = to_pci_dev(dev);
	if (!pdev)
		return -ENODEV;

	if (add_uevent_var(env, "PCI_CLASS=%04X", pdev->class))
		return -ENOMEM;

	if (add_uevent_var(env, "PCI_ID=%04X:%04X", pdev->vendor, pdev->device))
		return -ENOMEM;

	if (add_uevent_var(env, "PCI_SUBSYS_ID=%04X:%04X", pdev->subsystem_vendor,
			   pdev->subsystem_device))
		return -ENOMEM;

	if (add_uevent_var(env, "PCI_SLOT_NAME=%s", pci_name(pdev)))
		return -ENOMEM;

	if (add_uevent_var(env, "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02x",
			   pdev->vendor, pdev->device,
			   pdev->subsystem_vendor, pdev->subsystem_device,
			   (u8)(pdev->class >> 16), (u8)(pdev->class >> 8),
			   (u8)(pdev->class)))
		return -ENOMEM;
	return 0;
}

static int pci_hp_notifier(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	struct device *dev = data;

	switch (event) {
	case BUS_NOTIFY_ADD_DEVICE:
		to_pci_dev(dev)->drivers_autoprobe = false;
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block pci_hp_nb = {
	.notifier_call = &pci_hp_notifier,
};

static int __init pci_hp_init(void)
{
	return bus_register_notifier(&pci_bus_type, &pci_hp_nb);
}

fs_initcall(pci_hp_init);
