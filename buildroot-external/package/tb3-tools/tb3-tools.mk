################################################################################
#
# tb3-tools
#
################################################################################

TB3_TOOLS_LICENSE = MIT
TB3_TOOLS_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/tb3-tools
TB3_TOOLS_SITE_METHOD = local

define TB3_TOOLS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(@D)/tb3-pmic.c -o $(@D)/tb3-pmic
endef

define TB3_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tb3-pmic $(TARGET_DIR)/usr/sbin/tb3-pmic
	printf '#!/bin/sh\nexec /usr/sbin/tb3-pmic reboot-mode "$$@"\n' > $(TARGET_DIR)/usr/sbin/reboot-mode
	chmod 755 $(TARGET_DIR)/usr/sbin/reboot-mode
endef

$(eval $(generic-package))
