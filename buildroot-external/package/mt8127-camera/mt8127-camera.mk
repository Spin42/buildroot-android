################################################################################
#
# mt8127-camera
#
################################################################################

MT8127_CAMERA_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/mt8127-camera/src
MT8127_CAMERA_SITE_METHOD = local
MT8127_CAMERA_LICENSE = GPL-2.0
MT8127_CAMERA_MODULE_SUBDIRS = sensors cam

$(eval $(kernel-module))
$(eval $(generic-package))
