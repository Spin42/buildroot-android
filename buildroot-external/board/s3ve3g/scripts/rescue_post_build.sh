#!/bin/sh
# Post-build for the rescue initramfs:
# - keep only the kernel modules listed in board/s3ve3g/rescue-modules.txt (and their
#   dependencies), uncompressed so the xz-compressed cpio can compress them together;
# - install root's authorized_keys and a fixed dropbear host key from the build host.
#
# SSH material comes from $S3VE3G_RESCUE_SSH_DIR (default: <output dir>/../rescue-ssh):
#   authorized keys: $S3VE3G_RESCUE_AUTHORIZED_KEYS (default: ~/.ssh/*.pub); without any, a
#                    dedicated pair id_ed25519{,.pub} is generated there
#   host key:        ssh_host_ed25519_key, generated once; known_hosts there has its entry
set -e

BOARD_DIR="${BR2_EXTERNAL_ANDROID_PATH}/board/s3ve3g"

# --- kernel modules -----------------------------------------------------------------------------
KVER=$(ls "${TARGET_DIR}/lib/modules")
MODDIR="${TARGET_DIR}/lib/modules/${KVER}"
# The host depmod is built without zstd: decompress first so it can read every module.
find "${MODDIR}" -name '*.ko.zst' -exec "${HOST_DIR}/bin/zstd" -q -d --rm {} +
"${HOST_DIR}/sbin/depmod" -a -b "${TARGET_DIR}" "${KVER}"

keep="${BUILD_DIR}/rescue-modules.keep"
: > "${keep}"
for m in $(sed -e 's/#.*//' "${BOARD_DIR}/rescue-modules.txt"); do
	line=$(grep -E "(^|/)${m}\.ko:" "${MODDIR}/modules.dep" || true)
	if [ -z "${line}" ]; then
		echo "rescue_post_build: module ${m} is not built (rescue-modules.txt)" >&2
		exit 1
	fi
	echo "${line}" | tr -d ':' | tr ' ' '\n' | grep . >> "${keep}"
done
(cd "${MODDIR}" && find . -name '*.ko' | sed 's|^\./||' | sort) > "${keep}.all"
sort -u "${keep}" | comm -23 "${keep}.all" - | (cd "${MODDIR}" && xargs -r rm -f)
find "${MODDIR}" -type d -empty -delete
"${HOST_DIR}/sbin/depmod" -a -b "${TARGET_DIR}" "${KVER}"
echo "rescue_post_build: kept $(wc -l < "${MODDIR}/modules.dep") of $(wc -l < "${keep}.all") modules"

# Firmware search path of the kernel loader; filled in by later milestones.
mkdir -p "${TARGET_DIR}/lib/firmware"

# --- ssh ----------------------------------------------------------------------------------------
SSH_DIR="${S3VE3G_RESCUE_SSH_DIR:-$(dirname "${BASE_DIR}")/rescue-ssh}"
mkdir -p "${SSH_DIR}"
chmod 700 "${SSH_DIR}"

keys=$(ls ${S3VE3G_RESCUE_AUTHORIZED_KEYS:-${HOME}/.ssh/*.pub} 2>/dev/null || true)
if [ -z "${keys}" ]; then
	[ -f "${SSH_DIR}/id_ed25519" ] || \
		ssh-keygen -q -t ed25519 -N '' -C s3ve3g-rescue -f "${SSH_DIR}/id_ed25519"
	keys="${SSH_DIR}/id_ed25519.pub"
fi
install -d -m 700 "${TARGET_DIR}/root/.ssh"
cat ${keys} > "${TARGET_DIR}/root/.ssh/authorized_keys"
chmod 600 "${TARGET_DIR}/root/.ssh/authorized_keys"
echo "rescue_post_build: authorized_keys from" ${keys}

[ -f "${SSH_DIR}/ssh_host_ed25519_key" ] || \
	ssh-keygen -q -t ed25519 -N '' -C s3ve3g-rescue-host -f "${SSH_DIR}/ssh_host_ed25519_key"
install -d -m 700 "${TARGET_DIR}/etc/rescue"
python3 -I "${BOARD_DIR}/scripts/dropbear-hostkey.py" \
	"${SSH_DIR}/ssh_host_ed25519_key" "${TARGET_DIR}/etc/rescue/dropbear_ed25519_host_key"
chmod 600 "${TARGET_DIR}/etc/rescue/dropbear_ed25519_host_key"
echo "10.0.42.1 $(cut -d' ' -f1,2 "${SSH_DIR}/ssh_host_ed25519_key.pub")" > "${SSH_DIR}/known_hosts"
