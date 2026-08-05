#!/usr/bin/env python3
"""Convert an OpenWrt usign Ed25519 public key into an SPKI PEM public key.

apk-tools on OpenWrt 25.12 verifies usign-signed package indexes with keys in
/etc/apk/keys/ that are PEM-encoded Ed25519 public keys (the same shape as
OpenWrt's own openwrt-25.12.pem). usign stores the raw 32-byte Ed25519 key
with an "untrusted comment" header; this script re-encodes it as PEM.
"""

import base64
import binascii
import sys


SPKI_PREFIX = binascii.unhexlify("302a300506032b6570032100")


def extract_ed25519_key(payload: bytes) -> bytes:
    """Extract the raw 32-byte Ed25519 key from a usign public key payload.

    usign encodes `struct pubkey` (pkalg[2] + fingerprint[8] + pubkey[32]) as
    base64, so the payload is 42 bytes with an "Ed" magic. Plain 32-byte
    payloads are accepted as a fallback.
    """
    if len(payload) == 32:
        return payload
    if len(payload) == 42 and payload[:2] == b"Ed":
        return payload[10:42]
    raise ValueError(
        f"expected a 42-byte usign or 32-byte Ed25519 public key, "
        f"got {len(payload)} bytes"
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} USIGN_PUBLIC_KEY PEM_OUTPUT", file=sys.stderr)
        return 2
    _, key_path, pem_path = sys.argv
    encoded_lines = []
    with open(key_path, "rb") as key_file:
        for raw_line in key_file:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith(b"untrusted comment:"):
                continue
            encoded_lines.append(line)
    if len(encoded_lines) != 1:
        print("usign public key must contain exactly one base64 payload", file=sys.stderr)
        return 1
    try:
        raw_key = base64.b64decode(encoded_lines[0], validate=True)
    except binascii.Error as error:
        print(f"invalid usign public key payload: {error}", file=sys.stderr)
        return 1
    try:
        raw_key = extract_ed25519_key(raw_key)
    except ValueError as error:
        print(f"{error}", file=sys.stderr)
        return 1
    der = SPKI_PREFIX + raw_key
    b64 = base64.b64encode(der).decode("ascii")
    with open(pem_path, "w", encoding="ascii") as pem_file:
        pem_file.write("-----BEGIN PUBLIC KEY-----\n")
        for offset in range(0, len(b64), 64):
            pem_file.write(b64[offset : offset + 64] + "\n")
        pem_file.write("-----END PUBLIC KEY-----\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
