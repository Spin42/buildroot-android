################################################################################
#
# Sim selector
#
################################################################################

define SIM_SELECTOR_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0744 $(BR2_EXTERNAL)/package/sim-selector/S39sim-selector $(TARGET_DIR)/etc/init.d
endef

$(eval $(generic-package))
