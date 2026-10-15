#!/bin/sh
set -eu
image="${BINARIES_DIR}/rootfs.ext2"
image_bytes=$(wc -c < "$image")
if [ "$image_bytes" -ne 268435456 ] || [ "$image_bytes" -gt 5498732544 ]; then
    echo "unexpected userdata image size: ${image_bytes}" >&2
    exit 1
fi
"${HOST_DIR}/sbin/dumpe2fs" -h "$image" > "${BINARIES_DIR}/rootfs-ext2-info.txt"
features=$(sed -n 's/^Filesystem features: *//p' "${BINARIES_DIR}/rootfs-ext2-info.txt")
for feature in $features; do
    case "$feature" in
        filetype|sparse_super|large_file) ;;
        *) echo "unsupported lk2nd filesystem feature: $feature" >&2; exit 1 ;;
    esac
done
"${HOST_DIR}/sbin/e2fsck" -fn "$image"
echo "persistent userdata filesystem: ${image} (${image_bytes}/5498732544 bytes)"
