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

static LIST_HEAD(acpi_root_bridge_list);
struct acpi_root_bridge {
	struct list_head list;
	acpi_handle handle;
	u32 flags;
};

static const struct acpi_device_id root_device_ids[] = {
	{"PNP0A03", 0},
	{"", 0},
};

/* bridge flags */
#define ROOT_BRIDGE_HAS_EJ0	(0x00000002)
#define ROOT_BRIDGE_HAS_PS3	(0x00000080)

#define ACPI_STA_FUNCTIONING	(0x00000008)

static struct acpi_root_bridge *acpi_root_handle_to_bridge(acpi_handle handle)
{
	struct acpi_root_bridge *bridge;

	list_for_each_entry(bridge, &acpi_root_bridge_list, list)
		if (bridge->handle == handle)
			return bridge;

	return NULL;
}

/* allocate and initialize host bridge data structure */
static void add_acpi_root_bridge(acpi_handle handle)
{
	struct acpi_root_bridge *bridge;
	acpi_handle dummy_handle;
	acpi_status status;

	/* if the bridge doesn't have _STA, we assume it is always there */
	status = acpi_get_handle(handle, "_STA", &dummy_handle);
	if (ACPI_SUCCESS(status)) {
		unsigned long long tmp;

		status = acpi_evaluate_integer(handle, "_STA", NULL, &tmp);
		if (ACPI_FAILURE(status)) {
			printk(KERN_DEBUG "%s: _STA evaluation failure\n",
						 __func__);
			return;
		}
		if ((tmp & ACPI_STA_FUNCTIONING) == 0)
			/* don't register this object */
			return;
	}

	bridge = kzalloc(sizeof(struct acpi_root_bridge), GFP_KERNEL);
	if (!bridge)
		return;

	bridge->handle = handle;

	if (ACPI_SUCCESS(acpi_get_handle(handle, "_EJ0", &dummy_handle)))
		bridge->flags |= ROOT_BRIDGE_HAS_EJ0;
	if (ACPI_SUCCESS(acpi_get_handle(handle, "_PS3", &dummy_handle)))
		bridge->flags |= ROOT_BRIDGE_HAS_PS3;

	list_add(&bridge->list, &acpi_root_bridge_list);
}

static void remove_acpi_root_bridge(struct acpi_root_bridge *bridge)
{
	list_del(&bridge->list);
	kfree(bridge);
}

struct acpi_root_hp_work {
	struct work_struct work;
	acpi_handle handle;
	u32 type;
	void *context;
};

static void alloc_acpi_root_hp_work(acpi_handle handle, u32 type,
					void *context,
					void (*func)(struct work_struct *work))
{
	struct acpi_root_hp_work *hp_work;
	int ret;

	hp_work = kmalloc(sizeof(*hp_work), GFP_KERNEL);
	if (!hp_work)
		return;

	hp_work->handle = handle;
	hp_work->type = type;
	hp_work->context = context;

	INIT_WORK(&hp_work->work, func);
	ret = queue_work(kacpi_hotplug_wq, &hp_work->work);
	if (!ret)
		kfree(hp_work);
}

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

static int acpi_root_evaluate_object(acpi_handle handle, char *cmd, int val)
{
	acpi_status status;
	struct acpi_object_list arg_list;
	union acpi_object arg;

	arg_list.count = 1;
	arg_list.pointer = &arg;
	arg.type = ACPI_TYPE_INTEGER;
	arg.integer.value = val;

	status = acpi_evaluate_object(handle, cmd, &arg_list, NULL);
	if (ACPI_FAILURE(status)) {
		pr_warn("%s: %s to %d failed\n",
				 __func__, cmd, val);
		return -1;
	}

	return 0;
}

static void handle_root_bridge_removal(acpi_handle handle,
		 struct acpi_root_bridge *bridge)
{
	u32 flags = 0;
	struct acpi_device *device;

	if (bridge) {
		flags = bridge->flags;
		remove_acpi_root_bridge(bridge);
	}

	if (!acpi_bus_get_device(handle, &device)) {
		int ret_val;

		/* remove pci devices at first */
		ret_val = acpi_bus_trim(device, 0);
		printk(KERN_DEBUG "acpi_bus_trim stop return %x\n", ret_val);

		/* remove acpi devices */
		ret_val = acpi_bus_trim(device, 1);
		printk(KERN_DEBUG "acpi_bus_trim remove return %x\n", ret_val);
	}

	if (flags & ROOT_BRIDGE_HAS_PS3) {
		acpi_status status;

		status = acpi_evaluate_object(handle, "_PS3", NULL, NULL);
		if (ACPI_FAILURE(status))
			pr_warn("%s: _PS3 failed\n", __func__);
	}
	if (flags & ROOT_BRIDGE_HAS_EJ0)
		acpi_root_evaluate_object(handle, "_EJ0", 1);
}

static void _handle_hotplug_event_root(struct work_struct *work)
{
	struct acpi_root_bridge *bridge;
	char objname[64];
	struct acpi_buffer buffer = { .length = sizeof(objname),
				      .pointer = objname };
	struct acpi_root_hp_work *hp_work;
	acpi_handle handle;
	u32 type;

	hp_work = container_of(work, struct acpi_root_hp_work, work);
	handle = hp_work->handle;
	type = hp_work->type;

	bridge = acpi_root_handle_to_bridge(handle);

	acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	switch (type) {
	case ACPI_NOTIFY_BUS_CHECK:
		/* bus enumerate */
		printk(KERN_DEBUG "%s: Bus check notify on %s\n", __func__,
				 objname);
		if (!bridge) {
			handle_root_bridge_insertion(handle);
			add_acpi_root_bridge(handle);
		}

		break;

	case ACPI_NOTIFY_DEVICE_CHECK:
		/* device check */
		printk(KERN_DEBUG "%s: Device check notify on %s\n", __func__,
				 objname);
		if (!bridge) {
			handle_root_bridge_insertion(handle);
			add_acpi_root_bridge(handle);
		}
		break;

	case ACPI_NOTIFY_EJECT_REQUEST:
		/* request device eject */
		printk(KERN_DEBUG "%s: Device eject notify on %s\n", __func__,
				 objname);
		handle_root_bridge_removal(handle, bridge);
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
	alloc_acpi_root_hp_work(handle, type, context,
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

	add_acpi_root_bridge(handle);

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
