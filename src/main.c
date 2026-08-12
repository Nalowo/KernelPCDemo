// SPDX-License-Identifier: GPL-2.0
/*
 * Debuggable char-device template.
 *
 * Registers a misc device /dev/template backed by a small in-kernel buffer.
 * The interesting functions (open/read/write/release) live in .text, so
 * breakpoints on them bind cleanly after `lx-symbols` -- unlike __init code in
 * the ephemeral .init.text section. Drive them from the guest:
 *
 *     echo hello > /dev/template     # -> template_write
 *     cat /dev/template              # -> template_read
 *
 * See README "Отладка" for the full GDB workflow and cases.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define TEMPLATE_BUF_SIZE 1024

static char *template_buf;		/* backing store */
static size_t template_len;		/* bytes currently stored */
static DEFINE_MUTEX(template_lock);	/* serialises access to buf/len */

static ssize_t template_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	ssize_t ret;

	/* gdb: `break template_read` -- triggered by `cat /dev/template`. */
	mutex_lock(&template_lock);

	if (*ppos >= template_len) {
		ret = 0;			/* EOF */
		goto out;
	}
	if (count > template_len - *ppos)
		count = template_len - *ppos;

	if (copy_to_user(ubuf, template_buf + *ppos, count)) {
		ret = -EFAULT;
		goto out;
	}
	*ppos += count;
	ret = count;
out:
	mutex_unlock(&template_lock);
	return ret;
}

static ssize_t template_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	ssize_t ret;

	/* gdb: `break template_write` -- triggered by `echo x > /dev/template`. */
	if (count > TEMPLATE_BUF_SIZE)
		count = TEMPLATE_BUF_SIZE;

	mutex_lock(&template_lock);

	if (copy_from_user(template_buf, ubuf, count)) {
		ret = -EFAULT;
		goto out;
	}
	template_len = count;
	*ppos = count;
	ret = count;
	pr_info("wrote %zd bytes\n", ret);
out:
	mutex_unlock(&template_lock);
	return ret;
}

static int template_open(struct inode *inode, struct file *file)
{
	pr_info("open\n");
	return 0;
}

static int template_release(struct inode *inode, struct file *file)
{
	pr_info("release\n");
	return 0;
}

static const struct file_operations template_fops = {
	.owner		= THIS_MODULE,
	.open		= template_open,
	.release	= template_release,
	.read		= template_read,
	.write		= template_write,
	.llseek		= default_llseek,
};

static struct miscdevice template_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "template",
	.fops	= &template_fops,
	.mode	= 0666,
};

static int __init template_init(void)
{
	int ret;

	template_buf = kzalloc(TEMPLATE_BUF_SIZE, GFP_KERNEL);
	if (!template_buf)
		return -ENOMEM;

	ret = misc_register(&template_dev);
	if (ret) {
		kfree(template_buf);
		return ret;
	}

	pr_info("loaded: /dev/%s (minor %d)\n",
		template_dev.name, template_dev.minor);
	return 0;
}

static void __exit template_exit(void)
{
	misc_deregister(&template_dev);
	kfree(template_buf);
	pr_info("unloaded\n");
}

module_init(template_init);
module_exit(template_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nalowo");
MODULE_DESCRIPTION("Debuggable char-device template");
MODULE_VERSION("0.2");
