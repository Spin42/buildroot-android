#!/bin/sh
# Post-image for the persistent system: check that rootfs.ext2 is an ext2 filesystem lk2nd can
# read, then make the sparse image that "fastboot flash userdata" sends.
#
# lk2nd's LK ext2 driver (lib/fs/ext2/ext2.c) refuses any ro_compat feature other than
# sparse_super and large_file, and reads files through block maps only (no extents, no flex_bg,
# no 64bit group descriptors). The kernel mounts the same filesystem as ext2 through the ext4
# driver (EXT4_USE_FOR_EXT2).
set -e

RAW="${BINARIES_DIR}/rootfs.ext2"
SPARSE="${BINARIES_DIR}/userdata.simg"
DUMPE2FS="${HOST_DIR}/sbin/dumpe2fs"
DEBUGFS="${HOST_DIR}/sbin/debugfs"
E2FSCK="${HOST_DIR}/sbin/e2fsck"
# userdata (mmcblk0p26): 24536415 sectors of 512 bytes
PART_SIZE=$((24536415 * 512))

fail() { echo "create_userdata_image: $*" >&2; exit 1; }

header=$("${DUMPE2FS}" -h "${RAW}" 2>/dev/null)
echo "${header}" | grep -q '^Filesystem revision #: *1 (dynamic)' || fail "not an ext2 rev 1 filesystem"
features=$(echo "${header}" | sed -n 's/^Filesystem features: *//p')
echo "create_userdata_image: features: ${features}"
allowed="ext_attr resize_inode dir_index filetype sparse_super large_file"
for f in ${features}; do
	case " ${allowed} " in
	*" ${f} "*) ;;
	*) fail "feature '${f}' is not readable by lk2nd's ext2 driver" ;;
	esac
done

"${E2FSCK}" -fn "${RAW}" > /dev/null || fail "e2fsck -fn reports errors"
for f in /extlinux/extlinux.conf /boot/zImage /boot/qcom-msm8226-samsung-s3ve3g.dtb /sbin/init; do
	"${DEBUGFS}" -R "stat ${f}" "${RAW}" 2>/dev/null | grep -q '^Inode:' || fail "${f} missing from the image"
done

size=$(stat -c %s "${RAW}")
[ "${size}" -le "${PART_SIZE}" ] || fail "rootfs.ext2 (${size}) is larger than userdata (${PART_SIZE})"

IMG2SIMG="${HOST_DIR}/bin/img2simg"
[ -x "${IMG2SIMG}" ] || IMG2SIMG=$(command -v img2simg) || fail "img2simg not found (install android-tools)"
"${IMG2SIMG}" "${RAW}" "${SPARSE}"

(cd "${BINARIES_DIR}" && sha256sum rootfs.ext2 userdata.simg > SHA256SUMS)
echo "create_userdata_image: ${RAW} (${size} bytes), ${SPARSE} ($(stat -c %s "${SPARSE}") bytes)"
