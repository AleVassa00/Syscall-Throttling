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
    if (_IOC_TYPE(cmd) != SC_MAGIC){
        return -EINVAL;
    }

    int value;
    char comm[TASK_COMM_LEN];

    switch (cmd) {

    case IOCTL_ADD_UID:
        value = (int)arg;
        add_uid((uid_t)value);
        printk(KERN_INFO "Added UID: %d\n", value);
        break;

    case IOCTL_ADD_SYSCALL:
        value = (int)arg;
        add_syscall(value);
        printk(KERN_INFO "Added syscall: %d\n", value);
        break;

    case IOCTL_ADD_COMM:
        if (copy_from_user(comm, (char __user *)arg, TASK_COMM_LEN))
            return -EFAULT;

        comm[TASK_COMM_LEN - 1] = '\0';
        add_comm(comm);
        printk(KERN_INFO "Added comm: %s\n", comm);
        break;

    case IOCTL_ENABLE:
        monitor_enable();
        printk(KERN_INFO "Monitor enabled\n");
        break;

    case IOCTL_DISABLE:
        monitor_disable();
        printk(KERN_INFO "Monitor disabled\n");
        break;

    case IOCTL_SET_MAX:
        value = (int)arg;
        monitor_set_max(value);
        printk(KERN_INFO "Max calls set: %d\n", value);
        break;
    case IOCTL_REMOVE_UID:
        value = (int)arg;
        remove_uid(value);
        break;

    case IOCTL_REMOVE_SYSCALL:
        value = (int)arg;
        remove_syscall(value);
        break;

    case IOCTL_REMOVE_COMM:
        if (copy_from_user(comm, (char __user *)arg, 16))
            return -EFAULT;
        comm[15] = '\0';
        remove_comm(comm);
        break;

    default:
        return -EINVAL;
    }

    return 0;
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
    dev_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(dev_class)) {
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
        devicedevice_destroy(dev_class, MKDEV(major, 0));
   }

    if (dev_class){
        class_destroy(dev_class);
    }
    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "Device removed\n");
}