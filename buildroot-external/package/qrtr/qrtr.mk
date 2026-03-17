################################################################################
#
# Qrtr
#
################################################################################

QRTR_VERSION = ef44ad10f284410e2db4c4ce22c8645f988f8402
QRTR_SITE = $(call github,linux-msm,qrtr,$(QRTR_VERSION))
QRTR_INSTALL_STAGING = YES
QRTR_LICENSE = BSD-3-Clause

$(eval $(meson-package))
