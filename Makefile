ifneq ($(KERNELRELEASE),)
#(module name)-objs
qe_usb_daq-objs:=usb_trans.o blk_daq.o usb_daq.o
#qe=qinger, the driver name must be different from all file names
obj-m:=qe_usb_daq.o

EXTRA_LDFLAGS := --start-group

else
#KERNELDIR:=/lib/modules/$(shell uname -r)/build
#KERNELDIR:=/usr/src/linux-source-deepin-4.4
KERNELDIR:=/usr/src/linux-source-4.4.0
PWD:=$(shell pwd)
#modules:
#	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
#modules_install:
#	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
#default:
all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
.PHONY: all clean
clean:
	rm -rf *.o *.mod.c *.mod.o *.ko *.order *symvers
endif