################################################################################
#
# FP2 Firmware
#
################################################################################

FP2_FIRMWARE_VERSION = 19.02.1
FP2_FIRMWARE_SITE = $(call github,FairBlobs,FP2-firmware,$(FP2_FIRMWARE_VERSION))
FP2_FIRMWARE_ANDROID = android_device_fairphone_FP2
FP2_FIRMWARE_ANDROID_COMMIT = f38e10cd6a2cd55310a5090200db0221fdef53a1
FP2_FIRMWARE_EXTRA_DOWNLOADS = $(call github,LineageOS,$(FP2_FIRMWARE_ANDROID),$(FP2_FIRMWARE_ANDROID_COMMIT)).tar.gz
FP2_FIRMWARE_LICENSE = proprietary

define FP2_FIRMWARE_BUILD_CMDS
    tar -xzf $(FP2_FIRMWARE_DL_DIR)/$(FP2_FIRMWARE_ANDROID_COMMIT).tar.gz -C $(BUILD_DIR)/
endef

define FP2_FIRMWARE_INSTALL_TARGET_CMDS
    mkdir -p $(TARGET_DIR)/lib/firmware/wlan/prima
    $(INSTALL) -D -m 0755 $(@D)/* $(TARGET_DIR)/lib/firmware
    $(INSTALL) -D -m 0755 $(BUILD_DIR)/$(FP2_FIRMWARE_ANDROID)-$(FP2_FIRMWARE_ANDROID_COMMIT)/wifi/WCNSS* $(TARGET_DIR)/lib/firmware/wlan/prima/
endef

$(eval $(generic-package))
