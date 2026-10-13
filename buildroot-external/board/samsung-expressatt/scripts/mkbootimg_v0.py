#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# ─── How to run ───
# uv run mkbootimg_v0.py KERNEL RAMDISK OUTPUT
# ────────────────────
"""Pack the SGH-I437 lk2nd test image in Android boot header v0 format."""

from __future__ import annotations

import hashlib
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from os import environ
from pathlib import Path
from typing import Final, override

PAGE_SIZE: Final = 2048
KERNEL_ADDRESS: Final = 0x80208000
RAMDISK_ADDRESS: Final = 0x84000000
SECOND_ADDRESS: Final = 0x81100000
TAGS_ADDRESS: Final = 0x83000000
DEFAULT_CMDLINE: Final = (
    "console=tty0 console=ttyMSM0,115200n8 earlycon loglevel=8 "
    "ignore_loglevel fbcon=font:VGA8x16 lk2nd.pass-simplefb=autorefresh "
    "lk2nd.pass-ramoops=zap panic=10 kit.fail_reboot=60 "
    "clk_ignore_unused pd_ignore_unused regulator_ignore_unused "
    "lk2nd.expressatt-boot-guard initcall_blacklist=qcom_watchdog_driver_init"
)
CMDLINE: Final = environ.get("EXPRESSATT_BOOT_CMDLINE", DEFAULT_CMDLINE)


@dataclass(frozen=True, slots=True)
class ImagePaths:
    """Input and output paths for one boot image."""

    kernel: Path
    ramdisk: Path
    output: Path


@dataclass(frozen=True, slots=True)
class UsageError(Exception):
    """The command line did not contain exactly three paths."""

    arguments: tuple[str, ...]

    @override
    def __str__(self) -> str:
        return "usage: mkbootimg_v0.py KERNEL RAMDISK OUTPUT"


@dataclass(frozen=True, slots=True)
class ImageCommandLineError(Exception):
    command_line: str

    @override
    def __str__(self) -> str:
        return "boot cmdline must be ASCII and shorter than 512 bytes"


def parse_paths(arguments: Sequence[str]) -> ImagePaths:
    """Parse the three positional path arguments."""
    if len(arguments) != 3:
        raise UsageError(tuple(arguments))
    return ImagePaths(*(Path(argument) for argument in arguments))


def pad_page(payload: bytes) -> bytes:
    """Pad a boot-image component to the v0 page boundary."""
    return payload + (b"\0" * (-len(payload) % PAGE_SIZE))


def build_image(paths: ImagePaths) -> None:
    """Write one boot image using the addresses proven for this board."""
    try:
        command_line = CMDLINE.encode("ascii")
    except UnicodeEncodeError as error:
        raise ImageCommandLineError(CMDLINE) from error
    if len(command_line) >= 512:
        raise ImageCommandLineError(CMDLINE)
    kernel = paths.kernel.read_bytes()
    ramdisk = paths.ramdisk.read_bytes()
    digest = hashlib.sha1(usedforsecurity=False)
    for payload in (kernel, ramdisk, b""):
        digest.update(payload)
        digest.update(struct.pack("<I", len(payload)))

    header = b"ANDROID!" + struct.pack(
        "<10I",
        len(kernel),
        KERNEL_ADDRESS,
        len(ramdisk),
        RAMDISK_ADDRESS,
        0,
        SECOND_ADDRESS,
        TAGS_ADDRESS,
        PAGE_SIZE,
        0,
        0,
    )
    header += b"".ljust(16, b"\0")
    header += command_line.ljust(512, b"\0")
    header += digest.digest().ljust(32, b"\0")
    _ = paths.output.write_bytes(pad_page(header) + pad_page(kernel) + pad_page(ramdisk))


def main(arguments: Sequence[str]) -> int:
    """Pack the image or report command-line misuse."""
    try:
        paths = parse_paths(arguments)
    except UsageError as error:
        print(error, file=sys.stderr)
        return 2
    try:
        build_image(paths)
    except ImageCommandLineError as error:
        print(error, file=sys.stderr)
        return 2
    print(f"{paths.output}: kernel {paths.kernel.stat().st_size} ramdisk {paths.ramdisk.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
