#!/bin/sh
# First light runs from an initramfs and loads no modules: drop them so the boot image stays small.
# Modules stay enabled in the kernel: with CONFIG_MODULES=n Kconfig would turn every "=m" into a
# built-in, including DRM_MSM, which would take the display away from simpledrm.
set -e
rm -rf "${TARGET_DIR}/lib/modules"
