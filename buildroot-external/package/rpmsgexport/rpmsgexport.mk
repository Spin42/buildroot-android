################################################################################
#
# Rpmsgexport
#
################################################################################

RPMSGEXPORT_VERSION = 324d88d668f36c6a5e6a9c2003a050b8a5a3cd60
RPMSGEXPORT_SITE = $(call github,andersson,rpmsgexport,$(RPMSGEXPORT_VERSION))

define RPMSGEXPORT_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)"
endef

define RPMSGEXPORT_INSTALL_TARGET_CMDS
    $(INSTALL) -D $(@D)/rpmsgexport $(TARGET_DIR)/usr/sbin/rpmsgexport
endef

$(eval $(generic-package))
