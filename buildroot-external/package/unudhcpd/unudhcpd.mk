################################################################################
#
# Unudhcpd
#
################################################################################

UNUDHCPD_VERSION = main
UNUDHCPD_SITE = https://gitlab.postmarketos.org/postmarketOS/unudhcpd.git
UNUDHCPD_SITE_METHOD = git

$(eval $(meson-package))
