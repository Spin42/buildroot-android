################################################################################
#
# Custom recipe for alsa-ucm-conf
#
################################################################################

ALSA_UCM_CONF_VERSION = master
ALSA_UCM_CONF_SITE = https://github.com/alsa-project/alsa-ucm-conf.git
ALSA_UCM_CONF_SITE_METHOD = git
ALSA_UCM_CONF_INSTALL_TARGET = YES

define ALSA_UCM_CONF_INSTALL_TARGET_CMDS
	cp -r $(@D)/ucm $(TARGET_DIR)/usr/share/alsa/
	cp -r $(@D)/ucm2 $(TARGET_DIR)/usr/share/alsa/
endef

$(eval $(generic-package))
