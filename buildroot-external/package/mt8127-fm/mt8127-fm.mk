################################################################################
#
# mt8127-fm
#
################################################################################

MT8127_FM_VERSION = $(MT8127_CONNECTIVITY_VERSION)
MT8127_FM_SITE = $(MT8127_CONNECTIVITY_SITE)
MT8127_FM_SITE_METHOD = $(MT8127_CONNECTIVITY_SITE_METHOD)
MT8127_FM_LICENSE = $(MT8127_CONNECTIVITY_LICENSE)
MT8127_FM_MODULE_SUBDIRS = fm
MT8127_FM_DEPENDENCIES = mt8127-wmt mt8127-consys
MT8127_FM_MODULE_MAKE_OPTS = KBUILD_EXTRA_SYMBOLS="$(MT8127_WMT_BUILDDIR)/wmt/Module.symvers $(MT8127_CONSYS_BUILDDIR)/consys/Module.symvers"

define MT8127_FM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(@D)/tools/fm-radio.c -o $(@D)/tools/fm-radio
endef

define MT8127_FM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tools/fm-radio $(TARGET_DIR)/usr/bin/fm-radio
endef

$(eval $(kernel-module))
$(eval $(generic-package))
