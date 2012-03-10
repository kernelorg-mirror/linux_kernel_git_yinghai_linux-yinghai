/*
 *  Code to emulate SCI interrupt for Hotplug node insertion/removal
 */
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/acpi.h>

#include "internal.h"

#include "acpica/accommon.h"
#include "acpica/acnamesp.h"
#include "acpica/acevents.h"

#define _COMPONENT		ACPI_SYSTEM_COMPONENT
ACPI_MODULE_NAME("sci_emu");

static void acpi_sci_notify_client(char *acpi_name, u32 event);
static int acpi_sci_notify_write_proc(struct file *file, const char *buffer, \
	unsigned long count, void *data);
struct proc_dir_entry *acpi_sci_dir;

static int acpi_sci_notify_write_proc(struct file *file, const char *buffer,
				      unsigned long count, void *data)
{
	u32 event;
	char *name1 = NULL;
	char *name2 = NULL;
	char *end_name = NULL;
	const char *delim = " ";
	char *temp_buf = NULL;
	char *temp_buf_addr = NULL;

	temp_buf = kmalloc(count+1, GFP_ATOMIC);
	if (!temp_buf) {
		printk(KERN_WARNING PREFIX
		 "acpi_sci_notify_wire_proc: Memory allocation failed\n");
		return count;
	}
	temp_buf[count] = '\0';
	temp_buf_addr = temp_buf;
	memcpy(temp_buf, buffer, count);
	name1 = strsep(&temp_buf, delim);
	name2 = strsep(&temp_buf, delim);

	if (name1 && name2)
		event = simple_strtoul(name2, &end_name, 10);
	else {
		printk(KERN_WARNING PREFIX "unknown device\n");
		kfree(temp_buf_addr);
		return count;
	}

	printk(KERN_INFO PREFIX
		"ACPI device name is <%s>, event code is <%d>\n",
		name1, event);

	acpi_sci_notify_client(name1, event);

	kfree(temp_buf_addr);

	return count;
}

static void acpi_sci_notify_client(char *acpi_name, u32 event)
{
	struct acpi_namespace_node *node;
	acpi_status status, status1;
	acpi_handle hlsb, hsb;
	union acpi_operand_object *obj_desc;

	status = acpi_get_handle(NULL, "\\_SB", &hsb);
	status1 = acpi_get_handle(hsb, acpi_name, &hlsb);
	if (ACPI_FAILURE(status) || ACPI_FAILURE(status1)) {
		printk(KERN_ERR PREFIX
	"acpi getting handle to <\\_SB.%s> failed inside notify_client\n",
			acpi_name);
		return;
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		printk(KERN_ERR PREFIX "Acquiring acpi namespace mutext failed\n");
		return;
	}

	node = acpi_ns_validate_handle(hlsb);
	if (!node) {
		acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
		printk(KERN_ERR PREFIX "Mapping handle to node failed\n");
		return;
	}

	/*
	 * Check for internal object and make sure there is a handler
	 * registered for this object
	 */
	obj_desc = acpi_ns_get_attached_object(node);
	if (obj_desc) {
		if (obj_desc->common_notify.system_notify) {
			/*
			 * Release the lock and queue the item for later
			 * exectuion
			 */
			acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
			status = acpi_ev_queue_notify_request(node, event);
			if (ACPI_FAILURE(status))
				printk(KERN_ERR PREFIX "acpi_ev_queue_notify_request failed\n");
			else
				printk(KERN_INFO PREFIX "Notify event is queued\n");
			return;
		}
	} else {
		printk(KERN_INFO PREFIX "Notify handler not registered for this device\n");
	}

	acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return;
}

int __init acpi_init_sci_emulate(void)
{
	struct proc_dir_entry   *notify_entry = NULL;

	ACPI_FUNCTION_TRACE("acpi_init_sci_emulate");

	acpi_sci_dir = proc_mkdir("sci", acpi_root_dir);
	if (!acpi_sci_dir)
		return_VALUE(-ENODEV);

	notify_entry = create_proc_entry("notify", \
		S_IWUGO|S_IRUGO, acpi_sci_dir);
	if (!notify_entry) {
		ACPI_DEBUG_PRINT((ACPI_DB_INIT,
			"Unable to create '%s' fs entry\n", "notify"));
	} else {
		notify_entry->write_proc = acpi_sci_notify_write_proc;
		notify_entry->data = NULL;
	}

	return_VALUE(0);
}
