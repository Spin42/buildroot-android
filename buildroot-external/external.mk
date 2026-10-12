# Source of the out-of-tree MT8127 connectivity drivers: package/mt8127-connectivity/src, with one
# directory per component; each component is its own package (mt8127-btif, -consys, -wmt, -wlan,
# -bt, -fm) and builds only its directory (MODULE_SUBDIRS).
MT8127_CONNECTIVITY_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/mt8127-connectivity/src
MT8127_CONNECTIVITY_SITE_METHOD = local
MT8127_CONNECTIVITY_LICENSE = GPL-2.0

# Buildroot 2025.05 copies COPYING to Licenses/gpl-2.0.txt after extracting U-Boot. Since U-Boot
# 2026.10 COPYING is a symlink to that file and the copy fails ("are the same file").
define UBOOT_COPY_OLD_LICENSE_FILE
	if [ -f $(@D)/COPYING ] && [ ! -L $(@D)/COPYING ]; then \
		$(INSTALL) -m 0644 -D $(@D)/COPYING $(@D)/Licenses/gpl-2.0.txt; \
	fi
endef

include $(sort $(wildcard $(BR2_EXTERNAL_ANDROID_PATH)/package/*/*.mk))
