################################################################################
#
# tb3-firmware
#
################################################################################

TB3_FIRMWARE_SITE = $(call qstrip,$(BR2_PACKAGE_TB3_FIRMWARE_DIR))
# The local site method requires a non-empty _SITE even when the package is disabled.
ifeq ($(BR2_PACKAGE_TB3_FIRMWARE),y)
TB3_FIRMWARE_SITE_METHOD = local
endif
TB3_FIRMWARE_LICENSE = proprietary

define TB3_FIRMWARE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/firmware
	rsync -a --exclude=".stamp_*" --exclude=".files-list*" $(@D)/ $(TARGET_DIR)/lib/firmware/
endef

$(eval $(generic-package))
