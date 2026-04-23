#ifndef DEVICE_H
#define DEVICE_H

#include <linux/ioctl.h>

#define SC_MAGIC 'k'

// comandi
#define IOCTL_ADD_UID     _IOW(SC_MAGIC, 1, int)
#define IOCTL_ADD_COMM _IOW(SC_MAGIC, 2, char *)
#define IOCTL_ADD_SYSCALL _IOW(SC_MAGIC, 3, int)
#define IOCTL_ENABLE      _IO(SC_MAGIC, 4)
#define IOCTL_DISABLE     _IO(SC_MAGIC, 5)
#define IOCTL_SET_MAX     _IOW(SC_MAGIC, 6, int)
#define IOCTL_REMOVE_UID     _IOW(SC_MAGIC, 7, int)
#define IOCTL_REMOVE_COMM    _IOW(SC_MAGIC, 8, char *)
#define IOCTL_REMOVE_SYSCALL _IOW(SC_MAGIC, 9, int)

// init/cleanup
int device_init(void);
void device_cleanup(void);

#endif