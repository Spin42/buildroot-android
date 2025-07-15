################################################################################
#
# Grate firmware
#
################################################################################

GRATE_FIRMWARE_VERSION = master
GRATE_FIRMWARE_SITE = https://gitlab.com/grate-driver/firmware.git
GRATE_FIRMWARE_SITE_METHOD = git

define GRATE_FIRMWARE_INSTALL_TARGET_CMDS
	$(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
