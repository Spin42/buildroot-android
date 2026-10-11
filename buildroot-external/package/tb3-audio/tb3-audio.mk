################################################################################
#
# tb3-audio
#
################################################################################

TB3_AUDIO_LICENSE = MIT
TB3_AUDIO_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/tb3-audio
TB3_AUDIO_SITE_METHOD = local
TB3_AUDIO_DEPENDENCIES = alsa-utils

define TB3_AUDIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/S30alsa $(TARGET_DIR)/etc/init.d/S30alsa
	$(INSTALL) -D -m 0644 $(@D)/asound.state $(TARGET_DIR)/var/lib/alsa/asound.state
endef

$(eval $(generic-package))
