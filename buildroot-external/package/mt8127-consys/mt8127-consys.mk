################################################################################
#
# mt8127-consys
#
################################################################################

MT8127_CONSYS_VERSION = $(MT8127_CONNECTIVITY_VERSION)
MT8127_CONSYS_SITE = $(MT8127_CONNECTIVITY_SITE)
MT8127_CONSYS_SITE_METHOD = $(MT8127_CONNECTIVITY_SITE_METHOD)
MT8127_CONSYS_LICENSE = $(MT8127_CONNECTIVITY_LICENSE)
MT8127_CONSYS_MODULE_SUBDIRS = consys

$(eval $(kernel-module))
$(eval $(generic-package))
