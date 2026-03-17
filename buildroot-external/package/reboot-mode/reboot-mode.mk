################################################################################
#
# reboot-mode
#
################################################################################

REBOOT_MODE_LICENSE = MIT
REBOOT_MODE_LICENSE_FILES =
REBOOT_MODE_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/reboot-mode
REBOOT_MODE_SITE_METHOD = local

define REBOOT_MODE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		$(@D)/reboot-mode.c -o $(@D)/reboot-mode
endef

define REBOOT_MODE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/reboot-mode $(TARGET_DIR)/usr/sbin/reboot-mode
endef

$(eval $(generic-package))
