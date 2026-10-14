################################################################################
#
# Unudhcpd
#
################################################################################

UNUDHCPD_VERSION = 0.5.0
UNUDHCPD_SITE = https://gitlab.postmarketos.org/postmarketOS/unudhcpd.git
UNUDHCPD_SITE_METHOD = git

$(eval $(meson-package))
