################################################################################
#
# adsp-boot
#
################################################################################

ADSP_BOOT_VERSION = 1.0
ADSP_BOOT_SITE_METHOD = local
ADSP_BOOT_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/adsp-boot
ADSP_BOOT_LICENSE = GPL-3.0-or-later

define ADSP_BOOT_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 744 $(BR2_EXTERNAL_ANDROID_PATH)/package/adsp-boot/S35adsp-boot \
		$(TARGET_DIR)/etc/init.d/S35adsp-boot
endef

$(eval $(generic-package))
