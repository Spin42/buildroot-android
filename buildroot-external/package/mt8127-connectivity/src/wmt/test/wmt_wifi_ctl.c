// SPDX-License-Identifier: GPL-2.0
/*
 * Bring-up helper: switches the WiFi function of the WMT core on and off from debugfs, the way
 * the stock userspace did through /dev/wmtWifi. Not needed once the WiFi driver calls the WMT
 * core itself.
 */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "wmt_exp.h"

static struct dentry *dir;
static bool wifi_on;

static ssize_t power_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char val[4] = {};
	bool on;

	if (copy_from_user(val, buf, min(count, sizeof(val) - 1)))
		return -EFAULT;
	on = val[0] == '1';

	if (on == wifi_on)
		return count;

	if (on ? mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI) == MTK_WCN_BOOL_FALSE :
		 mtk_wcn_wmt_func_off(WMTDRV_TYPE_WIFI) == MTK_WCN_BOOL_FALSE)
		return -EIO;

	wifi_on = on;
	return count;
}

static ssize_t power_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	char val[3] = { wifi_on ? '1' : '0', '\n' };

	return simple_read_from_buffer(buf, count, ppos, val, 2);
}

static const struct file_operations power_fops = {
	.owner = THIS_MODULE,
	.read = power_read,
	.write = power_write,
};

static int __init wmt_wifi_ctl_init(void)
{
	dir = debugfs_create_dir("wmt_wifi", NULL);
	debugfs_create_file("power", 0600, dir, NULL, &power_fops);
	return 0;
}

static void __exit wmt_wifi_ctl_exit(void)
{
	if (wifi_on)
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_WIFI);
	debugfs_remove_recursive(dir);
}

module_init(wmt_wifi_ctl_init);
module_exit(wmt_wifi_ctl_exit);
MODULE_DESCRIPTION("Bring-up control of the WMT WiFi function");
MODULE_LICENSE("GPL");
