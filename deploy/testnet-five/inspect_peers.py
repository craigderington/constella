#!/usr/bin/env python3
"""Validate and print a Constella peers.dat file for release-gate evidence."""

import hashlib
import ipaddress
import struct
import sys
from pathlib import Path
from typing import NoReturn


HEADER_SIZE = 33
RECORD_SIZE = 23
CHECKSUM_SIZE = 32


def fail(message: str) -> NoReturn:
    raise SystemExit(f"invalid peers.dat: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: inspect_peers.py <peers.dat>")

    raw = Path(sys.argv[1]).read_bytes()
    if len(raw) < HEADER_SIZE + CHECKSUM_SIZE:
        fail("truncated")
    if raw[:4] != b"ADR1" or raw[4] != 1:
        fail("wrong magic or version")
    if hashlib.blake2b(raw[:-CHECKSUM_SIZE], digest_size=32).digest() != raw[-CHECKSUM_SIZE:]:
        fail("checksum mismatch")

    max_seen, new_count, tried_count = struct.unpack_from("<III", raw, 21)
    expected = HEADER_SIZE + (new_count + tried_count) * RECORD_SIZE + CHECKSUM_SIZE
    if len(raw) != expected:
        fail(f"length {len(raw)} does not match record counts ({expected})")

    print(f"new={new_count} tried={tried_count} max_seen={max_seen}")
    offset = HEADER_SIZE
    for table, count in (("new", new_count), ("tried", tried_count)):
        for _ in range(count):
            ip_raw = raw[offset : offset + 16]
            port, seen = struct.unpack_from("<HI", raw, offset + 16)
            successes = raw[offset + 22]
            ip = ipaddress.ip_address(ip_raw)
            if ip.ipv4_mapped is not None:
                ip = ip.ipv4_mapped
            print(f"{table:5} {ip}:{port} seen={seen} successes={successes}")
            offset += RECORD_SIZE


if __name__ == "__main__":
    main()
