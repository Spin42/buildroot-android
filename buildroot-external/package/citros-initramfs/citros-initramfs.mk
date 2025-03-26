################################################################################
# citros-initramfs package
################################################################################

CITROS_INITRAMFS_TAG = v0.1
CITROS_INITRAMFS_EXTRA_DOWNLOADS = https://github.com/Spin42/initramfs-citros/releases/download/$(CITROS_INITRAMFS_TAG)/initramfs-$(CITROS_INITRAMFS_TAG).cpio.gz

define CITROS_INITRAMFS_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(CITROS_INITRAMFS_DL_DIR)/initramfs-$(CITROS_INITRAMFS_TAG).cpio.gz $(TARGET_DIR)/boot/initramfs.cpio.gz
endef

$(eval $(generic-package))
