#!/bin/sh
set -eu

# The first-light initramfs uses only built-in drivers. Build the broad base
# config's modules to keep their code out of zImage, but do not spend BOOT
# partition space shipping modules that cannot help the initial boot test.
target_dir=$1
rm -rf "${target_dir}/lib/modules"
