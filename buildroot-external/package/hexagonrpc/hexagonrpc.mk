################################################################################
#
# hexagonrpc
#
################################################################################

HEXAGONRPC_VERSION = v0.4.0
HEXAGONRPC_SITE = $(call github,linux-msm,hexagonrpc,$(HEXAGONRPC_VERSION))
HEXAGONRPC_LICENSE = GPL-3.0
HEXAGONRPC_INSTALL_STAGING = YES

define HEXAGONRPC_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 744 $(BR2_EXTERNAL_ANDROID_PATH)/package/hexagonrpc/S28hexagonfs \
		$(TARGET_DIR)/etc/init.d/S28hexagonfs
	$(INSTALL) -D -m 744 $(BR2_EXTERNAL_ANDROID_PATH)/package/hexagonrpc/S32hexagonrpcd \
		$(TARGET_DIR)/etc/init.d/S32hexagonrpcd
endef

HEXAGONRPC_CONF_OPTS = -Dhexagonrpcd_verbose=true

$(eval $(meson-package))
