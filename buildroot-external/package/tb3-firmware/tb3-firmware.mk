################################################################################
#
# tb3-firmware
#
################################################################################

# The option exists only when the package is selected; every other defconfig still parses this file
TB3_FIRMWARE_SITE = $(or $(call qstrip,$(BR2_PACKAGE_TB3_FIRMWARE_DIR)),$(BR2_EXTERNAL_ANDROID_PATH)/board/tb3_710f/firmware)
TB3_FIRMWARE_SITE_METHOD = local
TB3_FIRMWARE_LICENSE = proprietary

define TB3_FIRMWARE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/firmware
	rsync -a --exclude=".stamp_*" --exclude=".files-list*" $(@D)/ $(TARGET_DIR)/lib/firmware/
endef

$(eval $(generic-package))
