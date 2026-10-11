################################################################################
#
# tb3-wifi
#
################################################################################

TB3_WIFI_LICENSE = MIT
TB3_WIFI_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/tb3-wifi
TB3_WIFI_SITE_METHOD = local
# Installed after wpa_supplicant so that its configuration replaces the stock one.
TB3_WIFI_DEPENDENCIES = wpa_supplicant

define TB3_WIFI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/S40wifi $(TARGET_DIR)/etc/init.d/S40wifi
	$(INSTALL) -D -m 0644 $(@D)/wpa_supplicant.conf $(TARGET_DIR)/etc/wpa_supplicant.conf
endef

$(eval $(generic-package))
