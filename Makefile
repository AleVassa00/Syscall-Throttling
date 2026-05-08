obj-m += syscall_monitor.o
syscall_monitor-objs := main.o syscall_hook.o probe.o device.o monitor.o registry.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

load: 
	sudo insmod syscall_monitor.ko
remove: 
	sudo rmmod syscall_monitor

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order