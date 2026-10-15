#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Run: python3 test_legacy_nv.py RAW_NV NEW_EVIDENCE_DIRECTORY
import hashlib
from pathlib import Path
import subprocess
import sys
from typing import Final

EXPECTED: Final = "62cabe64c22530463310e05e31eefeb1c9615d83bafac7b0ce8861e6fd983e19"
TABLES: Final = ((1, 4, 72), (2, 76, 1100), (0, 1176, 5004),
                (3, 6180, 4), (4, 6184, 20480), (5, 26664, 160),
                (18, 26824, 260), (11, 27116, 640), (12, 27756, 52),
                (13, 27808, 160), (14, 27968, 480), (15, 28448, 4),
                (16, 28452, 4))


def invoke(source: Path, destination: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(Path(__file__).with_name("project_legacy_nv.py")),
         str(source), str(destination)], check=False, capture_output=True, text=True)


def main(arguments: list[str]) -> int:
    if len(arguments) != 2:
        print("Usage: test_legacy_nv.py RAW_NV NEW_EVIDENCE_DIRECTORY", file=sys.stderr)
        return 2
    source, directory = (Path(argument) for argument in arguments)
    directory.mkdir()
    raw = source.read_bytes()
    output = directory / "projected.bin"
    result = invoke(source, output)
    assert result.returncode == 0, result.stderr
    projected = output.read_bytes()
    assert hashlib.sha256(projected).hexdigest() == EXPECTED
    assert len(projected) == 28456 and projected[:4] == raw[:4]
    assert all(projected[start:start+length] == raw[start:start+length]
               for _, start, length in TABLES)
    assert source.read_bytes() == raw
    print("PASS: CLI equals independently validated RAM candidate; all 13 tables and bitmap unchanged")
    result = invoke(source, source)
    assert result.returncode != 0 and source.read_bytes() == raw
    print("PASS: refusing input/output alias preserves original")
    result = invoke(source, output)
    assert result.returncode != 0 and output.read_bytes() == projected
    print("PASS: existing destination unchanged")
    bitmap = int.from_bytes(raw[:4], "little")
    for bit, _, _ in TABLES:
        invalid = directory / f"missing-bit-{bit}.bin"
        invalid.write_bytes((bitmap & ~(1 << bit)).to_bytes(4, "little") + raw[4:])
        destination = directory / f"rejected-bit-{bit}.bin"
        result = invoke(invalid, destination)
        assert result.returncode != 0 and not destination.exists()
    print("PASS: each of the 13 required validity bits fails before output creation")
    invalid_inputs = (raw[:3], raw[:-1], raw + b"\0", projected,
                      raw[:100] + bytes([raw[100] ^ 1]) + raw[101:])
    for index, invalid_payload in enumerate(invalid_inputs):
        invalid = directory / f"invalid-schema-{index}.bin"
        invalid.write_bytes(invalid_payload)
        destination = directory / f"rejected-schema-{index}.bin"
        result = invoke(invalid, destination)
        assert result.returncode != 0 and not destination.exists()
    assert not list(directory.glob(".nv-*"))
    assert source.read_bytes() == raw
    print("PASS: short/long/projected/unknown-schema inputs rejected; no temporary output remains")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
