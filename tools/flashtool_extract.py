#!/usr/bin/env python3
"""Extract Infineon FlashTool payloads (.fls / .dfat / .flb / .eep).

Record size fields include the 8-byte (type + size) header.
Type 0xB = partition TOC; type 0xC = load segment (0x28-byte subhdr + data);
type 0x10 = build path + partition-type id.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


def iter_tlvs(data: bytes, start: int = 0x28):
    pos = start
    while pos + 8 <= len(data):
        tid, sz = struct.unpack_from("<II", data, pos)
        if sz < 8 or pos + sz > len(data):
            break
        yield pos, tid, sz, data[pos + 8 : pos + sz]
        pos += sz


def parse_toc(body: bytes):
    # 0x14-byte header then 0x20-byte entries: name[16], addr, length, type, pad
    entries = []
    p = 0x14
    while p + 0x20 <= len(body):
        name = body[p : p + 16].split(b"\0", 1)[0].decode("latin1", "replace")
        if not name or name.startswith("\xff"):
            break
        addr, length, typ, _ = struct.unpack_from("<IIII", body, p + 16)
        entries.append((name, addr, length, typ))
        p += 0x20
    return entries


def extract(path: Path, out_dir: Path) -> None:
    data = path.read_bytes()
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = path.stem

    magic, hdr = struct.unpack_from("<II", data, 0)
    print(f"{path.name}: magic={magic:#x} hdr={hdr:#x} size={len(data)}")

    seg_i = 0
    for pos, tid, sz, body in iter_tlvs(data):
        if tid == 0xB:
            print("  TOC partitions:")
            for name, addr, length, typ in parse_toc(body):
                if length:
                    print(f"    {name:16} addr={addr:08X} len={length:08X} typ={typ:X}")
        elif tid == 0xC and sz >= 0x28:
            pay_off = pos + 0x28
            pay_len = sz - 0x28
            meta = struct.unpack_from("<IIIIII", data, pos + 8)
            blob = data[pay_off : pay_off + pay_len]
            out = out_dir / f"{stem}_seg{seg_i}_{pay_len:X}.bin"
            out.write_bytes(blob)
            print(f"  SEG{seg_i} typeC @{pos:#x} pay@{pay_off:#x}+{pay_len:#x} meta[5]={meta[5]:#x} -> {out.name}")
            seg_i += 1
        elif tid == 0x10 and sz >= 0x28:
            meta = struct.unpack_from("<IIIIII", data, pos + 8)
            path_s = data[pos + 0x28 : pos + sz].split(b"\0", 1)[0]
            print(f"  NAME type10 part={meta[5]:#x} path={path_s!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("-o", "--out", type=Path, default=Path("flashtool_out"))
    args = ap.parse_args()
    for f in args.files:
        extract(f, args.out)


if __name__ == "__main__":
    main()
