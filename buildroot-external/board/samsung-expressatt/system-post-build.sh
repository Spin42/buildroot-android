#!/bin/sh
set -eu
target_dir=$1
board_dir="${BR2_EXTERNAL_ANDROID_PATH}/board/samsung-expressatt"
"$board_dir/post-build.sh" "$target_dir"

for obsolete in "$target_dir/init" "$target_dir/etc/init.d/S50dropbear" "$target_dir/etc/init.d/S50telnet"; do
    if [ -e "$obsolete" ] || [ -L "$obsolete" ]; then
        rm -- "$obsolete"
    fi
done

if [ -L "$target_dir/etc/dropbear" ]; then
    rm -- "$target_dir/etc/dropbear"
fi
mkdir -p "$target_dir/etc/dropbear"

dtb="$target_dir/boot/qcom-msm8960-samsung-expressatt.dtb"
if ! "${HOST_DIR}/bin/fdtget" -p "$dtb" /reserved-memory >/dev/null 2>&1; then
    "${HOST_DIR}/bin/fdtput" -c "$dtb" /reserved-memory
    for cells in '#address-cells' '#size-cells'; do
        "${HOST_DIR}/bin/fdtput" -t x "$dtb" /reserved-memory "$cells" \
            "$("${HOST_DIR}/bin/fdtget" -t x "$dtb" / "$cells")"
    done
    "${HOST_DIR}/bin/fdtput" "$dtb" /reserved-memory ranges
fi
