################################################################################
#
# tb3-bluetooth
#
################################################################################

TB3_BLUETOOTH_LICENSE = MIT
TB3_BLUETOOTH_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/tb3-bluetooth
TB3_BLUETOOTH_SITE_METHOD = local

define TB3_BLUETOOTH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/S35bluetooth $(TARGET_DIR)/etc/init.d/S35bluetooth
endef

$(eval $(generic-package))
