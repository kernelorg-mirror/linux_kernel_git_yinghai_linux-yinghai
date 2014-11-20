#include <linux/kernel.h>
#include <linux/pci.h>
#include <asm/pci-direct.h>
#include <asm/io.h>
#include <asm/pci_x86.h>

/* Direct PCI access. This is used for PCI accesses in early boot before
   the PCI subsystem works. */

u32 read_pci_config(u8 bus, u8 slot, u8 func, u8 offset)
{
	u32 v;
	outl(0x80000000 | (bus<<16) | (slot<<11) | (func<<8) | offset, 0xcf8);
	v = inl(0xcfc);
	return v;
}

u8 read_pci_config_byte(u8 bus, u8 slot, u8 func, u8 offset)
{
	u8 v;
	outl(0x80000000 | (bus<<16) | (slot<<11) | (func<<8) | offset, 0xcf8);
	v = inb(0xcfc + (offset&3));
	return v;
}

u16 read_pci_config_16(u8 bus, u8 slot, u8 func, u8 offset)
{
	u16 v;
	outl(0x80000000 | (bus<<16) | (slot<<11) | (func<<8) | offset, 0xcf8);
	v = inw(0xcfc + (offset&2));
	return v;
}

void write_pci_config(u8 bus, u8 slot, u8 func, u8 offset,
				    u32 val)
{
	outl(0x80000000 | (bus<<16) | (slot<<11) | (func<<8) | offset, 0xcf8);
	outl(val, 0xcfc);
}

void write_pci_config_byte(u8 bus, u8 slot, u8 func, u8 offset, u8 val)
{
	outl(0x80000000 | (bus<<16) | (slot<<11) | (func<<8) | offset, 0xcf8);
	outb(val, 0xcfc + (offset&3));
}

void write_pci_config_16(u8 bus, u8 slot, u8 func, u8 offset, u16 val)
{
	outl(0x80000000 | (bus<<16) | (slot<<11) | (func<<8) | offset, 0xcf8);
	outw(val, 0xcfc + (offset&2));
}

int early_pci_allowed(void)
{
	return (pci_probe & (PCI_PROBE_CONF1|PCI_PROBE_NOEARLY)) ==
			PCI_PROBE_CONF1;
}

void early_dump_pci_device(u8 bus, u8 slot, u8 func)
{
	int i;
	int j;
	u32 val;

	printk(KERN_INFO "pci 0000:%02x:%02x.%d config space:",
	       bus, slot, func);

	for (i = 0; i < 256; i += 4) {
		if (!(i & 0x0f))
			printk("\n  %02x:",i);

		val = read_pci_config(bus, slot, func, i);
		for (j = 0; j < 4; j++) {
			printk(" %02x", val & 0xff);
			val >>= 8;
		}
	}
	printk("\n");
}

void early_dump_pci_devices(void)
{
	unsigned bus, slot, func;

	if (!early_pci_allowed())
		return;

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				u32 l;
				u8 type;

				l = read_pci_config(bus, slot, func,
							PCI_VENDOR_ID);
				/*
				 * some broken boards return 0 or ~0 if a slot
				 *  is empty
				 */
				if (l == 0xffffffff || l == 0x00000000 ||
				    l == 0x0000ffff || l == 0xffff0000)
					continue;

				early_dump_pci_device(bus, slot, func);

				if (func == 0) {
					type = read_pci_config_byte(bus, slot,
								    func,
							       PCI_HEADER_TYPE);
					if (!(type & 0x80))
						break;
				}
			}
		}
	}
}

static __init int
early_pci_read(struct pci_bus *bus, unsigned int devfn, int where,
			int size, u32 *value)
{
	int num, slot, func;

	num = bus->number;
	slot = devfn >> 3;
	func = devfn & 7;
	switch (size) {
	case 1:
		*value = read_pci_config_byte(num, slot, func, where);
		break;
	case 2:
		*value = read_pci_config_16(num, slot, func, where);
		break;
	case 4:
		*value = read_pci_config(num, slot, func, where);
		break;
	}

	return 0;
}

static __init int
early_pci_write(struct pci_bus *bus, unsigned int devfn, int where,
			int size, u32 value)
{
	int num, slot, func;

	num = bus->number;
	slot = devfn >> 3;
	func = devfn & 7;
	switch (size) {
	case 1:
		write_pci_config_byte(num, slot, func, where, (u8)value);
		break;
	case 2:
		write_pci_config_16(num, slot, func, where, (u16)value);
		break;
	case 4:
		write_pci_config(num, slot, func, where, (u32)value);
		break;
	}

	return 0;
}

static __initdata struct pci_ops pci_early_ops = {
	.read  = early_pci_read,
	.write = early_pci_write,
};
static __initdata struct pci_bus pci_early_bus = {
	.ops = &pci_early_ops,
};
static __initdata char pci_early_init_name[8];
static __initdata struct pci_dev pci_early_dev = {
	.bus = &pci_early_bus,
	.dev = {
		.init_name = pci_early_init_name,
	},
};

__init struct pci_dev *get_early_pci_dev(int num, int slot, int func)
{
	struct pci_dev *pdev;

	pdev = &pci_early_dev;
	pdev->devfn = (slot<<3) | (func & 7);
	pdev->bus->number = num;
	sprintf((char *)pdev->dev.init_name, "%02x:%02x.%01x", num, slot, func);

	return pdev;
}
