################################################################################
#
# One-time service to resize root filesystem
#
################################################################################

FIRST_BOOT_RESIZE_VERSION = 1.0
FIRST_BOOT_RESIZE_SITE = $(BR2_EXTERNAL)/package/first-boot-resize
FIRST_BOOT_RESIZE_SITE_METHOD = local

define FIRST_BOOT_RESIZE_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/S15first-boot-resize $(TARGET_DIR)/etc/init.d/S15first-boot-resize
endef

$(eval $(generic-package))
