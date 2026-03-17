################################################################################
#
# tqftpserv
#
################################################################################

TQFTPSERV_VERSION = v1.1.1
TQFTPSERV_SITE = $(call github,linux-msm,tqftpserv,$(TQFTPSERV_VERSION))
TQFTPSERV_DEPENDENCIES = qrtr zstd
TQFTPSERV_LICENSE = BSD-3-Clause

define TQFTPSERV_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 744 $(BR2_EXTERNAL_ANDROID_PATH)/package/tqftpserv/S31tqftpserv \
		$(TARGET_DIR)/etc/init.d/S31tqftpserv
endef

$(eval $(meson-package))
