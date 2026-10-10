################################################################################
#
# mt8127-wmt
#
################################################################################

MT8127_WMT_VERSION = $(MT8127_CONNECTIVITY_VERSION)
MT8127_WMT_SITE = $(MT8127_CONNECTIVITY_SITE)
MT8127_WMT_SITE_METHOD = $(MT8127_CONNECTIVITY_SITE_METHOD)
MT8127_WMT_LICENSE = $(MT8127_CONNECTIVITY_LICENSE)
MT8127_WMT_MODULE_SUBDIRS = wmt
MT8127_WMT_DEPENDENCIES = mt8127-btif mt8127-consys
MT8127_WMT_MODULE_MAKE_OPTS = KBUILD_EXTRA_SYMBOLS="$(MT8127_BTIF_BUILDDIR)/btif/Module.symvers $(MT8127_CONSYS_BUILDDIR)/consys/Module.symvers"

$(eval $(kernel-module))
$(eval $(generic-package))
