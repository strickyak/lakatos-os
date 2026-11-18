#!/usr/bin/env python3
"""Generates a 20 KB (0x5000) ESP32 NVS v2 partition image from /etc/lakatos/secrets.

Initializes namespace 'cyd_cfg' with:
  - devName  (from 'devname <val>')
  - shUrl    (from 'neturl <val>')
  - shPass   (from 'netpass <val>' or 'shpass <val>')
  - iata     (from 'airport <val>', uppercased 3 chars)

Intentionally leaves Wi-Fi ('cyd_wifi') and 'serialNo' absent so Wi-Fi starts
empty and Lakatos OS generates a fresh random serial (10000000..99999999) on
first boot.
"""

import pathlib
import struct
import sys
import zlib

NVS_PAGE_SIZE = 4096
NVS_PART_SIZE = 0x5000  # 20,480 bytes (5 pages), matching partitions_lakatos.csv
NVS_ENTRY_SIZE = 32
NVS_VERSION_2 = 0xFE  # 256 - 2
NVS_PAGE_ACTIVE = 0xFFFFFFFE
NVS_TYPE_U8 = 0x01
NVS_TYPE_STR = 0x21


def nvs_crc32(data: bytes) -> int:
    return zlib.crc32(data, 0xFFFFFFFF) & 0xFFFFFFFF


def pack_entry(ns_idx: int, item_type: int, span: int, key: str, data8: bytes) -> bytes:
    assert len(data8) == 8
    key_bytes = key.encode("ascii")
    if len(key_bytes) > 15:
        raise ValueError(f"NVS key too long (>15 chars): {key!r}")
    key16 = key_bytes + b"\x00" * (16 - len(key_bytes))
    hdr4 = struct.pack("<BBBB", ns_idx, item_type, span, 0xFF)
    crc = nvs_crc32(hdr4 + key16 + data8)
    return hdr4 + struct.pack("<I", crc) + key16 + data8


def parse_secrets(secrets_path: pathlib.Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw_line in secrets_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        cmd = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""
        if cmd == "devname" and arg:
            out["devName"] = arg
        elif cmd == "neturl" and arg:
            out["shUrl"] = arg
        elif cmd in ("netpass", "shpass") and arg:
            out["shPass"] = arg
        elif cmd == "airport" and arg:
            out["iata"] = arg[:3].upper()
    return out


def build_nvs_partition(cfg: dict[str, str]) -> bytes:
    page = bytearray(b"\xFF" * NVS_PAGE_SIZE)

    # Page 0 header (bytes 0..31): Active, seq=0, version=2 (0xFE)
    struct.pack_into("<II", page, 0, NVS_PAGE_ACTIVE, 0)
    page[8] = NVS_VERSION_2
    hdr_crc = nvs_crc32(bytes(page[4:28]))
    struct.pack_into("<I", page, 28, hdr_crc)

    slots: list[bytes] = []

    # Entry 0: Namespace 'cyd_cfg' -> index 1
    slots.append(pack_entry(0, NVS_TYPE_U8, 1, "cyd_cfg", struct.pack("<B7s", 1, b"\xFF" * 7)))

    # String entries in namespace 1
    for key in ("devName", "shUrl", "shPass", "iata"):
        if key not in cfg:
            continue
        str_bytes = cfg[key].encode("utf-8") + b"\x00"
        size = len(str_bytes)
        child_count = (size + NVS_ENTRY_SIZE - 1) // NVS_ENTRY_SIZE
        span = 1 + child_count
        data_crc = nvs_crc32(str_bytes)
        data8 = struct.pack("<HHI", size, 0xFFFF, data_crc)
        slots.append(pack_entry(1, NVS_TYPE_STR, span, key, data8))
        padded = str_bytes.ljust(child_count * NVS_ENTRY_SIZE, b"\xFF")
        for c in range(child_count):
            slots.append(padded[c * NVS_ENTRY_SIZE : (c + 1) * NVS_ENTRY_SIZE])

    if len(slots) > 126:
        raise ValueError(f"Too many NVS entries for a single page ({len(slots)} > 126)")

    # Write entry state bitmap (bytes 32..63): 0b10 = Written for slots 0..len(slots)-1
    for idx, raw_slot in enumerate(slots):
        byte_off = 32 + (idx // 4)
        bit_shift = (idx % 4) * 2
        page[byte_off] &= ~(1 << bit_shift)
        entry_off = (2 + idx) * NVS_ENTRY_SIZE
        page[entry_off : entry_off + NVS_ENTRY_SIZE] = raw_slot

    remaining_pages = b"\xFF" * (NVS_PART_SIZE - NVS_PAGE_SIZE)
    return bytes(page) + remaining_pages


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <secrets_file> <out_nvs_bin>", file=sys.stderr)
        sys.exit(1)

    secrets_path = pathlib.Path(sys.argv[1])
    out_path = pathlib.Path(sys.argv[2])

    cfg = parse_secrets(secrets_path)
    nvs_bytes = build_nvs_partition(cfg)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(nvs_bytes)
    keys_summary = ", ".join(sorted(cfg.keys()))
    print(f"[Lakatos NVS] Built {out_path} ({len(nvs_bytes)} B, cyd_cfg keys: {keys_summary})")


if __name__ == "__main__":
    main()
