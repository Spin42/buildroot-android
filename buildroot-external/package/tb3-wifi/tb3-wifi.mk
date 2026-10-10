################################################################################
#
# tb3-wifi
#
################################################################################

TB3_WIFI_LICENSE = MIT
TB3_WIFI_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/tb3-wifi
TB3_WIFI_SITE_METHOD = local

define TB3_WIFI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/S40wifi $(TARGET_DIR)/etc/init.d/S40wifi
endef

$(eval $(generic-package))
