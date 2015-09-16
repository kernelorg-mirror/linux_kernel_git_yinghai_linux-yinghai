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

#define LINK_RETRAIN_TIMEOUT HZ

int pcie_link_retrain(struct pci_dev *parent)
{
	u16 reg16;
	unsigned long start_jiffies;

	if (!pci_is_pcie(parent))
		return 0;

	pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &reg16);
	/* Retrain link */
	reg16 |= PCI_EXP_LNKCTL_RL;
	pcie_capability_write_word(parent, PCI_EXP_LNKCTL, reg16);
	dev_printk(KERN_DEBUG, &parent->dev, "%s: lnk_ctrl = %x\n", __func__,
			 reg16);

	/* Wait for link training end. Break out after waiting for timeout */
	start_jiffies = jiffies;
	for (;;) {
		pcie_capability_read_word(parent, PCI_EXP_LNKSTA, &reg16);
		if (!(reg16 & PCI_EXP_LNKSTA_LT))
			break;
		if (time_after(jiffies, start_jiffies + LINK_RETRAIN_TIMEOUT))
			break;
		msleep(1);
	}
	if (!(reg16 & PCI_EXP_LNKSTA_LT))
		return 0;

	return -1;
}
