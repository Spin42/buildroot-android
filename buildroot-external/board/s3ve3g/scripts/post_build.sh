#!/bin/sh
# Post-build for the persistent system (s3ve3g_defconfig):
# - every kernel module, uncompressed (the host depmod is built without zstd);
# - root's authorized_keys and the rescue image's fixed dropbear host key, so one known_hosts
#   line fits both systems;
# - telnetd is started by usr/sbin/usb-gadget.sh, bound to the USB address, not by S50telnet;
#   unudhcpd likewise, so its OpenRC service file (unused under BusyBox init) goes too.
#
# SSH material: the same sources and defaults as scripts/rescue_post_build.sh
# ($S3VE3G_RESCUE_SSH_DIR, default <output dir>/../rescue-ssh; $S3VE3G_RESCUE_AUTHORIZED_KEYS,
# default ~/.ssh/*.pub).
set -e

BOARD_DIR="${BR2_EXTERNAL_ANDROID_PATH}/board/s3ve3g"

# --- kernel modules -----------------------------------------------------------------------------
KVER=$(ls "${TARGET_DIR}/lib/modules")
MODDIR="${TARGET_DIR}/lib/modules/${KVER}"
find "${MODDIR}" -name '*.ko.zst' -exec "${HOST_DIR}/bin/zstd" -q -d --rm {} +
"${HOST_DIR}/sbin/depmod" -a -b "${TARGET_DIR}" "${KVER}"
echo "post_build: $(wc -l < "${MODDIR}/modules.dep") modules in /lib/modules/${KVER}"
mkdir -p "${TARGET_DIR}/lib/firmware" "${TARGET_DIR}/etc/s3ve3g"

# --- kernel and device tree for lk2nd's extlinux ------------------------------------------------
for f in boot/zImage boot/qcom-msm8226-samsung-s3ve3g.dtb extlinux/extlinux.conf; do
	[ -f "${TARGET_DIR}/${f}" ] || { echo "post_build: /${f} missing from the target" >&2; exit 1; }
done

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
echo "post_build: authorized_keys from" ${keys}

[ -f "${SSH_DIR}/ssh_host_ed25519_key" ] || \
	ssh-keygen -q -t ed25519 -N '' -C s3ve3g-rescue-host -f "${SSH_DIR}/ssh_host_ed25519_key"
# The dropbear package links /etc/dropbear to /var/run/dropbear (a tmpfs); this root is writable.
[ -L "${TARGET_DIR}/etc/dropbear" ] && rm -f "${TARGET_DIR}/etc/dropbear"
install -d -m 700 "${TARGET_DIR}/etc/dropbear"
python3 -I "${BOARD_DIR}/scripts/dropbear-hostkey.py" \
	"${SSH_DIR}/ssh_host_ed25519_key" "${TARGET_DIR}/etc/dropbear/dropbear_ed25519_host_key"
chmod 600 "${TARGET_DIR}/etc/dropbear/dropbear_ed25519_host_key"
echo "10.0.42.1 $(cut -d' ' -f1,2 "${SSH_DIR}/ssh_host_ed25519_key.pub")" > "${SSH_DIR}/known_hosts"

rm -f "${TARGET_DIR}/etc/init.d/S50telnet" "${TARGET_DIR}/etc/init.d/unudhcpd"
