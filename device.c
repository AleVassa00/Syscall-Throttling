#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/limits.h>

#include "device.h"
#include "monitor.h"
#include "registry.h"

#define IOCTL_ADD_SYSCALL           _IOW('a', 1, int)
#define IOCTL_REMOVE_SYSCALL        _IOW('a', 2, int)

#define IOCTL_ADD_UID               _IOW('a', 3, int)
#define IOCTL_REMOVE_UID            _IOW('a', 4, int)

#define IOCTL_ADD_PROGRAM_NAME      _IOW('a', 5, char *)
#define IOCTL_REMOVE_PROGRAM_NAME   _IOW('a', 6, char *)

#define IOCTL_ENABLE_MONITOR        _IO('a', 7)
#define IOCTL_DISABLE_MONITOR       _IO('a', 8)

#define IOCTL_SET_MAX               _IOW('a', 9, int)

static int get_int_from_user(unsigned long arg, int *value)
{
    if (copy_from_user(value, (int __user *)arg, sizeof(int)))
        return -EFAULT;

    return 0;
}

static int get_path_from_user(unsigned long arg, char *buf)
{
    long ret;

    ret = strncpy_from_user(buf, (const char __user *)arg, PATH_MAX);

    if (ret < 0)
        return -EFAULT;

    if (ret == 0)
        return -EINVAL;

    if (ret >= PATH_MAX)
        return -ENAMETOOLONG;

    buf[PATH_MAX - 1] = '\0';

    return 0;
}

static int check_root(void)
{
    if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
        return -EPERM;

    return 0;
}

static long device_ioctl(struct file *file,
                         unsigned int cmd,
                         unsigned long arg)
{
    int value;
    char *prog_path;          // <- puntatore, non array
    int ret;

    ret = check_root();
    if (ret)
        return ret;

    /* Alloca solo per i comandi che ne hanno bisogno */
    prog_path = NULL;
    if (cmd == IOCTL_ADD_PROGRAM_NAME || cmd == IOCTL_REMOVE_PROGRAM_NAME) {
        prog_path = kmalloc(PATH_MAX, GFP_KERNEL);
        if (!prog_path)
            return -ENOMEM;
    }

    switch (cmd) {
    case IOCTL_ADD_SYSCALL:
        ret = get_int_from_user(arg, &value);
        if (!ret) ret = registry_add_syscall(value);
        break;
    case IOCTL_REMOVE_SYSCALL:
        ret = get_int_from_user(arg, &value);
        if (!ret) ret = registry_remove_syscall(value);
        break;
    case IOCTL_ADD_UID:
        ret = get_int_from_user(arg, &value);
        if (!ret) ret = add_user_id(value);
        break;
    case IOCTL_REMOVE_UID:
        ret = get_int_from_user(arg, &value);
        if (!ret) ret = remove_user_id(value);
        break;
    case IOCTL_ADD_PROGRAM_NAME:
        ret = get_path_from_user(arg, prog_path);
        if (!ret) ret = add_prog_inode(prog_path);
        break;
    case IOCTL_REMOVE_PROGRAM_NAME:
        ret = get_path_from_user(arg, prog_path);
        if (!ret) ret = remove_prog_inode(prog_path);
        break;
    case IOCTL_ENABLE_MONITOR:
        monitor_set_enabled(1);
        ret = 0;
        break;
    case IOCTL_DISABLE_MONITOR:
        monitor_set_enabled(0);
        ret = 0;
        break;
    case IOCTL_SET_MAX:
        ret = get_int_from_user(arg, &value);
        if (!ret) {
            if (value <= 0) ret = -EINVAL;
            else { monitor_set_max((unsigned long)value); ret = 0; }
        }
        break;
    default:
        ret = -EINVAL;
        break;
    }

    kfree(prog_path);   // safe anche se NULL
    return ret;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
};

static struct miscdevice dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "syscall_monitor",
    .fops = &fops,
};

int device_init(void)
{
    int ret;

    ret = misc_register(&dev);
    if (ret) {
        printk(KERN_ERR "[DEVICE] misc_register failed ret=%d\n", ret);
        return ret;
    }

    printk(KERN_INFO "[DEVICE] registered /dev/syscall_monitor\n");

    return 0;
}

void device_cleanup(void)
{
    misc_deregister(&dev);

    printk(KERN_INFO "[DEVICE] deregistered /dev/syscall_monitor\n");
}