/*
 *	drivers/pci/pci_test.c
 *		by Yinghai Lu <yinghai@kernel.org>
 *
 * # insmod pci_test.ko data_file=pci_test_data.txt mask_file=pci_test_mask.txt
 * # lspci -tv
 * # cat /proc/ioports_test
 * # cat /proc/iomem_test
 * # rmmod pci_test
 *
 * data_file: is root bus resource from boot log + "lspci -vvxxxx"
 * mask_file: modified from "lspci -vvxxxx" with mask head.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

struct test_pci_root {
	struct list_head node;
	u16 segment;
	u8 busnumber;
	struct resource secondary;
	struct resource c_pri; /* holder for child primary */
	struct list_head root_res_list;
	struct resource secondary_os;
	struct pci_bus *bus;
};

struct test_pci_dev {
	struct list_head node;
	u32 mbdf;
	int space_size; /* 256 or 4096 */
	unsigned char *reg;

	int mask_size;
	unsigned char *mask;

	int invisiable;
	struct test_pci_dev *parent;  /* device on root bus has no parent */
	struct list_head dev_list; /* node in children devices */
	int bridge;
	struct resource secondary;
	struct resource c_pri; /* holder for child primary */
	struct list_head children;  /* bridge to point to children devices */
};

struct test_pci_mask {
	struct list_head node;
	u32 vendor_device_id;
	int space_size; /* 256 or 4096 */
	unsigned char *reg; /* for mask: bit set means rw */
};

static LIST_HEAD(test_pci_root_list);
static LIST_HEAD(test_pci_dev_list);
static LIST_HEAD(test_pci_mask_list);

static struct test_pci_root *test_pci_get_root(int mn, int bn)
{
	struct test_pci_root *root;

	list_for_each_entry(root, &test_pci_root_list, node)
		if (root->segment == (mn & 0xffff) &&
		    root->busnumber == (bn & 0xff))
			return root;

	return NULL;
}

static struct test_pci_root *test_pci_add_root(int mn, int bn, int bn1, int bn2)
{
	struct test_pci_root *root;

	if (bn > bn1 || bn1 > bn2)
		return NULL;

	root = test_pci_get_root(mn, bn);
	if (root) /* bus come last ? */
		goto set_secondary;

	root = kzalloc(sizeof(struct test_pci_root), GFP_KERNEL);
	if (!root)
		return NULL;

	INIT_LIST_HEAD(&root->root_res_list);
	list_add_tail(&root->node, &test_pci_root_list);

	root->segment = mn & 0xffff;
	root->busnumber = bn & 0xff;

set_secondary:
	if (root->c_pri.parent)
		release_resource(&root->c_pri);
	root->secondary.flags = IORESOURCE_BUS;
	root->secondary.start = bn1 & 0xff;
	root->secondary.end = bn2 & 0xff;
	root->c_pri.flags = IORESOURCE_BUS;
	root->c_pri.start = root->secondary.start;
	root->c_pri.end = root->secondary.start;
	request_resource(&root->secondary, &root->c_pri);

	/* for os */
	root->secondary_os.flags = IORESOURCE_BUS;
	root->secondary_os.start = bn1 & 0xff;
	root->secondary_os.end = bn2 & 0xff;

	return root;
}

static int test_pci_add_root_res(int mn, int bn,
			resource_size_t start, resource_size_t end,
			int flags, resource_size_t offset)
{
	struct resource_entry *entry;
	struct test_pci_root *root;

	root = test_pci_get_root(mn, bn);
	if (!root) {
		root = test_pci_add_root(mn, bn, bn, bn);
		if (!root)
			return -ENOMEM;
	}

	entry = resource_list_create_entry(NULL, 0);
	if (!entry)
		return -ENOMEM;

	entry->res->start = start;
	entry->res->end = end;
	entry->res->flags = flags;
	entry->offset = offset;
	resource_list_add_tail(entry, &root->root_res_list);

	return 0;
}

static void test_pci_free_all_root_res(struct list_head *head)
{
	struct resource_entry *entry, *tmp;

	resource_list_for_each_entry_safe(entry, tmp, head)
		resource_list_destroy_entry(entry);
}

#define MBDF(m, b, d, f) ((((m)&0xffff)<<16) | (((b)&0xff)<<8) | \
			  (((d)&0x1f)<<3) | ((f)&7))

static struct test_pci_dev *test_pci_add_dev(int mn, int bn,
			 int dn, int fn, int space_size, int bn1, int bn2)
{
	u32 mbdf = MBDF(mn, bn, dn, fn);
	struct test_pci_dev *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	list_add_tail(&dev->node, &test_pci_dev_list);

	dev->reg = kzalloc(space_size, GFP_KERNEL);
	if (!dev->reg)
		return NULL;

	dev->mbdf = mbdf;
	dev->space_size = space_size;

	if (bn1 != -1) {
		dev->secondary.flags = IORESOURCE_BUS;
		dev->secondary.start = bn1;
		dev->secondary.end = bn2;
		dev->c_pri.flags = IORESOURCE_BUS;
		dev->c_pri.start = bn1;
		dev->c_pri.end = bn1;
	}

	return dev;
}

static int test_pci_relocate_dev_reg(struct test_pci_dev *dev,
					 int space_size)
{
	unsigned char *reg;

	if (dev->space_size >= space_size)
		return 0;

	reg = kzalloc(space_size, GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	memcpy(reg, dev->reg, dev->space_size);
	kfree(dev->reg);
	dev->space_size = space_size;
	dev->reg = reg;

	return 0;
}

static void test_pci_free_dev(void)
{
	struct test_pci_dev *dev, *tmp;

	list_for_each_entry_safe(dev, tmp, &test_pci_dev_list, node) {
		list_del(&dev->node);
		kfree(dev->reg);
		kfree(dev);
	}
}

static struct test_pci_mask *test_pci_add_mask(int vendor_id,
				 int device_id, int space_size)
{
	u32 vendor_device_id = vendor_id | (device_id<<16);
	struct test_pci_mask *mask;

	mask = kzalloc(sizeof(*mask), GFP_KERNEL);
	if (!mask)
		return NULL;

	list_add_tail(&mask->node, &test_pci_mask_list);

	mask->reg = kzalloc(space_size, GFP_KERNEL);
	if (!mask->reg)
		return NULL;

	mask->vendor_device_id = vendor_device_id;
	mask->space_size = space_size;

	return mask;
}

static int test_pci_relocate_mask_reg(struct test_pci_mask *mask,
					 int space_size)
{
	unsigned char *reg;

	if (mask->space_size >= space_size)
		return 0;

	reg = kzalloc(space_size, GFP_KERNEL);
	if (!reg)
		return -ENOMEM;

	memcpy(reg, mask->reg, mask->space_size);
	kfree(mask->reg);
	mask->space_size = space_size;
	mask->reg = reg;

	return 0;
}

static void test_pci_free_mask(void)
{
	struct test_pci_mask *mask, *tmp;

	list_for_each_entry_safe(mask, tmp, &test_pci_mask_list, node) {
		list_del(&mask->node);
		kfree(mask->reg);
		kfree(mask);
	}
}

static void test_pci_free(void)
{
	struct test_pci_root *root, *tmp;

	list_for_each_entry_safe(root, tmp, &test_pci_root_list, node) {
		test_pci_free_all_root_res(&root->root_res_list);
		kfree(root);
	}

	test_pci_free_dev();
	test_pci_free_mask();
}

/* dump_validate and dump_init are adapted from pci_utils lib/dump.c */
static int dump_validate(char *s, char *fmt)
{
	while (*fmt) {
		if (*fmt == '#' ? !isxdigit(*s) : *fmt != *s)
			return 0;
		fmt++, s++;
	}

	return 1;
}

static int dump_init(const char *name, int mn_offset)
{
	struct file *f;
	char buf[256];
	struct test_pci_dev *dev = NULL;
	int len, mn, bn, dn, fn, i, j;
	int seg, bn1, bn2;
	unsigned long rstart, rend, bstart, bend, offset;
	int good;
	loff_t pos = 0;

	f = filp_open(name, O_RDONLY, 0);
	if (!f) {
		pr_err("pci_test: Cannot open %s", name);
		return -ENOENT;
	}

	while (kernel_read(f, pos, buf, sizeof(buf)-1) > 0) {
		char *z = strchr(buf, '\n');

		if (!z) {
			filp_close(f, 0);
			pr_err("pci_test: line too long or unterminated");
			return -EINVAL;
		}
		pos += z - buf + 1;

		*z-- = 0;
		if (z >= buf && *z == '\r')
			*z-- = 0;
		len = z - buf + 1;
		mn = 0;
		if (dump_validate(buf, "pci_bus ####:##: root bus resource [bus ##-##]") &&
		    sscanf(buf, "pci_bus %x:%x: root bus resource [bus %x-%x]",
			   &seg, &bn, &bn1, &bn2) == 4) {
			if (!test_pci_add_root(seg + mn_offset, bn, bn1, bn2))
				return -ENOMEM;
		} else if (dump_validate(buf, "pci_bus ####:##: root bus resource [bus ##]") &&
			   sscanf(buf, "pci_bus %x:%x: root bus resource [bus %x]",
				  &seg, &bn, &bn1) == 3) {
			if (!test_pci_add_root(seg + mn_offset, bn, bn1, bn1))
				return -ENOMEM;
		} else if (dump_validate(buf, "pci_bus ####:##: root bus resource [io")) {
			offset = 0;
			good = 0;
			if (strstr(buf, "bus address") &&
			    sscanf(buf, "pci_bus %x:%x: root bus resource [io  0x%lx-0x%lx] (bus address [0x%lx-0x%lx])",
				   &seg, &bn, &rstart, &rend, &bstart, &bend) == 6) {
				offset = rstart - bstart;
				good = 1;
			} else if (sscanf(buf, "pci_bus %x:%x: root bus resource [io  0x%lx-0x%lx]",
					  &seg, &bn, &rstart, &rend) == 4)
				good = 1;

			if (good && test_pci_add_root_res(seg + mn_offset,
							bn, rstart, rend,
							IORESOURCE_IO, offset))
				return -ENOMEM;
		} else if (dump_validate(buf, "pci_bus ####:##: root bus resource [mem")) {
			offset = 0;
			good = 0;
			if (strstr(buf, "bus address") &&
			    sscanf(buf, "pci_bus %x:%x: root bus resource [mem 0x%lx-0x%lx] (bus address [0x%lx-0x%lx])",
				   &seg, &bn, &rstart, &rend, &bstart, &bend) == 6) {
				offset = rstart - bstart;
				good = 1;
			} else if (sscanf(buf, "pci_bus %x:%x: root bus resource [mem 0x%lx-0x%lx]",
					  &seg, &bn, &rstart, &rend) == 4)
				good = 1;

			if (good && test_pci_add_root_res(seg + mn_offset,
							bn, rstart, rend,
							IORESOURCE_MEM, offset))
				return -ENOMEM;
		} else if ((dump_validate(buf, "##:##.# [##-##]") &&
			    sscanf(buf, "%x:%x.%d [%x-%x]", &bn, &dn, &fn, &bn1, &bn2) == 5) ||
			   (dump_validate(buf, "####:##:##.# [##-##]") &&
			    sscanf(buf, "%x:%x:%x.%d [%x-%x]", &mn, &bn, &dn, &fn, &bn1, &bn2) == 6)) {
			dev = test_pci_add_dev(mn + mn_offset, bn,
						 dn, fn, 256, bn1, bn2);
			if (!dev)
				return -ENOMEM;
		} else if ((dump_validate(buf, "##:##.# ") &&
			    sscanf(buf, "%x:%x.%d", &bn, &dn, &fn) == 3) ||
			   (dump_validate(buf, "####:##:##.# ") &&
			    sscanf(buf, "%x:%x:%x.%d", &mn, &bn, &dn, &fn) == 4)) {
			dev = test_pci_add_dev(mn + mn_offset, bn,
							  dn, fn, 256, -1, -1);
			if (!dev)
				return -ENOMEM;
		} else if (!len) {
			dev = NULL;
		} else if (dev &&
			   (dump_validate(buf, "##: ") ||
			    dump_validate(buf, "###: ")) &&
			   sscanf(buf, "%x: ", &i) == 1) {
			z = strchr(buf, ' ') + 1;
			while (isxdigit(z[0]) && isxdigit(z[1]) &&
			       (!z[2] || z[2] == ' ') &&
			       sscanf(z, "%x", &j) == 1 && j < 256) {
				if (i >= 4096) {
					filp_close(f, 0);
					pr_err("pci_test: At most 4096 bytes of config space are supported");
				}
				if (i >= dev->space_size)
					test_pci_relocate_dev_reg(dev, 4096);
				dev->reg[i++] = j;
				z += 2;
				if (*z)
					z++;
			}

			if (*z) {
				filp_close(f, 0);
				pr_err("pci_test: Malformed line");
				return -EINVAL;
			}
		}
	}

	filp_close(f, 0);

	return 0;
}

static int mask_dump_init(const char *name)
{
	struct file *f;
	char buf[256];
	struct test_pci_mask *mask = NULL;
	int len, vendor_id, device_id, i, j;
	loff_t pos = 0;

	f = filp_open(name, O_RDONLY, 0);
	if (!f) {
		pr_err("pci_test: Cannot open %s", name);
		return -ENOENT;
	}

	while (kernel_read(f, pos, buf, sizeof(buf)-1) > 0) {
		char *z = strchr(buf, '\n');

		if (!z) {
			filp_close(f, 0);
			pr_err("pci_test: line too long or unterminated");
			return -EINVAL;
		}
		pos += z - buf + 1;

		*z-- = 0;
		if (z >= buf && *z == '\r')
			*z-- = 0;
		len = z - buf + 1;
		if (dump_validate(buf, "mask: ####:####") &&
		    sscanf(buf, "mask: %x:%x", &vendor_id, &device_id) == 2) {
			mask = test_pci_add_mask(vendor_id,
							  device_id, 256);
			if (!mask)
				return -ENOMEM;
		} else if (!len) {
			mask = NULL;
		} else if (mask &&
			   (dump_validate(buf, "##: ") ||
			    dump_validate(buf, "###: ")) &&
			   sscanf(buf, "%x: ", &i) == 1) {
			z = strchr(buf, ' ') + 1;
			while (isxdigit(z[0]) && isxdigit(z[1]) &&
			       (!z[2] || z[2] == ' ') &&
			       sscanf(z, "%x", &j) == 1 && j < 256) {
				if (i >= 4096) {
					filp_close(f, 0);
					pr_err("pci_test: At most 4096 bytes of config space are supported");
				}
				if (i >= mask->space_size)
					test_pci_relocate_mask_reg(mask, 4096);
				mask->reg[i++] = j;
				z += 2;
				if (*z)
					z++;
			}

			if (*z) {
				filp_close(f, 0);
				pr_err("pci_test: Malformed line");
				return -EINVAL;
			}
		}
	}

	filp_close(f, 0);

	return 0;
}

#define MASK_WARN_NUM 512
static u32 mask_warn[MASK_WARN_NUM];
static void warn_mask_once(int vendor_id, int device_id)
{
	int i;
	u32 mask = vendor_id | (device_id << 16);

	for (i = 0; i < MASK_WARN_NUM; i++) {
		if (mask_warn[i] == mask)
			return;

		if (!mask_warn[i]) {
			mask_warn[i] = mask;
			break;
		}
	}

	pr_warn("pci_test: no mask for %04x:%04x\n", vendor_id, device_id);
}

static struct test_pci_mask *test_pci_get_mask(int vendor_id, int device_id)
{
	int vendor_device_id = vendor_id | (device_id << 16);
	struct test_pci_mask *mask;

	list_for_each_entry(mask, &test_pci_mask_list, node)
		if (vendor_device_id == mask->vendor_device_id)
			return mask;

	return NULL;
}

static unsigned char bridge_mask[0x40] = {
/*00:*/ 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x10, 0x00,
	0x04, 0x00, 0x04, 0x06, 0x10, 0x00, 0x81, 0x00,
/*10:*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xff, 0xff, 0x00, 0xf0, 0xf0, 0x00, 0x20,
/*20:*/ 0xf0, 0xff, 0xf0, 0xff, 0xf0, 0xff, 0xf0, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
/*30:*/ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0b, 0x01, 0x13, 0x00,
};

static void test_pci_probe_mask(void)
{
	struct test_pci_dev *dev;

	list_for_each_entry(dev, &test_pci_dev_list, node) {
		unsigned char *reg = dev->reg;
		u16 vendor_id = reg[0] | (reg[1] << 8);
		u16 device_id = reg[2] | (reg[3] << 8);
		struct test_pci_mask *mask;

		mask = test_pci_get_mask(vendor_id, device_id);
		if (!mask) {
			u16 class = (reg[0xb] <<  8) | reg[0xa];

			warn_mask_once(vendor_id, device_id);
			if (class == PCI_CLASS_BRIDGE_PCI) {
				pr_warn("pci_test: use default bridge mask for %04x:%04x\n",
					vendor_id, device_id);
				dev->mask = bridge_mask;
				dev->mask_size = ARRAY_SIZE(bridge_mask);
			}
		} else {
			dev->mask = mask->reg;
			dev->mask_size = mask->space_size;
		}
	}
}

static void test_pci_update_secondary_from_reg(struct test_pci_dev *parent)
{
	unsigned char *reg = parent->reg;

	parent->secondary.flags = IORESOURCE_BUS;
	parent->secondary.start = reg[0x19];
	parent->secondary.end = reg[0x1a];
	parent->c_pri.flags = IORESOURCE_BUS;
	parent->c_pri.start = parent->c_pri.end = reg[0x19];
}

static void test_pci_link_devices(void)
{
	struct test_pci_dev *dev, *parent;

	/* set mask and find out all bridges */
	list_for_each_entry(dev, &test_pci_dev_list, node) {
		unsigned char *reg = dev->reg;
		u16 class = (reg[0xb] <<  8) | reg[0xa];

		if (class != PCI_CLASS_BRIDGE_PCI)
			continue;

		INIT_LIST_HEAD(&dev->children);
		dev->bridge = 1;

		/* if bus range is not passed from ##:##.0 [##-##] */
		if (!dev->secondary.flags)
			test_pci_update_secondary_from_reg(dev);
	}

	/* link children to parent */
	list_for_each_entry(dev, &test_pci_dev_list, node) {
		int mn = (dev->mbdf >> 16) & 0xffff;
		int bn = (dev->mbdf >> 8) & 0xff;

		/* check if it is on root bus */
		if (test_pci_get_root(mn, bn))
			continue;

		/* find parent for it */
		list_for_each_entry(parent, &test_pci_dev_list, node) {
			int parent_mn;

			if (!parent->bridge)
				continue;

			parent_mn = (parent->mbdf >> 16) & 0xffff;
			if (parent_mn != mn)
				continue;

			if (bn != parent->secondary.start)
				continue;

			/* it is */
			list_add_tail(&dev->dev_list, &parent->children);
			dev->parent = parent;
			break;
		}

		if (!dev->parent) {
			int dn = (dev->mbdf >> 3) & 0x1f;
			int fn = dev->mbdf & 0x07;

			pr_warn("pci_test: no parent for %04x:%02x:%02x.%1x\n",
					mn, bn, dn, fn);
		}
	}
}

static int pci_test_request_bus(struct resource *parent_res,
				struct resource *child_res)
{
	int ret;

	ret = request_resource(parent_res, child_res);
	if (!ret)
		return 0;

	/* child_res could have 0xff for probing, so shrink it to parent's */
	if (child_res->end != 0xff || child_res->start > parent_res->end)
		return ret;

	child_res->end = parent_res->end;
	ret = request_resource(parent_res, child_res);
	if (!ret)
		return 0;

	child_res->end = 0xff;

	return ret;
}

static void test_pci_update_invisiable_bridge(struct test_pci_dev *parent)
{
	struct test_pci_dev *child;
	int invisiable = 1;

	if (parent->c_pri.parent)
		invisiable = 0;

	list_for_each_entry(child, &parent->children, dev_list) {
		child->invisiable = invisiable;

		if (!child->bridge)
			continue;

		if (!invisiable &&
		    !pci_test_request_bus(&parent->secondary,
					  &child->secondary))
			request_resource(&child->secondary, &child->c_pri);

		test_pci_update_invisiable_bridge(child);
	}
}

static void test_pci_update_invisiable(void)
{
	struct test_pci_dev *parent;

	/* read bus info from reg */
	list_for_each_entry(parent, &test_pci_dev_list, node) {
		if (!parent->bridge)
			continue;

		test_pci_update_secondary_from_reg(parent);
	}

	/* if can not allocate bridge secondary properly, then invisiable */
	list_for_each_entry(parent, &test_pci_dev_list, node) {
		int mn, bn;
		struct test_pci_root *root;

		if (!parent->bridge)
			continue;

		mn = (parent->mbdf >> 16) & 0xffff;
		bn = (parent->mbdf >> 8) & 0xff;
		root = test_pci_get_root(mn, bn);
		if (!root)
			continue;

		if (!pci_test_request_bus(&root->secondary, &parent->secondary))
			request_resource(&parent->secondary, &parent->c_pri);

		test_pci_update_invisiable_bridge(parent);
	}
}

static void test_pci_update_bus(struct test_pci_dev *parent, int b_19_orig,
				int b_1a_orig, int b_19, int b_1a)
{
	struct test_pci_dev *child;
	struct resource *parent_res;

	if (!parent->bridge)
		return;

	if ((b_19_orig == b_19) && (b_1a_orig == b_1a))
		return;

	{
		int mn = (parent->mbdf >> 16) & 0xffff;
		int bn = (parent->mbdf >> 8) & 0xff;
		int dn = (parent->mbdf >> 3) & 0x1f;
		int fn = parent->mbdf & 0x07;

		pr_warn("pci_test:  %04x:%02x:%02x.%1x update bus [%02x-%02x] ==> [%02x-%02x]\n",
			mn, bn, dn, fn, b_19_orig, b_1a_orig, b_19, b_1a);
	}

	parent_res = parent->secondary.parent;
	if (parent_res) {
		release_child_resources(&parent->secondary);
		release_resource(&parent->secondary);
	} else {
		/* bridge on root bus */
		struct test_pci_root *root;
		int mn = (parent->mbdf >> 16) & 0xffff;
		int bn = (parent->mbdf >> 8) & 0xff;

		root = test_pci_get_root(mn, bn);
		if (!root)
			return;

		parent_res = &root->secondary;
	}

	list_for_each_entry(child, &parent->children, dev_list) {
		child->mbdf &= 0xffff00ff;
		child->mbdf |= b_19 << 8;

		if (child->bridge)
			child->reg[0x18] = b_19;
	}

	test_pci_update_secondary_from_reg(parent);

	if (!pci_test_request_bus(parent_res, &parent->secondary))
		request_resource(&parent->secondary, &parent->c_pri);

	test_pci_update_invisiable_bridge(parent);
}

static int test_pci_init(const char *fn, int domain_offset, const char *mask_fn)
{
	if (dump_init(fn, domain_offset)) {
		test_pci_free();
		return -EINVAL;
	}

	mask_dump_init(mask_fn);

	test_pci_probe_mask();

	test_pci_link_devices();

	test_pci_update_invisiable();

	return 0;
}

/*--------------------------------------------*/

static struct test_pci_dev *test_pci_get_dev(int mn, int bn,
						  int dn, int fn)
{
	int mbdf = MBDF(mn, bn, dn, fn);
	struct test_pci_dev *dev;

	list_for_each_entry(dev, &test_pci_dev_list, node)
		if (mbdf == dev->mbdf && !dev->invisiable)
			return dev;

	return NULL;
}

static int test_pci_read(struct test_pci_dev *dev,
			 int where, int size, u32 *val)
{
	int i;
	unsigned char *buf = (unsigned char *)val;
	unsigned char *reg = dev->reg;

	for (i = 0; i < size; i++)
		buf[i] = reg[where + i];

	return 0;
}

static int test_pci_write(struct test_pci_dev *dev,
			  int where, int size, u32 val)
{
	int i;
	unsigned char *buf = (unsigned char *)&val;
	unsigned char *reg = dev->reg;
	unsigned char mask;

	/* bit set means: rw */
	for (i = 0; i < size; i++) {
		if (dev->mask && (where + i < dev->mask_size))
			mask = dev->mask[where + i];
		else {
			/* just guess */
			if (!reg[where + i])
				mask = 0x00;
			else
				mask = 0xff;
		}
		reg[where + i] &= ~mask;
		reg[where + i] |= buf[i] & mask;
	}

	return 0;
}

/*--------------------------------------------*/

static int pci_test_read(struct pci_bus *bus, unsigned int devfn,
			 int where, int size, u32 *val)
{
	struct test_pci_dev *dev;

	dev = test_pci_get_dev(pci_domain_nr(bus),
				 bus->number, devfn >> 3, devfn & 7);
	if (!dev) {
		*val = 0xffffffff;  /* or 0 ? */

		return -EINVAL;
	}

	return test_pci_read(dev, where, size, val);
}

static int pci_test_write(struct pci_bus *bus, unsigned int devfn,
			  int where, int size, u32 val)
{
	struct test_pci_dev *dev;
	unsigned char b_19_orig, b_1a_orig, *reg;
	int ret;

	dev = test_pci_get_dev(pci_domain_nr(bus),
				      bus->number, devfn >> 3, devfn & 7);
	if (!dev)
		return -EINVAL;

	reg = dev->reg;
	b_19_orig = reg[0x19];
	b_1a_orig = reg[0x1a];
	ret = test_pci_write(dev, where, size, val);
	test_pci_update_bus(dev, b_19_orig, b_1a_orig, reg[0x19], reg[0x1a]);

	return ret;
}

static struct pci_ops pci_test_ops = {
	.read = pci_test_read,
	.write = pci_test_write,
};

static int pci_test_get_root_resources(struct test_pci_root *root,
				  struct list_head *list)
{
	struct resource_entry *src, *dst;

	/* set the resource list */
	resource_list_for_each_entry(src, &root->root_res_list) {
		dst = resource_list_create_entry(NULL, 0);
		if (!dst)
			return -ENOMEM;

		dst->res->start = src->res->start;
		dst->res->end = src->res->end;
		dst->res->flags = src->res->flags;
		dst->offset = src->offset;
		resource_list_add_tail(dst, list);
	}

	return 0;
}

/*--------------------------------------------*/
static struct resource pci_test_ioport_resource = {
	.name   = "PCI IO",
	.start  = 0,
	.end    = IO_SPACE_LIMIT,
	.flags  = IORESOURCE_IO,
};

static struct resource pci_test_iomem_resource = {
	.name   = "PCI mem",
	.start  = 0,
	.end    = -1,
	.flags  = IORESOURCE_MEM,
};

struct pci_test_root_info;

struct pci_test_root_ops {
	struct pci_ops *pci_ops;
	void (*release_info)(struct pci_test_root_info *info);
	int (*prepare_resources)(struct pci_test_root_info *info);
};

struct pci_test_root_info {
	struct test_pci_root *root;
	struct pci_test_root_ops *ops;
	struct list_head resources;
	char name[16];
};

struct pci_root_info_ext {
	struct pci_test_root_info common;
	struct pci_sysdata sd;
};

static int pci_test_root_prepare_resources(struct pci_test_root_info *ci)
{
	int ret;
	struct list_head *list = &ci->resources;
	struct resource_entry *entry;

	ret = pci_test_get_root_resources(ci->root, list);
	resource_list_for_each_entry(entry, list)
		entry->res->name = ci->name;

	return ret;
}

static void pci_test_root_info_release(struct pci_test_root_info *ci)
{
	kfree(container_of(ci, struct pci_root_info_ext, common));
}

static struct pci_test_root_ops pci_test_root_ops = {
	.pci_ops = &pci_test_ops,
	.release_info = pci_test_root_info_release,
	.prepare_resources = pci_test_root_prepare_resources,
};

static void __pci_test_root_release_info(struct pci_test_root_info *info)
{
	struct resource *res;
	struct resource_entry *entry, *tmp;

	if (!info)
		return;

	resource_list_for_each_entry_safe(entry, tmp, &info->resources) {
		res = entry->res;
		if (res->parent &&
		    (res->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
			release_resource(res);
		resource_list_destroy_entry(entry);
	}

	info->ops->release_info(info);
}

static void pci_test_root_release_info(struct pci_host_bridge *bridge)
{
	struct resource *res;
	struct resource_entry *entry;

	resource_list_for_each_entry(entry, &bridge->windows) {
		res = entry->res;
		if (res->parent &&
		    (res->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
			release_resource(res);
	}
	__pci_test_root_release_info(bridge->release_data);
}

static void pci_test_root_add_resources(struct pci_test_root_info *info)
{
	struct resource_entry *entry, *tmp;
	struct resource *res, *root = NULL;

	resource_list_for_each_entry_safe(entry, tmp, &info->resources) {
		res = entry->res;
		if (res->flags & IORESOURCE_MEM)
			root = &pci_test_iomem_resource;
		else if (res->flags & IORESOURCE_IO)
			root = &pci_test_ioport_resource;
		else
			continue;

		if (res == root)
			continue;

		if (insert_resource(root, res)) {
			pr_info("ignoring host bridge window %pR\n", res);
			resource_list_destroy_entry(entry);
		}
	}
}

static struct pci_bus *pci_test_root_create(struct test_pci_root *root,
				     struct pci_test_root_ops *ops,
				     struct pci_test_root_info *info,
				     void *sysdata)
{
	int ret = -1, busnum = root->secondary.start;
	struct pci_bus *bus;

	info->root = root;
	info->ops = ops;
	INIT_LIST_HEAD(&info->resources);
	snprintf(info->name, sizeof(info->name), "PCI Bus %04x:%02x",
		 root->segment, busnum);

	if (ops->prepare_resources)
		ret = ops->prepare_resources(info);
	if (ret < 0)
		goto out_release_info;

	pci_test_root_add_resources(info);
	pci_add_resource(&info->resources, &root->secondary_os);
	bus = pci_create_root_bus(NULL, busnum, ops->pci_ops,
				  sysdata, &info->resources);
	if (!bus)
		goto out_release_info;

	bus->ioport_res = &pci_test_ioport_resource;
	bus->iomem_res = &pci_test_iomem_resource;

	pci_scan_child_bus(bus);
	pci_set_host_bridge_release(to_pci_host_bridge(bus->bridge),
					pci_test_root_release_info, info);

	return bus;

out_release_info:
	__pci_test_root_release_info(info);

	return NULL;
}

static struct pci_bus *pci_test_scan_root(struct test_pci_root *root)
{
	struct pci_root_info_ext *info_ext;
	struct pci_bus *bus;

	info_ext = kzalloc(sizeof(*info_ext), GFP_KERNEL);
	if (!info_ext)
		return NULL;

	info_ext->sd.domain = root->segment;
	bus = pci_test_root_create(root, &pci_test_root_ops, &info_ext->common,
					&info_ext->sd);

	return bus;
}

static void pci_bus_disable_match_devices(struct pci_bus *bus)
{
	struct pci_dev *dev;
	struct pci_bus *child;

	list_for_each_entry(dev, &bus->devices, bus_list)
		dev->match_driver = -1;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		child =  dev->subordinate;
		if (child)
			pci_bus_disable_match_devices(child);
	}
}

static char *data_file;
static int domain_nr_offset = 0xf000;
static char *mask_file;
static bool survey_first = true;
module_param(data_file, charp, 0444);
module_param(domain_nr_offset, int, 0444);
module_param(mask_file, charp, 0444);
module_param(survey_first, bool, 0444);
MODULE_PARM_DESC(data_file, "data file for pci root bus res and register info");
MODULE_PARM_DESC(domain_nr_offset, "offset for change domain_nr in data file");
MODULE_PARM_DESC(mask_file, "mask file for register mask info");
MODULE_PARM_DESC(survey_first, "survey BARs in register (from firmware) and reserve them at first");

#ifdef CONFIG_PROC_FS
static struct resource *next_resource(struct resource *p, bool sibling_only)
{
	/* Caller wants to traverse through siblings only */
	if (sibling_only)
		return p->sibling;

	if (p->child)
		return p->child;
	while (!p->sibling && p->parent)
		p = p->parent;
	return p->sibling;
}

static void *r_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct resource *p = v;
	(*pos)++;
	return (void *)next_resource(p, false);
}

enum { MAX_IORES_LEVEL = 5 };

static void *r_start(struct seq_file *m, loff_t *pos)
{
	struct resource *p = m->private;
	loff_t l = 0;

	for (p = p->child; p && l < *pos; p = r_next(m, p, &l))
		;
	return p;
}

static void r_stop(struct seq_file *m, void *v)
{
}

static int r_show(struct seq_file *m, void *v)
{
	struct resource *root = m->private;
	struct resource *r = v, *p;
	int width = root->end < 0x10000 ? 4 : 8;
	int depth;

	for (depth = 0, p = r; depth < MAX_IORES_LEVEL; depth++, p = p->parent)
		if (p->parent == root)
			break;
	seq_printf(m, "%*s%0*llx-%0*llx : %s\n",
			depth * 2, "",
			width, (unsigned long long) r->start,
			width, (unsigned long long) r->end,
			r->name ? r->name : "<BAD>");
	return 0;
}

static const struct seq_operations resource_op = {
	.start	= r_start,
	.next	= r_next,
	.stop	= r_stop,
	.show	= r_show,
};

static int ioports_open(struct inode *inode, struct file *file)
{
	int res = seq_open(file, &resource_op);

	if (!res) {
		struct seq_file *m = file->private_data;

		m->private = &pci_test_ioport_resource;
	}
	return res;
}

static int iomem_open(struct inode *inode, struct file *file)
{
	int res = seq_open(file, &resource_op);

	if (!res) {
		struct seq_file *m = file->private_data;

		m->private = &pci_test_iomem_resource;
	}
	return res;
}

static const struct file_operations proc_ioports_operations = {
	.open		= ioports_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static const struct file_operations proc_iomem_operations = {
	.open		= iomem_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

struct proc_dir_entry *ioports_proc;
struct proc_dir_entry *iomem_proc;

static void ioresources_init(void)
{
	ioports_proc = proc_create("ioports_test", 0, NULL,
				  &proc_ioports_operations);
	iomem_proc = proc_create("iomem_test", 0, NULL,
				 &proc_iomem_operations);
}

static void ioresources_exit(void)
{
	proc_remove(ioports_proc);
	proc_remove(iomem_proc);
}
#else
static void ioresources_init(void)
{
}
static void ioresources_exit(void)
{
}
#endif /* CONFIG_PROC_FS */

static __init int pci_test_init(void)
{
	struct test_pci_root *root;

	if (!data_file)
		return 0;

	pr_info("data file: %s domain_nr_offset: %04x\n",
		data_file, domain_nr_offset);

	if (test_pci_init(data_file, domain_nr_offset, mask_file))
		return 0;

	list_for_each_entry(root, &test_pci_root_list, node) {
		root->bus = pci_test_scan_root(root);
		if (!root->bus)
			continue;

		if (survey_first)
			pcibios_resource_survey_bus(root->bus);

		pci_assign_unassigned_root_bus_resources(root->bus);
		pci_bus_disable_match_devices(root->bus);
		pci_bus_add_devices(root->bus);
	}

	ioresources_init();

	return 0;
}

static void __exit pci_test_exit(void)
{
	struct test_pci_root *root;

	list_for_each_entry(root, &test_pci_root_list, node)
		if (root->bus) {
			pci_stop_root_bus(root->bus);
			pci_remove_root_bus(root->bus);
		}

	ioresources_exit();

	test_pci_free();
}

module_init(pci_test_init);
module_exit(pci_test_exit);

MODULE_AUTHOR("Yinghai Lu <yinghai@kernel.org>");
MODULE_DESCRIPTION("PCI test for resource allocation code");
MODULE_LICENSE("GPL v2");
