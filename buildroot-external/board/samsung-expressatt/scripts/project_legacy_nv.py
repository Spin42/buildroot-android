#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Run: python3 project_legacy_nv.py RAW_NV NEW_OUTPUT
import hashlib
import os
from pathlib import Path
import sys
import tempfile
from typing import Final

RAW_SIZE: Final = 29776
RAW_SHA256: Final = "865bea0bf48a7ce921f8834ee0ca2273c2d8ff9fe6d33ada0d369059a34f3294"
REQUIRED_BITS: Final = (1, 2, 0, 3, 4, 5, 18, 11, 12, 13, 14, 15, 16)
PROJECTED_SIZE: Final = 28456


class InvalidCalibration(ValueError):
    def __init__(self, reason: str) -> None:
        self.reason: str = reason
        super().__init__(reason)


def project_legacy_nv(raw: bytes) -> bytes:
    if len(raw) != RAW_SIZE:
        raise InvalidCalibration("unsupported raw NV size")
    bitmap = int.from_bytes(raw[:4], "little")
    if not all(bitmap & (1 << bit) for bit in REQUIRED_BITS):
        raise InvalidCalibration("required stock calibration table is not valid")
    if hashlib.sha256(raw).hexdigest() != RAW_SHA256:
        raise InvalidCalibration("unknown raw NV identity/schema")
    return raw[:27084] + b"\xff" * 26 + b"\0" * 6 + raw[27116:PROJECTED_SIZE]


def write_new_atomic(destination: Path, payload: bytes) -> None:
    with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=".nv-") as stage:
        stage.write(payload)
        stage.flush()
        os.fsync(stage.fileno())
        os.link(stage.name, destination)


def main(arguments: list[str]) -> int:
    if len(arguments) != 2:
        print("Usage: project_legacy_nv.py RAW_NV NEW_OUTPUT", file=sys.stderr)
        return 2
    try:
        payload = project_legacy_nv(Path(arguments[0]).read_bytes())
        write_new_atomic(Path(arguments[1]), payload)
    except (InvalidCalibration, OSError) as error:
        print(f"NV projection refused: {error}", file=sys.stderr)
        return 1
    print("Projected verified stock calibration for legacy RIVA; source unchanged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
