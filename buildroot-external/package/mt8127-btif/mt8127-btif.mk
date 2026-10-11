################################################################################
#
# mt8127-btif
#
################################################################################

MT8127_BTIF_SITE = $(MT8127_CONNECTIVITY_SITE)
MT8127_BTIF_SITE_METHOD = $(MT8127_CONNECTIVITY_SITE_METHOD)
MT8127_BTIF_LICENSE = $(MT8127_CONNECTIVITY_LICENSE)
MT8127_BTIF_MODULE_SUBDIRS = btif

$(eval $(kernel-module))
$(eval $(generic-package))
