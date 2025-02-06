################################################################################
#
# Rmtfs
#
################################################################################

RMTFS_VERSION = 33e1e40615efc59b17a515afe857c51b8b8c1ad1
RMTFS_SITE = $(call github,linux-msm,rmtfs,$(RMTFS_VERSION))
RTMFS_DEPENDENCIES = qrtr libqrtr libudev

define RMTFS_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
	CC="$(TARGET_CC)" \
        CFLAGS="$(TARGET_CFLAGS) -I$(TARGET_DIR)/usr/include" \
        LDFLAGS="$(TARGET_LDFLAGS) -L$(TARGET_DIR)/usr/lib -lqrtr -ludev"
endef

define RMTFS_INSTALL_TARGET_CMDS
    $(INSTALL) -D $(@D)/rmtfs $(TARGET_DIR)/usr/bin/rmtfs
    $(INSTALL) -D -m744 $(BR2_EXTERNAL)/package/rmtfs/S20rmtfs $(TARGET_DIR)/etc/init.d/S20rmtfs
    $(INSTALL) -D -m644 $(BR2_EXTERNAL)/package/rmtfs/modem.rules $(TARGET_DIR)/usr/lib/udev/rules.d/55-modem.rules
    $(INSTALL) -D -m644 $(BR2_EXTERNAL)/package/rmtfs/rmtfs.rules $(TARGET_DIR)/usr/lib/udev/rules.d/65-rmtfs.rules
endef

$(eval $(generic-package))
