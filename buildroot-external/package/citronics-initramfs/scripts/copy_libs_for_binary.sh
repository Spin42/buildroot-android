#!/bin/sh

# Usage:
# ./copy_libs_recursive.sh <binary> <rootfs> <dest>
# Example:
# ./copy_libs_recursive.sh output/target/usr/sbin/parted output/target initramfs-root

BINARY="$1"
ROOT="$2"
DEST="$3"
TOOLCHAIN_READELF="arm-linux-gnueabihf-readelf"

# Tracks already processed libraries to avoid duplication
PROCESSED=""

copy_and_resolve() {
    local file="$1"

    # Normalize path
    file=$(realpath "$file")

    # Print and skip if already done
    echo "$PROCESSED" | grep -qx "$file" && return
    PROCESSED="$PROCESSED
$file"

    # Only copy if inside ROOT
    if echo "$file" | grep -q "^$ROOT"; then
        relpath="${file#$ROOT}"
        target="$DEST$relpath"
        echo "Copying: $file -> $target"
        mkdir -p "$(dirname "$target")"
        cp -aL "$file" "$target"

        # Copy symlinks pointing to this file
        find "$ROOT" -type l 2>/dev/null | while read -r link; do
            resolved=$(realpath -e "$link" 2>/dev/null)
            if [ "$resolved" = "$file" ]; then
                rel_link="${link#$ROOT}"
                dest_link="$DEST$rel_link"
                target_link=$(readlink "$link")

                echo "Copying symlink: $link -> $dest_link (-> $target_link)"
                mkdir -p "$(dirname "$dest_link")"
                ln -snf "$target_link" "$dest_link"
            fi
        done
    else
        echo "Skipping $file (outside ROOT)"
    fi

    # Get DT_NEEDED entries
    needed_libs=$($TOOLCHAIN_READELF -d "$file" 2>/dev/null | grep '(NEEDED)' | awk -F'[][]' '{print $2}')
    for lib in $needed_libs; do
        # Find the lib in ROOT
        lib_path=$(find "$ROOT/lib" "$ROOT/usr/lib" -follow \( -type f -o -type l \) -name "$lib" 2>/dev/null | head -n1)
        if [ -n "$lib_path" ]; then
            copy_and_resolve "$lib_path"
        else
            echo "Warning: $lib not found in $ROOT"
        fi
    done

    # Get interpreter (PT_INTERP)
    interp=$($TOOLCHAIN_READELF -l "$file" 2>/dev/null | awk '/Requesting program interpreter/ {print $NF}')
    if [ -n "$interp" ]; then
        interp_path="$ROOT$interp"
        if [ -f "$interp_path" ]; then
            copy_and_resolve "$interp_path"
        else
            echo "Warning: Interpreter $interp not found at $interp_path"
        fi
    fi
}

copy_and_resolve "$BINARY"
