################################################################################
#
# mt8127-wlan
#
################################################################################

MT8127_WLAN_SITE = $(MT8127_CONNECTIVITY_SITE)
MT8127_WLAN_SITE_METHOD = $(MT8127_CONNECTIVITY_SITE_METHOD)
MT8127_WLAN_LICENSE = $(MT8127_CONNECTIVITY_LICENSE)
MT8127_WLAN_MODULE_SUBDIRS = wlan
MT8127_WLAN_DEPENDENCIES = mt8127-wmt mt8127-consys
MT8127_WLAN_MODULE_MAKE_OPTS = KBUILD_EXTRA_SYMBOLS="$(MT8127_WMT_BUILDDIR)/wmt/Module.symvers $(MT8127_CONSYS_BUILDDIR)/consys/Module.symvers"

$(eval $(kernel-module))
$(eval $(generic-package))
