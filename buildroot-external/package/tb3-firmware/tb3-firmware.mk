################################################################################
#
# tb3-firmware
#
################################################################################

TB3_FIRMWARE_SITE = $(call qstrip,$(BR2_PACKAGE_TB3_FIRMWARE_DIR))
TB3_FIRMWARE_SITE_METHOD = local
TB3_FIRMWARE_LICENSE = proprietary

define TB3_FIRMWARE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/firmware
	cp -a $(@D)/. $(TARGET_DIR)/lib/firmware/
endef

$(eval $(generic-package))
