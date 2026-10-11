################################################################################
#
# tb3-gpu-test
#
################################################################################

TB3_GPU_TEST_LICENSE = MIT
TB3_GPU_TEST_SITE = $(BR2_EXTERNAL_ANDROID_PATH)/package/tb3-gpu-test
TB3_GPU_TEST_SITE_METHOD = local
TB3_GPU_TEST_DEPENDENCIES = mesa3d

define TB3_GPU_TEST_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) $(@D)/tb3-gpu-test.c -o $(@D)/tb3-gpu-test \
		-lgbm -lEGL -lGLESv2
endef

define TB3_GPU_TEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tb3-gpu-test $(TARGET_DIR)/usr/bin/tb3-gpu-test
endef

$(eval $(generic-package))
