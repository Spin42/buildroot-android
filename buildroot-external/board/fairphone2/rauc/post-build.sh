#!/bin/sh
#
# Post-build script of the RAUC configuration:
# - creates the development signing key on first use and installs its
#   certificate as the RAUC keyring,
# - builds the initramfs into /boot of the root filesystem, so that it is
#   updated together with the kernel.

set -e

RAUC_DIR="$(dirname "$0")"
KEY_DIR="${RAUC_KEY_DIR:-${BASE_DIR}/rauc-keys}"

if [ ! -f "${KEY_DIR}/key.pem" ]; then
	mkdir -p "${KEY_DIR}"
	# Valid from the epoch: the phone clock may not be set when a bundle is installed
	"${HOST_DIR}/bin/openssl" req -x509 -newkey rsa:4096 -nodes \
		-not_before 19700101000000Z -not_after 20991231235959Z \
		-subj "/O=Fairphone 2 development/CN=rauc" \
		-keyout "${KEY_DIR}/key.pem" -out "${KEY_DIR}/cert.pem"
	echo "Generated a RAUC development key in ${KEY_DIR}"
fi
install -D -m 0644 "${KEY_DIR}/cert.pem" "${TARGET_DIR}/etc/rauc/keyring.pem"
mkdir -p "${TARGET_DIR}/data"

# Initramfs: busybox from the target, with its libraries
INITRAMFS="${BUILD_DIR}/fp2-rauc-initramfs"
READELF=$(ls "${HOST_DIR}"/bin/*-readelf | head -n 1)
rm -rf "${INITRAMFS}"
mkdir -p "${INITRAMFS}"/bin "${INITRAMFS}"/dev "${INITRAMFS}"/proc \
	"${INITRAMFS}"/sys "${INITRAMFS}"/lib

install -m 0755 "${RAUC_DIR}/initramfs-init" "${INITRAMFS}/init"
install -m 0755 "${TARGET_DIR}/bin/busybox" "${INITRAMFS}/bin/busybox"
for applet in sh mount umount mkdir grep sed seq sleep losetup switch_root reboot; do
	ln -sf busybox "${INITRAMFS}/bin/${applet}"
done

copy_libs() {
	for lib in $("${READELF}" -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		[ -e "${INITRAMFS}/lib/${lib}" ] && continue
		src=$(find "${TARGET_DIR}/lib" "${TARGET_DIR}/usr/lib" -maxdepth 1 -name "${lib}" | head -n 1)
		[ -n "${src}" ] || { echo "missing ${lib}" >&2; exit 1; }
		cp -L "${src}" "${INITRAMFS}/lib/${lib}"
		copy_libs "${INITRAMFS}/lib/${lib}"
	done
}
copy_libs "${INITRAMFS}/bin/busybox"
interp=$("${READELF}" -l "${INITRAMFS}/bin/busybox" | sed -n 's/.*interpreter: \(.*\)\]/\1/p')
cp -L "${TARGET_DIR}${interp}" "${INITRAMFS}${interp}"

(cd "${INITRAMFS}" && find . | cpio -o -H newc --owner=0:0 --quiet) | gzip -9 \
	> "${TARGET_DIR}/boot/initramfs.cpio.gz"
