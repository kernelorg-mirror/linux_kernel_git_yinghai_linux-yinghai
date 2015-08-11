#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/delay.h>

int pcie_link_disable_get(struct pci_dev *dev)
{
	u16 lnk_ctrl;
	if (!pci_is_pcie(dev))
		return 0;

	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnk_ctrl);

	return !!(lnk_ctrl & PCI_EXP_LNKCTL_LD);
}

void pcie_link_disable_set(struct pci_dev *dev, int bit)
{
	u16 lnk_ctrl, old_lnk_ctrl;

	if (!pci_is_pcie(dev))
		return;

	pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &lnk_ctrl);
	old_lnk_ctrl = lnk_ctrl;

	if (!bit)
		lnk_ctrl &= ~PCI_EXP_LNKCTL_LD;
	else
		lnk_ctrl |= PCI_EXP_LNKCTL_LD;

	if (old_lnk_ctrl == lnk_ctrl)
		return;

	pcie_capability_write_word(dev, PCI_EXP_LNKCTL, lnk_ctrl);

	dev_printk(KERN_DEBUG, &dev->dev, "%s: lnk_ctrl = %x\n", __func__,
			 lnk_ctrl);
}
EXPORT_SYMBOL(pcie_link_disable_set);
