#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/limits.h>
#include <linux/slab.h>



#include "syscall_hook.h"
#include "device.h"
#include "monitor.h"
#include "registry.h"
#include "stats.h"

#define IOCTL_ADD_SYSCALL           _IOW('a', 1, int)
#define IOCTL_REMOVE_SYSCALL        _IOW('a', 2, int)

#define IOCTL_ADD_UID               _IOW('a', 3, int)
#define IOCTL_REMOVE_UID            _IOW('a', 4, int)

#define IOCTL_ADD_PROGRAM_NAME      _IOW('a', 5, char *)
#define IOCTL_REMOVE_PROGRAM_NAME   _IOW('a', 6, char *)

#define IOCTL_ENABLE_MONITOR        _IO('a', 7)
#define IOCTL_DISABLE_MONITOR       _IO('a', 8)

#define IOCTL_SET_MAX               _IOW('a', 9, int)

#define IOCTL_GET_STATS             _IOR('a', 10, struct monitor_stats)

#define IOCTL_LIST_UIDS      _IOR('a', 11, struct uid_list)
#define IOCTL_LIST_PROGRAMS  _IOR('a', 12, struct prog_list)
#define IOCTL_LIST_SYSCALLS  _IOR('a', 13, struct syscall_list)

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

static int handle_program_ioctl(unsigned long arg, int add)
{
    char *prog_path;
    int ret;

    prog_path = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!prog_path)
        return -ENOMEM;

    ret = get_path_from_user(arg, prog_path);
    if (!ret) {
        if (add)
            ret = add_prog_inode(prog_path);
        else
            ret = remove_prog_inode(prog_path);
    }

    kfree(prog_path);
    return ret;
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int value;
    int ret;

    ret = check_root();
    if (ret){
        return ret;
    }
    switch (cmd) {
    case IOCTL_ADD_SYSCALL:
        ret = get_int_from_user(arg, &value);
        if (ret){
            break;
        }
        ret = add_syscall(value);
        if (ret){
            break;
        }
        ret = add_syscall_hook(value);
        if (ret) {
            remove_syscall(value);
            break;
        }
        break;
    case IOCTL_REMOVE_SYSCALL:
        ret = get_int_from_user(arg, &value);
        if (ret)
            break;
        ret = remove_syscall_hook(value);
        if (ret)
            break;
        ret = remove_syscall(value);
        if (ret){
            printk(KERN_ERR "[DEVICE] remove_syscall failed after unhook ret=%d\n", ret);
        }
        break;
    case IOCTL_ADD_UID:
        ret = get_int_from_user(arg, &value);
        if (!ret)
            ret = add_user_id(value);
        break;

    case IOCTL_REMOVE_UID:
        ret = get_int_from_user(arg, &value);
        if (!ret)
            ret = remove_user_id(value);
        break;

    case IOCTL_ADD_PROGRAM_NAME:
        ret = handle_program_ioctl(arg, 1);
        break;

    case IOCTL_REMOVE_PROGRAM_NAME:
        ret = handle_program_ioctl(arg, 0);
        break;

    case IOCTL_ENABLE_MONITOR:
        monitor_set_enabled(true);
        ret = 0;
        break;

    case IOCTL_DISABLE_MONITOR:
        monitor_set_enabled(false);
        ret = 0;
        break;

    case IOCTL_SET_MAX:
        ret = get_int_from_user(arg, &value);
        if (!ret) {
            if (value <= 0)
                ret = -EINVAL;
            else {
                monitor_set_max((unsigned long)value);
                ret = 0;
            }
        }
        break;
    case IOCTL_GET_STATS:{         
        struct monitor_stats stats;
        stats_get(&stats);
        if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
            ret = -EFAULT;
        else
            ret = 0;
        break;
    }
    case IOCTL_LIST_UIDS: {
        struct uid_list *list;

        list = kmalloc(sizeof(*list), GFP_KERNEL);
        if (!list) {
            ret = -ENOMEM;
            break;
        }

        ret = get_uid_list(list);
        if (!ret) {
            if (copy_to_user((void __user *)arg, list, sizeof(*list)))
                ret = -EFAULT;
        }

        kfree(list);
        break;
    }

    case IOCTL_LIST_PROGRAMS: {
        struct prog_list *list;

        list = kmalloc(sizeof(*list), GFP_KERNEL);
        if (!list) {
            ret = -ENOMEM;
            break;
        }

        ret = get_prog_list(list);
        if (!ret) {
            if (copy_to_user((void __user *)arg, list, sizeof(*list)))
                ret = -EFAULT;
        }

        kfree(list);
        break;
    }

    case IOCTL_LIST_SYSCALLS: {
        struct syscall_list *list;

        list = kmalloc(sizeof(*list), GFP_KERNEL);
        if (!list) {
            ret = -ENOMEM;
            break;
        }

        ret = get_syscall_list(list);
        if (!ret) {
            if (copy_to_user((void __user *)arg, list, sizeof(*list)))
                ret = -EFAULT;
        }

        kfree(list);
        break;
    }

    default:
        ret = -ENOTTY;
        break;
    }

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
    /*
        * Misc device usato per esporre /dev/syscall_monitor.
        * È sufficiente per il progetto perché il modulo necessita
        * di un singolo character device dedicato alle ioctl di
        * configurazione del monitor.
    */

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