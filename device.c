#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#include "device.h"
#include "registry.h"
#include "monitor.h"

#define DEVICE_NAME "syscall_monitor"

static int major;
static struct class *dev_class = NULL;
static struct device *dev_device = NULL;

// ==============================
// IOCTL
// ==============================

static long device_ioctl(struct file *file,
                         unsigned int cmd,
                         unsigned long arg)
{
    if (_IOC_TYPE(cmd) != SC_MAGIC)
        return -EINVAL;

    if (current_uid().val != 0)
        return -EPERM;

    int value;
    char comm[TASK_COMM_LEN];

    switch (cmd) {

    case IOCTL_ADD_SYSCALL:
        return add_syscall((int)arg);

    case IOCTL_REMOVE_SYSCALL:
        return remove_syscall((int)arg);

    case IOCTL_ADD_UID:
        return add_uid((uid_t)arg);

    case IOCTL_ADD_COMM:
        if (copy_from_user(comm, (char __user *)arg, TASK_COMM_LEN))
            return -EFAULT;
        comm[TASK_COMM_LEN - 1] = '\0';
        return add_comm(comm);

    case IOCTL_REMOVE_UID:
        return remove_uid((uid_t)arg);

    case IOCTL_REMOVE_COMM:
        if (copy_from_user(comm, (char __user *)arg, TASK_COMM_LEN))
            return -EFAULT;
        comm[TASK_COMM_LEN - 1] = '\0';
        return remove_comm(comm);

    case IOCTL_SET_MAX:
        monitor_set_max((int)arg);
        return 0;

    case IOCTL_ENABLE:
        if (is_syscall_list_empty() ||
           (is_uid_list_empty() && is_comm_list_empty()))
            return -EINVAL;

        monitor_enable();
        return 0;

    case IOCTL_DISABLE:
        monitor_disable();
        return 0;

    default:
        return -EINVAL;
    }
}

// ==============================
// READ
// ==============================

static ssize_t device_read(struct file *file,
                          char __user *buf,
                          size_t len,
                          loff_t *offset)
{
    char tmp[256];
    int n;

    if (*offset > 0)
        return 0;

    n = snprintf(tmp, sizeof(tmp),
        "Monitor: %s\nPeak delay: %llu\nPeak blocked: %d\nAvg blocked: %llu\n",
        monitor_is_enabled() ? "ON" : "OFF",
        get_peak_delay(),
        get_peak_blocked(),
        get_avg_blocked()
    );

    if (copy_to_user(buf, tmp, n))
        return -EFAULT;

    *offset += n;
    return n;
}

// ==============================

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
    .read = device_read,
};

// ==============================

int device_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);

    dev_class = class_create(DEVICE_NAME);
    dev_device = device_create(dev_class, NULL,
                              MKDEV(major, 0), NULL,
                              DEVICE_NAME);
    return 0;
}

void device_cleanup(void)
{
    device_destroy(dev_class, MKDEV(major, 0));
    class_destroy(dev_class);
    unregister_chrdev(major, DEVICE_NAME);
}