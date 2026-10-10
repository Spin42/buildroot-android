# Source of the out-of-tree MT8127 connectivity drivers: one repository with one directory per
# component; each component is its own package (mt8127-btif, -consys, -wmt, -wlan) and builds
# only its directory (MODULE_SUBDIRS).
MT8127_CONNECTIVITY_VERSION = main
MT8127_CONNECTIVITY_SITE = https://github.com/mlainez/mt8127-consys.git
MT8127_CONNECTIVITY_SITE_METHOD = git
MT8127_CONNECTIVITY_LICENSE = GPL-2.0

include $(sort $(wildcard $(BR2_EXTERNAL_ANDROID_PATH)/package/*/*.mk))
