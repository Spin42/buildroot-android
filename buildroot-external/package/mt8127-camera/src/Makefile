# Out-of-tree build against the kernel Buildroot builds for the tablet:
#   make [KDIR=<kernel build dir>]
KDIR ?= $(HOME)/Projects/buildroot-android-tb3/output/build/linux-custom
export ARCH = arm
export CROSS_COMPILE ?= $(HOME)/Projects/buildroot-android-tb3/output/host/bin/arm-buildroot-linux-gnueabihf-

all modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
