################################################################################
#
# mt8127-bt
#
################################################################################

MT8127_BT_SITE = $(MT8127_CONNECTIVITY_SITE)
MT8127_BT_SITE_METHOD = $(MT8127_CONNECTIVITY_SITE_METHOD)
MT8127_BT_LICENSE = $(MT8127_CONNECTIVITY_LICENSE)
MT8127_BT_MODULE_SUBDIRS = bt
MT8127_BT_DEPENDENCIES = mt8127-wmt
MT8127_BT_MODULE_MAKE_OPTS = KBUILD_EXTRA_SYMBOLS="$(MT8127_WMT_BUILDDIR)/wmt/Module.symvers"

$(eval $(kernel-module))
$(eval $(generic-package))
