#!/bin/sh
#
# Post-image script of the RAUC configuration, run after post-image-uboot.sh
# (which builds u-boot-fp2.img and the userdata image):
#   update.raucb - RAUC bundle of the root filesystem, signed with the
#                  development key

set -e

KEY_DIR="${RAUC_KEY_DIR:-${BASE_DIR}/rauc-keys}"
BUNDLE_DIR="${BUILD_DIR}/fp2-rauc-bundle"
VERSION="$(date -u +%Y%m%d%H%M%S)"

rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_DIR}"
cp "${BINARIES_DIR}/rootfs.ext4" "${BUNDLE_DIR}/rootfs.ext4"
cat > "${BUNDLE_DIR}/manifest.raucm" <<EOF
[update]
compatible=fairphone2-buildroot
version=${VERSION}

[bundle]
format=verity

[image.rootfs]
filename=rootfs.ext4
EOF

rm -f "${BINARIES_DIR}/update.raucb"
"${HOST_DIR}/bin/rauc" bundle \
	--cert="${KEY_DIR}/cert.pem" \
	--key="${KEY_DIR}/key.pem" \
	"${BUNDLE_DIR}" "${BINARIES_DIR}/update.raucb"
echo "RAUC bundle ${VERSION} created at ${BINARIES_DIR}/update.raucb"
