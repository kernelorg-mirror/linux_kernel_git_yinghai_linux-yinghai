/*
 * Separated from drivers/pci/hotplug/acpiphp_glue.c
 *	only support root bridge
 */

#include <linux/init.h>
#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/acpi.h>

static const struct acpi_device_id root_device_ids[] = {
	{"PNP0A03", 0},
	{"", 0},
};

/* bridge flags */
#define ROOT_BRIDGE_HAS_EJ0	(0x00000002)
#define ROOT_BRIDGE_HAS_PS3	(0x00000080)

#define ACPI_STA_FUNCTIONING	(0x00000008)

static void handle_root_bridge_insertion(acpi_handle handle)
{
	struct acpi_device *device, *pdevice;
	acpi_handle phandle;
	int ret_val;

	acpi_get_parent(handle, &phandle);
	if (acpi_bus_get_device(phandle, &pdevice)) {
		printk(KERN_DEBUG "no parent device, assuming NULL\n");
		pdevice = NULL;
	}
	if (!acpi_bus_get_device(handle, &device)) {
		/* check if  pci root_bus is removed */
		struct acpi_pci_root *root = acpi_driver_data(device);
		if (pci_find_bus(root->segment, root->secondary.start))
			return;

		printk(KERN_DEBUG "bus exists... trim\n");
		/* this shouldn't be in here, so remove
		 * the bus then re-add it...
		 */
		ret_val = acpi_bus_trim(device, 1);
		printk(KERN_DEBUG "acpi_bus_trim return %x\n", ret_val);
	}
	if (acpi_bus_add(&device, pdevice, handle, ACPI_BUS_TYPE_DEVICE)) {
		printk(KERN_ERR "cannot add bridge to acpi list\n");
		return;
	}
	if (acpi_bus_start(device))
		printk(KERN_ERR "cannot start bridge\n");
}

static void handle_root_bridge_removal(struct acpi_device *device)
{
	int ret_val;
	struct acpi_eject_event *ej_event;

	ej_event = kmalloc(sizeof(*ej_event), GFP_KERNEL);
	if (!ej_event)
		return;

	ej_event->device = device;
	ej_event->event = ACPI_NOTIFY_EJECT_REQUEST;

	/* remove pci devices at first */
	ret_val = acpi_bus_trim(device, 0);
	printk(KERN_DEBUG "acpi_bus_trim stop return %x\n", ret_val);

	acpi_bus_hot_remove_device(ej_event);
}

static void _handle_hotplug_event_root(struct work_struct *work)
{
	struct acpi_pci_root *root;
	char objname[64];
	struct acpi_buffer buffer = { .length = sizeof(objname),
				      .pointer = objname };
	struct acpi_hp_work *hp_work;
	acpi_handle handle;
	u32 type;

	hp_work = container_of(work, struct acpi_hp_work, work);
	handle = hp_work->handle;
	type = hp_work->type;

	root = acpi_pci_find_root(handle);

	acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	switch (type) {
	case ACPI_NOTIFY_BUS_CHECK:
		/* bus enumerate */
		printk(KERN_DEBUG "%s: Bus check notify on %s\n", __func__,
				 objname);
		if (!root)
			handle_root_bridge_insertion(handle);

		break;

	case ACPI_NOTIFY_DEVICE_CHECK:
		/* device check */
		printk(KERN_DEBUG "%s: Device check notify on %s\n", __func__,
				 objname);
		if (!root)
			handle_root_bridge_insertion(handle);
		break;

	case ACPI_NOTIFY_EJECT_REQUEST:
		/* request device eject */
		printk(KERN_DEBUG "%s: Device eject notify on %s\n", __func__,
				 objname);
		if (root)
			handle_root_bridge_removal(root->device);
		break;
	default:
		printk(KERN_WARNING "notify_handler: unknown event type 0x%x for %s\n",
				 type, objname);
		break;
	}

	kfree(hp_work); /* allocated in handle_hotplug_event_bridge */
}

static void handle_hotplug_event_root(acpi_handle handle, u32 type,
					void *context)
{
	alloc_acpi_hp_work(handle, type, context,
				_handle_hotplug_event_root);
}

static bool acpi_is_root_bridge_object(acpi_handle handle)
{
	struct acpi_device_info *info = NULL;
	acpi_status status;
	bool ret;

	status = acpi_get_object_info(handle, &info);
	if (ACPI_FAILURE(status))
		return false;

	ret = !acpi_match_object_info_ids(info, root_device_ids);

	kfree(info);

	return ret;
}

static acpi_status __init
find_root_bridges(acpi_handle handle, u32 lvl, void *context, void **rv)
{
	acpi_status status;
	char objname[64];
	struct acpi_buffer buffer = { .length = sizeof(objname),
				      .pointer = objname };
	int *count = (int *)context;

	if (!acpi_is_root_bridge_object(handle))
		return AE_OK;

	(*count)++;

	acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	status = acpi_install_notify_handler(handle, ACPI_SYSTEM_NOTIFY,
					handle_hotplug_event_root, NULL);
	if (ACPI_FAILURE(status))
		printk(KERN_DEBUG "acpi root: %s notify handler is not installed, exit status: %u\n",
				  objname, (unsigned int)status);
	else
		printk(KERN_DEBUG "acpi root: %s notify handler is installed\n",
				 objname);

	return AE_OK;
}

static int __init acpi_pci_root_hp_init(void)
{
	int num = 0;

	acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
		ACPI_UINT32_MAX, find_root_bridges, NULL, &num, NULL);

	printk(KERN_DEBUG "Found %d acpi root devices\n", num);

	return 0;
}

subsys_initcall(acpi_pci_root_hp_init);
