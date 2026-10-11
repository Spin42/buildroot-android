# Source of the out-of-tree MT8127 connectivity drivers: package/mt8127-connectivity/src, with one
# directory per component; each component is its own package (mt8127-btif, -consys, -wmt, -wlan,
# -bt, -fm) and builds only its directory (MODULE_SUBDIRS).
MT8127_CONNECTIVITY_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/mt8127-connectivity/src
MT8127_CONNECTIVITY_SITE_METHOD = local
MT8127_CONNECTIVITY_LICENSE = GPL-2.0

include $(sort $(wildcard $(BR2_EXTERNAL_ANDROID_PATH)/package/*/*.mk))
