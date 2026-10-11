# Out-of-tree build against a configured 6.18 kernel build directory:
#   make KDIR=~/Projects/lenovo_tab3_7/port/build-618 KSRC=~/Projects/linux-mt8127-dev
KDIR ?= $(HOME)/Projects/lenovo_tab3_7/port/build-618
KSRC ?= $(HOME)/Projects/linux-mt8127-dev
export ARCH = arm
export CROSS_COMPILE ?= $(HOME)/Projects/buildroot-android-tb3/output/host/bin/arm-buildroot-linux-gnueabihf-

all modules:
	$(MAKE) -C $(KSRC) O=$(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KSRC) O=$(KDIR) M=$(CURDIR) clean
