################################################################################
#
# citronics-initramfs
#
################################################################################

CITRONICS_INITRAMFS_VERSION = 13694b8
CITRONICS_INITRAMFS_SITE = https://github.com/Citronics/initramfs.git
CITRONICS_INITRAMFS_SITE_METHOD = git
CITRONICS_INITRAMFS_LICENSE_FILES = LICENSE

CITRONICS_INITRAMFS_STAGING = $(@D)/initramfs-root
CITRONICS_INITRAMFS_SRC_DIR = initramfs/usr/share/citronics-initramfs
CITRONICS_BINARIES = unudhcpd telnetd busybox kpartx dmsetup sfdisk parted resize2fs
OVERLAY_DIR := $(BR2_EXTERNAL_FP2_PATH)/board/fairphone2/overlay

KERNEL_VERSION := $(shell find $(TARGET_DIR)/lib/modules -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | head -n1)

define copy_libs_for_binary
	# Use readelf to find needed libs and copy them if missing
	for lib in $$(readelf -d $(1) 2>/dev/null | grep NEEDED | sed -n 's/.*\[\(.*\)\].*/\1/p'); do \
		found=0; \
		for libdir in $(TARGET_DIR)/lib $(TARGET_DIR)/usr/lib; do \
			if [ -e $$libdir/$$lib ]; then \
				echo "  Copying library $$lib from $$libdir"; \
				mkdir -p $(CITRONICS_INITRAMFS_STAGING)/lib; \
				cp -aL $$libdir/$$lib $(CITRONICS_INITRAMFS_STAGING)/lib/; \
				found=1; \
				break; \
			else \
				# Look in subdirs too
				path=$$(find $$libdir -type f -name "$$lib" 2>/dev/null | head -n1); \
				if [ -n "$$path" ]; then \
					echo "  Copying library $$lib from $$path"; \
					mkdir -p $(CITRONICS_INITRAMFS_STAGING)/lib; \
					cp -aL "$$path" $(CITRONICS_INITRAMFS_STAGING)/lib/; \
					found=1; \
					break; \
				fi; \
			fi; \
		done; \
		if [ $$found -eq 0 ]; then \
			echo "  Warning: library $$lib not found in any known lib directory"; \
		fi; \
	done; \
	# Also copy interpreter itself (like ld-linux*.so) if needed
	interp=$$(readelf -l $(1) 2>/dev/null | grep "interpreter" | sed -n 's/.*\[\(.*\)\].*/\1/p'); \
	if [ -n "$$interp" ]; then \
		interpname=$$(basename $$interp); \
		found_interp=0; \
		for libdir in $(TARGET_DIR)/lib $(TARGET_DIR)/usr/lib; do \
			if [ -e $$libdir/$$interpname ]; then \
				echo "  Copying interpreter $$interpname from $$libdir"; \
				mkdir -p $(CITRONICS_INITRAMFS_STAGING)/lib; \
				cp -aL $$libdir/$$interpname $(CITRONICS_INITRAMFS_STAGING)/lib/; \
				found_interp=1; \
				break; \
			else \
				path=$$(find $$libdir -type f -name "$$interpname" 2>/dev/null | head -n1); \
				if [ -n "$$path" ]; then \
					echo "  Copying interpreter $$interpname from $$path"; \
					mkdir -p $(CITRONICS_INITRAMFS_STAGING)/lib; \
					cp -aL "$$path" $(CITRONICS_INITRAMFS_STAGING)/lib/; \
					found_interp=1; \
					break; \
				fi; \
			fi; \
		done; \
		if [ $$found_interp -eq 0 ]; then \
			echo "  Warning: interpreter $$interpname not found in any known lib directory"; \
		fi; \
	fi;
endef

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
	cp -aL $(BR2_EXTERNAL)/board/fairphone2/overlay/usr/share/deviceinfo/deviceinfo $(CITRONICS_INITRAMFS_STAGING)/usr/share/deviceinfo

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
        fi; \
        $(call copy_libs_for_binary,$$src)
    done

	cd $(CITRONICS_INITRAMFS_STAGING)/bin && \
		ln -sf busybox sh \

	# Create CPIO archive
	(cd $(CITRONICS_INITRAMFS_STAGING) && \
		find . | cpio -o -H newc --owner root:root > ../initramfs.cpio)

	# Compress the CPIO archive
	gzip -f $(@D)/initramfs.cpio

	@echo "Initramfs archive created at $(@D)/initramfs.cpio.gz"

	cp $(@D)/initramfs.cpio.gz $(BINARIES_DIR)/initramfs.gz
endef

$(eval $(generic-package))
