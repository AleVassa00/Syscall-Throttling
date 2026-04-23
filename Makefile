obj-m += syscall_monitor.o

# lista file del modulo
syscall_monitor-objs := main.o registry.o device.o monitor.o syscall_hook.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean