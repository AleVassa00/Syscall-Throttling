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
// IOCTL HANDLER
// ==============================
static long device_ioctl(struct file *file,
                         unsigned int cmd,
                         unsigned long arg)
{
    if (_IOC_TYPE(cmd) != SC_MAGIC)
        return -EINVAL;

    int value;
    char comm[TASK_COMM_LEN];

    switch (cmd) {

    case IOCTL_ADD_UID:
        value = (int)arg;
        return add_uid((uid_t)value);

    case IOCTL_ADD_COMM:
        if (copy_from_user(comm, (char __user *)arg, TASK_COMM_LEN))
            return -EFAULT;

        comm[TASK_COMM_LEN - 1] = '\0';
        return add_comm(comm);

    case IOCTL_REMOVE_UID:
        value = (int)arg;
        return remove_uid(value);

    case IOCTL_REMOVE_COMM:
        if (copy_from_user(comm, (char __user *)arg, TASK_COMM_LEN))
            return -EFAULT;

        comm[TASK_COMM_LEN - 1] = '\0';
        return remove_comm(comm);

    case IOCTL_SET_MAX:
        value = (int)arg;
        monitor_set_max(value);
        return 0;

    case IOCTL_ENABLE:
        // 👉 controllo configurazione
        if (is_uid_list_empty() && is_comm_list_empty())
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
// FILE OPERATIONS
// ==============================

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
};

// ==============================
// INIT
// ==============================

int device_init(void)
{
    // registra device
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        printk(KERN_ERR "Failed to register device\n");
        return major;
    }

    // crea classe
    dev_class = class_create(DEVICE_NAME);
    if (IS_ERR(dev_class)) {
        printk(KERN_ERR "class_create failed: %ld\n", PTR_ERR(dev_class));
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(dev_class);
    }

    // crea /dev entry
    dev_device = device_create(dev_class, NULL,
                              MKDEV(major, 0),
                              NULL,
                              DEVICE_NAME);

    if (IS_ERR(dev_device)) {
        class_destroy(dev_class);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(dev_device);
    }

    printk(KERN_INFO "Device created: /dev/%s\n", DEVICE_NAME);
    return 0;
}

// ==============================
// CLEANUP
// ==============================

void device_cleanup(void)
{
   if (dev_device){
        device_destroy(dev_class, MKDEV(major, 0));
   }

    if (dev_class){
        class_destroy(dev_class);
    }
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "Device removed\n");
}