#!/bin/sh
set -eu
target_dir=$1
board_dir="${BR2_EXTERNAL_ANDROID_PATH}/board/samsung-expressatt"
kernel_dir=
for candidate in "${BUILD_DIR}"/linux-*; do
    [ -f "$candidate/include/config/kernel.release" ] || continue
    [ -z "$kernel_dir" ] || { echo 'Multiple prepared kernel trees' >&2; exit 1; }
    kernel_dir=$candidate
done
[ -n "$kernel_dir" ] || { echo 'No prepared kernel tree' >&2; exit 1; }
release=$(cat "$kernel_dir/include/config/kernel.release")
module_dir="$target_dir/lib/modules/$release"
rm -rf "$target_dir/lib/modules"
mkdir -p "$module_dir"

# Sign unstripped Kbuild outputs; never strip an already signed module.
while IFS= read -r module_path; do
    destination="$module_dir/kernel/$module_path"
    mkdir -p "$(dirname "$destination")"
    cp "$kernel_dir/$module_path" "$destination"
    "$kernel_dir/scripts/sign-file" sha512 "$kernel_dir/certs/signing_key.pem" \
        "$kernel_dir/certs/signing_key.x509" "$destination"
    printf 'kernel/%s\n' "$module_path"
done < "$board_dir/system-modules.list" > "$module_dir/modules.order"
cp "$kernel_dir/modules.builtin" "$kernel_dir/modules.builtin.modinfo" "$module_dir/"
"${HOST_DIR}/sbin/depmod" -b "$target_dir" "$release"
