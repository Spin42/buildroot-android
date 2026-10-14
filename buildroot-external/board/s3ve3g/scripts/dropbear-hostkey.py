#!/usr/bin/env python3
"""Convert an unencrypted OpenSSH ed25519 private key to dropbear's key format.

Usage: dropbear-hostkey.py <openssh private key> <dropbear key out>

Lets the build give the rescue image a fixed host key made with the host's ssh-keygen, so the
host's known_hosts entry stays valid from one build to the next. Dropbear's ed25519 format is
string "ssh-ed25519", then string (32-byte seed || 32-byte public key); OpenSSH stores the same
64 bytes as its private key field.
"""
import base64
import struct
import sys


def read_string(buf, off):
    (n,) = struct.unpack_from(">I", buf, off)
    off += 4
    return buf[off:off + n], off + n


def ssh_string(data):
    return struct.pack(">I", len(data)) + data


def main(src, dst):
    lines = open(src).read().strip().splitlines()
    if lines[0] != "-----BEGIN OPENSSH PRIVATE KEY-----":
        sys.exit(f"{src}: not an OpenSSH private key")
    blob = base64.b64decode("".join(lines[1:-1]))
    magic = b"openssh-key-v1\0"
    if not blob.startswith(magic):
        sys.exit(f"{src}: bad magic")
    off = len(magic)
    cipher, off = read_string(blob, off)
    kdf, off = read_string(blob, off)
    _kdfopts, off = read_string(blob, off)
    if cipher != b"none" or kdf != b"none":
        sys.exit(f"{src}: encrypted keys are not supported")
    (nkeys,) = struct.unpack_from(">I", blob, off)
    off += 4
    if nkeys != 1:
        sys.exit(f"{src}: expected one key, found {nkeys}")
    _pub, off = read_string(blob, off)
    priv, off = read_string(blob, off)

    p = 0
    check1, check2 = struct.unpack_from(">II", priv, p)
    p += 8
    if check1 != check2:
        sys.exit(f"{src}: corrupt private section")
    keytype, p = read_string(priv, p)
    if keytype != b"ssh-ed25519":
        sys.exit(f"{src}: key type {keytype.decode()}, expected ssh-ed25519")
    pub, p = read_string(priv, p)
    secret, p = read_string(priv, p)
    if len(pub) != 32 or len(secret) != 64 or secret[32:] != pub:
        sys.exit(f"{src}: malformed ed25519 key")

    with open(dst, "wb") as f:
        f.write(ssh_string(b"ssh-ed25519") + ssh_string(secret))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
