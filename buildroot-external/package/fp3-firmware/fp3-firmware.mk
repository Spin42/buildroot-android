################################################################################
#
# FP3 Firmware
#
################################################################################

FP3_FIRMWARE_VERSION = ee529820bb874ab1b52873f0b144c3786f01ee4c
FP3_FIRMWARE_SITE = $(call github,FairBlobs,FP3-firmware,$(FP3_FIRMWARE_VERSION))
FP3_FIRMWARE_LICENSE = proprietary

define FP3_FIRMWARE_INSTALL_TARGET_CMDS
	mkdir -p $(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3
	mkdir -p $(TARGET_DIR)/lib/firmware/wlan/prima
	mkdir -p $(TARGET_DIR)/lib/firmware/qcom
	# Modem (MPSS + MBA)
	$(INSTALL) -D -m 0644 $(@D)/mba.mbn $(TARGET_DIR)/lib/firmware/mba.mbn
	for f in $(@D)/modem.mdt $(@D)/modem.b*; do \
		$(INSTALL) -D -m 0644 $$f $(TARGET_DIR)/lib/firmware/$$(basename $$f); \
	done
	# ADSP
	for f in $(@D)/adsp.mdt $(@D)/adsp.b*; do \
		$(INSTALL) -D -m 0644 $$f $(TARGET_DIR)/lib/firmware/$$(basename $$f); \
	done
	# WCNSS (WiFi/BT remoteproc)
	for f in $(@D)/wcnss.mdt $(@D)/wcnss.b*; do \
		$(INSTALL) -D -m 0644 $$f $(TARGET_DIR)/lib/firmware/$$(basename $$f); \
	done
	# Venus (video encoder/decoder)
	for f in $(@D)/venus.mdt $(@D)/venus.b*; do \
		$(INSTALL) -D -m 0644 $$f $(TARGET_DIR)/lib/firmware/$$(basename $$f); \
	done
	# GPU Adreno 506 PM4/PFP firmware (driver looks in qcom/ first)
	$(INSTALL) -D -m 0644 $(@D)/a530_pfp.fw $(TARGET_DIR)/lib/firmware/qcom/a530_pfp.fw
	$(INSTALL) -D -m 0644 $(@D)/a530_pm4.fw $(TARGET_DIR)/lib/firmware/qcom/a530_pm4.fw
	# GPU Adreno 506 zap shader (FP3-specific DTS path)
	for f in $(@D)/a506_zap.mdt $(@D)/a506_zap.b* $(@D)/a506_zap.elf; do \
		$(INSTALL) -D -m 0644 $$f $(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/$$(basename $$f); \
	done
	# WiFi NV data and config
	$(INSTALL) -D -m 0644 $(@D)/wlan/prima/WCNSS_qcom_wlan_nv.bin $(TARGET_DIR)/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin
	$(INSTALL) -D -m 0644 $(@D)/wlan/prima/WCNSS_qcom_cfg.ini $(TARGET_DIR)/lib/firmware/wlan/prima/WCNSS_qcom_cfg.ini
	# Modem config files
	cp -a $(@D)/modem_pr $(TARGET_DIR)/lib/firmware/modem_pr
	# DTS firmware-name symlinks (DTS expects qcom/msm8953/fairphone/fp3/<fw>.mbn)
	# ADSP: adsp.mdt + adsp.b00-b14
	for f in $(TARGET_DIR)/lib/firmware/adsp.mdt $(TARGET_DIR)/lib/firmware/adsp.b*; do \
		ln -sf ../../../../$$(basename $$f) \
			$(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/$$(basename $$f); \
	done
	# MBA
	ln -sf ../../../../mba.mbn \
		$(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/mba.mbn
	# WCNSS: wcnss.mdt + wcnss.b*
	for f in $(TARGET_DIR)/lib/firmware/wcnss.mdt $(TARGET_DIR)/lib/firmware/wcnss.b*; do \
		ln -sf ../../../../$$(basename $$f) \
			$(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/$$(basename $$f); \
	done
	# WCNSS NV bin
	ln -sf ../../../../wlan/prima/WCNSS_qcom_wlan_nv.bin \
		$(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/WCNSS_qcom_wlan_nv.bin
	# Modem (MPSS): modem.mdt + modem.b*
	for f in $(TARGET_DIR)/lib/firmware/modem.mdt $(TARGET_DIR)/lib/firmware/modem.b*; do \
		ln -sf ../../../../$$(basename $$f) \
			$(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/$$(basename $$f); \
	done
	# .mbn aliases (DTS firmware-name uses .mbn extension, actual files are .mdt)
	ln -sf adsp.mdt $(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/adsp.mbn
	ln -sf wcnss.mdt $(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/wcnss.mbn
	ln -sf modem.mdt $(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/modem.mbn
	ln -sf a506_zap.mdt $(TARGET_DIR)/lib/firmware/qcom/msm8953/fairphone/fp3/a506_zap.mbn
	# Sensor registry (sns.reg for in-kernel QCOM_SNS_REG driver)
	$(INSTALL) -D -m 644 $(BR2_EXTERNAL_ANDROID_PATH)/package/fp3-firmware/sns.reg \
		$(TARGET_DIR)/lib/firmware/qcom/sensors/sns.reg
endef

$(eval $(generic-package))
