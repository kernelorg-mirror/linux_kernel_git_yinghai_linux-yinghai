/*
 * Architecture specific debugfs files
 *
 * Copyright (C) 2007, Intel Corp.
 *	Huang Ying <ying.huang@intel.com>
 *
 * This file is released under the GPLv2.
 */
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/io.h>
#include <linux/mm.h>

#include <asm/setup.h>

struct dentry *arch_debugfs_dir;
EXPORT_SYMBOL(arch_debugfs_dir);

#ifdef CONFIG_DEBUG_BOOT_PARAMS
static struct debugfs_blob_wrapper boot_params_blob = {
	.data		= &boot_params,
	.size		= sizeof(boot_params),
};

static int __init boot_params_kdebugfs_init(void)
{
	struct dentry *dbp, *version, *data;
	int error = -ENOMEM;

	dbp = debugfs_create_dir("boot_params", NULL);
	if (!dbp)
		return -ENOMEM;

	version = debugfs_create_x16("version", S_IRUGO, dbp,
				     &boot_params.hdr.version);
	if (!version)
		goto err_dir;

	data = debugfs_create_blob("data", S_IRUGO, dbp,
				   &boot_params_blob);
	if (!data)
		goto err_version;

	return 0;

err_version:
	debugfs_remove(version);
err_dir:
	debugfs_remove(dbp);
	return error;
}
#endif /* CONFIG_DEBUG_BOOT_PARAMS */

static int __init arch_kdebugfs_init(void)
{
	int error = 0;

	arch_debugfs_dir = debugfs_create_dir("x86", NULL);
	if (!arch_debugfs_dir)
		return -ENOMEM;

#ifdef CONFIG_DEBUG_BOOT_PARAMS
	error = boot_params_kdebugfs_init();
#endif

	return error;
}
arch_initcall(arch_kdebugfs_init);
