################################################################################
#
# s3ve3g-firmware
#
################################################################################

S3VE3G_FIRMWARE_VERSION = 702c16ad3616ecb61cd64a31aa56f37495f4e192
S3VE3G_FIRMWARE_SITE = https://raw.githubusercontent.com/LineageOS/android_device_samsung_msm8226-common/$(S3VE3G_FIRMWARE_VERSION)/prima
S3VE3G_FIRMWARE_SOURCE = WCNSS_qcom_wlan_nv.bin
S3VE3G_FIRMWARE_LICENSE = proprietary

# A single file, not an archive.
define S3VE3G_FIRMWARE_EXTRACT_CMDS
	cp $(S3VE3G_FIRMWARE_DL_DIR)/$(S3VE3G_FIRMWARE_SOURCE) $(@D)/
endef

define S3VE3G_FIRMWARE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/WCNSS_qcom_wlan_nv.bin \
		$(TARGET_DIR)/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin
endef

$(eval $(generic-package))
