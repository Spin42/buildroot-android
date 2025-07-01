################################################################################
#
# citronics-initramfs
#
################################################################################

CITRONICS_INITRAMFS_VERSION = 6e058d7
CITRONICS_INITRAMFS_SITE = https://github.com/Citronics/initramfs.git
CITRONICS_INITRAMFS_SITE_METHOD = git
CITRONICS_INITRAMFS_LICENSE_FILES = LICENSE

CITRONICS_INITRAMFS_STAGING = $(@D)/initramfs-root
CITRONICS_INITRAMFS_SRC_DIR = initramfs/usr/share/citronics-initramfs
CITRONICS_BINARIES = unudhcpd busybox kpartx dmsetup parted resize2fs blkid sfdisk lsblk partprobe udevd udevadm kmod e2fsck telnetd

DEFCONFIG_NAME = $(subst ",,$(notdir $(BR2_DEFCONFIG)))
BOARD_NAME = $(patsubst %_defconfig,%,$(DEFCONFIG_NAME))
BOARD_DIR = $(BR2_EXTERNAL)/board/$(BOARD_NAME)

KERNEL_VERSION := $(shell find $(TARGET_DIR)/lib/modules -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | head -n1)

define CITRONICS_INITRAMFS_BUILD_CMDS
	true
endef

define CITRONICS_INITRAMFS_INSTALL_TARGET_CMDS
	mkdir -p $(CITRONICS_INITRAMFS_STAGING)

	mkdir -p $(CITRONICS_INITRAMFS_STAGING)/{bin,sbin,usr/bin,usr/sbin,lib,etc,proc,sys,dev,tmp,var,run,mnt,root,sysroot}
	# Ensure /tmp and /run are writable at runtime
	chmod 1777 $(CITRONICS_INITRAMFS_STAGING)/tmp
	chmod 1777 $(CITRONICS_INITRAMFS_STAGING)/run

	# Copy files from repo directory to root of initramfs staging area
	cp -aL $(@D)/$(CITRONICS_INITRAMFS_SRC_DIR)/* $(CITRONICS_INITRAMFS_STAGING)/
	mkdir -p $(CITRONICS_INITRAMFS_STAGING)/usr/share/deviceinfo
	cp -RaL $(CITRONICS_INITRAMFS_STAGING)/misc $(CITRONICS_INITRAMFS_STAGING)/usr/share/misc
	cp -RaL $(BOARD_DIR)/overlay/usr/share/deviceinfo $(CITRONICS_INITRAMFS_STAGING)/usr/share/

	mkdir -p $(CITRONICS_INITRAMFS_STAGING)/lib/modules/$(KERNEL_VERSION)
	@echo "Detected kernel version: $(KERNEL_VERSION)"
	@echo "Copying kernel modules listed in modules file..."

	while read -r modpath; do \
		src="$(TARGET_DIR)/lib/modules/$(KERNEL_VERSION)/$$modpath"; \
		dest="$(CITRONICS_INITRAMFS_STAGING)/lib/modules/$(KERNEL_VERSION)/$$modpath"; \
		if [ -f "$$src" ]; then \
			mkdir -p "$$(dirname "$$dest")"; \
			cp -aL "$$src" "$$dest"; \
		else \
			echo "Warning: kernel module file $$src not found!"; \
		fi; \
	done < $(BR2_EXTERNAL)/package/citronics-initramfs/modules

	@echo "Copying required binaries from target to initramfs..."
    for bin in $(CITRONICS_BINARIES); do \
        echo "Searching for $$bin..."; \
        src=""; \
        for dir in usr/sbin sbin usr/bin bin; do \
            candidate="$(TARGET_DIR)/$$dir/$$bin"; \
            echo "  Checking $$candidate..."; \
            if [ -x "$$candidate" ]; then \
                src="$$candidate"; \
                break; \
            fi; \
        done; \
        if [ -z "$$src" ]; then \
            echo "  Warning: binary $$bin not found in target"; \
        else \
            relpath=$$(realpath --relative-to=$(TARGET_DIR) $$src); \
            dest="$(CITRONICS_INITRAMFS_STAGING)/$$relpath"; \
            echo "  Copying $$src to $$dest"; \
            mkdir -p "$$(dirname $$dest)"; \
            cp -aL "$$src" "$$dest"; \
    		$(BR2_EXTERNAL)/package/citronics-initramfs/scripts/copy_libs_for_binary.sh $$src $(TARGET_DIR) $(CITRONICS_INITRAMFS_STAGING) ;\
        fi; \
    done

	cd $(CITRONICS_INITRAMFS_STAGING)/bin && \
		ln -sf busybox sh && \
		ln -sf busybox telnetd && \
		ln -sf busybox getty \

	cp -R $(TARGET_DIR)/usr/lib/udev $(CITRONICS_INITRAMFS_STAGING)/usr/lib/

	# Create CPIO archive
	(cd $(CITRONICS_INITRAMFS_STAGING) && \
		find . | cpio -o -H newc --owner root:root > ../initramfs.cpio)

	# Compress the CPIO archive
	gzip -f $(@D)/initramfs.cpio

	@echo "Initramfs archive created at $(@D)/initramfs.cpio.gz"

	cp $(@D)/initramfs.cpio.gz $(BINARIES_DIR)/initramfs.gz
endef

$(eval $(generic-package))
