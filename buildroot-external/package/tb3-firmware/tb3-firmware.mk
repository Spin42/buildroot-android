################################################################################
#
# tb3-firmware
#
################################################################################

TB3_FIRMWARE_SITE = $(call qstrip,$(BR2_PACKAGE_TB3_FIRMWARE_DIR))
# The directory is only set when the package is enabled, Buildroot needs a
# site for every local package
ifeq ($(TB3_FIRMWARE_SITE),)
TB3_FIRMWARE_SITE = $(TB3_FIRMWARE_PKGDIR)
endif
TB3_FIRMWARE_SITE_METHOD = local
TB3_FIRMWARE_LICENSE = proprietary

define TB3_FIRMWARE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/firmware
	rsync -a --exclude=".stamp_*" --exclude=".files-list*" $(@D)/ $(TARGET_DIR)/lib/firmware/
endef

$(eval $(generic-package))
