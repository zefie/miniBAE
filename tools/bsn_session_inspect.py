#!/usr/bin/env python3
"""Inspect Beatnik Editor 2 Session (.bsn) / IREZ resource files.

Session documents are IREZ+BEPF banks with editor metadata (BePf/DATe) and
optional user Midi/SONG/CaSd resources. See docs/be2_session_bsn.md.
"""
from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple


Resource = Dict[str, object]


def parse_irez(path: Path) -> Tuple[bytes, int, int, List[Resource], bytes]:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] not in (b"IREZ", b"ZREZ"):
        raise ValueError(f"{path}: missing IREZ/ZREZ signature")
    ver, num = struct.unpack(">II", data[4:12])
    offset = 12
    resources: List[Resource] = []
    for idx in range(num):
        if offset + 12 > len(data):
            break
        next_off = struct.unpack(">I", data[offset : offset + 4])[0]
        rtype = data[offset + 4 : offset + 8]
        rid = struct.unpack(">I", data[offset + 8 : offset + 12])[0]
        p = offset + 12
        if p >= len(data):
            break
        name_len = data[p]
        name = data[p + 1 : p + 1 + name_len]
        p = p + 1 + name_len
        if p + 4 > len(data):
            break
        body_len = struct.unpack(">I", data[p : p + 4])[0]
        body = data[p + 4 : p + 4 + body_len]
        resources.append(
            {
                "idx": idx,
                "type": rtype,
                "id": rid,
                "name": name,
                "body": body,
                "body_len": body_len,
                "hdr_offset": offset,
                "next_off": next_off,
            }
        )
        offset = p + 4 + body_len
    return data[:4], ver, num, resources, data


def song_header(body: bytes) -> Optional[dict]:
    if len(body) < 8:
        return None
    return {
        "midi_id": struct.unpack(">H", body[0:2])[0],
        "reserved": body[2],
        "reverb": body[3],
        "tempo": struct.unpack(">H", body[4:6])[0],
        "song_type": body[6],  # 0 SMS, 1 RMF structured, 2 linear/MOD (legacy)
    }


def is_session_document(resources: List[Resource]) -> bool:
    for r in resources:
        if r["type"] == b"BePf" and r["name"] == b"Session Prefs":
            return True
    return False


def index_by_type_id(resources: List[Resource]) -> Dict[Tuple[bytes, int], Resource]:
    out: Dict[Tuple[bytes, int], Resource] = {}
    for r in resources:
        out[(r["type"], int(r["id"]))] = r
    return out


def session_user_songs(resources: List[Resource]) -> List[dict]:
    """SONG entries whose object resource is type Midi (not emid groovoids)."""
    by = index_by_type_id(resources)
    songs = []
    for r in resources:
        if r["type"] != b"SONG":
            continue
        hdr = song_header(r["body"])  # type: ignore[arg-type]
        if not hdr:
            continue
        midi = by.get((b"Midi", hdr["midi_id"]))
        if not midi:
            continue
        songs.append(
            {
                "song_id": int(r["id"]),
                "name": r["name"].decode("latin-1", errors="replace"),  # type: ignore[union-attr]
                "midi_id": hdr["midi_id"],
                "midi_name": midi["name"].decode("latin-1", errors="replace"),  # type: ignore[union-attr]
                "midi_len": int(midi["body_len"]),
                "song_type": hdr["song_type"],
                "reverb": hdr["reverb"],
                "tempo": hdr["tempo"],
            }
        )
    songs.sort(key=lambda s: s["song_id"])
    return songs


def casd_entries(resources: List[Resource]) -> List[dict]:
    out = []
    for r in resources:
        if r["type"] != b"CaSd":
            continue
        body: bytes = r["body"]  # type: ignore[assignment]
        rate = None
        # Common: classic Mac snd format 1 with ExtendedHeader (0xFF) + rate44khz
        if len(body) >= 2 and struct.unpack(">H", body[0:2])[0] == 1:
            pos = body.find(b"\xff")
            if pos >= 0 and pos + 10 < len(body):
                # ExtendedHeader: encode(FF) baseKey rate(4) ...
                maybe_rate = struct.unpack(">I", body[pos + 6 : pos + 10])[0]
                if maybe_rate in (0xAC440000, 0x56220000, 0x2B110000, 0xBB800000):
                    rate = maybe_rate >> 16
        out.append(
            {
                "id": int(r["id"]),
                "name": r["name"].decode("latin-1", errors="replace"),  # type: ignore[union-attr]
                "body_len": int(r["body_len"]),
                "snd_format": struct.unpack(">H", body[0:2])[0] if len(body) >= 2 else None,
                "guess_rate_hz": rate,
            }
        )
    return out


def parse_date(body: bytes) -> List[dict]:
    entries = []
    off = 0
    while off + 12 <= len(body):
        tag = body[off : off + 4]
        rid, ts = struct.unpack(">II", body[off + 4 : off + 12])
        if tag == b"\x00\x00\x00\x00" and rid == 0 and ts == 0:
            break
        entries.append({"type": tag, "id": rid, "timestamp": ts})
        off += 12
    return entries


def inspect(path: Path, verbose: bool = False) -> int:
    sig, ver, claimed, resources, data = parse_irez(path)
    counts = Counter(r["type"] for r in resources)
    session = is_session_document(resources)
    songs = session_user_songs(resources)
    casds = casd_entries(resources)

    print(f"File: {path}")
    print(f"  signature: {sig!r}  version: {ver}  claimed: {claimed}  parsed: {len(resources)}  size: {len(data)}")
    print(f"  session document (BePf 'Session Prefs'): {session}")
    print("  resource counts:")
    for t, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        total = sum(int(r["body_len"]) for r in resources if r["type"] == t)
        label = t.decode("latin-1", errors="replace")
        print(f"    {label!r:8} count={n:4d}  body_bytes={total}")

    if songs:
        print(f"  user songs (SONG→Midi): {len(songs)}")
        for s in songs:
            print(
                f"    SONG id={s['song_id']} name={s['name']!r} "
                f"→ Midi id={s['midi_id']} ({s['midi_len']} bytes) name={s['midi_name']!r}"
            )
    else:
        print("  user songs (SONG→Midi): none")

    if casds:
        print(f"  uncompressed PCM cache (CaSd): {len(casds)}")
        for c in casds:
            print(
                f"    CaSd id={c['id']} name={c['name']!r} len={c['body_len']} "
                f"snd_fmt={c['snd_format']} rate≈{c['guess_rate_hz']}"
            )
    else:
        print("  uncompressed PCM cache (CaSd): none")

    nbeds = [r for r in resources if r["type"] == b"NbEd"]
    if nbeds:
        print(f"  NeoBAE session mods (NbEd): {len(nbeds)}")
        for r in nbeds:
            body: bytes = r["body"]  # type: ignore[assignment]
            name = r["name"].decode("latin-1", errors="replace")  # type: ignore[union-attr]
            print(f"    NbEd id={int(r['id'])} name={name!r} len={int(r['body_len'])}")
            if len(body) >= 8:
                ver, sect_count = struct.unpack(">II", body[0:8])
                print(f"      version={ver} sections={sect_count}")
                off = 8
                for _ in range(sect_count):
                    if off + 8 > len(body):
                        break
                    stype = body[off : off + 4]
                    ssize = struct.unpack(">I", body[off + 4 : off + 8])[0]
                    off += 8
                    payload = body[off : off + ssize]
                    off += ssize
                    if stype == b"GhSt" and len(payload) >= 4:
                        count = struct.unpack(">I", payload[0:4])[0]
                        ids = []
                        for i in range(count):
                            if 4 + (i + 1) * 4 <= len(payload):
                                ids.append(struct.unpack(">I", payload[4 + i * 4 : 8 + i * 4])[0])
                        print(f"      GhSt ghost INST ids ({count}): {ids}")
                    else:
                        print(f"      section {stype!r} payload={ssize} bytes")
    else:
        print("  NeoBAE session mods (NbEd): none")

    custom_snd = [
        r
        for r in resources
        if r["type"] in (b"snd ", b"csnd", b"esnd") and int(r["id"]) < 256
    ]
    # Heuristic print: low-id snd often custom in these fixtures (id 0)
    low_snd = [r for r in resources if r["type"] == b"snd " and int(r["id"]) == 0]
    if low_snd:
        print("  custom bank samples (snd id 0):")
        for r in low_snd:
            print(
                f"    snd  id={r['id']} name={r['name']!r} len={r['body_len']}"  # type: ignore[str-format]
            )

    nbets = [r for r in resources if r["type"] == b"nBeT"]
    if nbets:
        print(f"  NeoBAE editor layout (nBeT): {len(nbets)}")
        for r in nbets:
            body: bytes = r["body"]  # type: ignore[assignment]
            name = r["name"].decode("latin-1", errors="replace")  # type: ignore[union-attr]
            print(f"    nBeT id={int(r['id'])} name={name!r} len={int(r['body_len'])}")
            if len(body) >= 32 and body[0:4] == b"nBeT":
                ver = struct.unpack(">I", body[4:8])[0]
                x, y, w, h, maximized = struct.unpack(">iiiiI", body[8:28])
                if ver >= 2 and len(body) >= 64:
                    open_flags, ie_inst, ie_index, ie_bank, ie_prog, ie_song, se_row, se_song, ini_len = (
                        struct.unpack(">IIIiiIiII", body[28:64])
                    )
                    print(
                        f"      version={ver} window=({x},{y},{w}x{h}) maximized={maximized} "
                        f"open_flags=0x{open_flags:x} ie_inst={ie_inst} se_row={se_row} "
                        f"imgui_ini_bytes={ini_len}"
                    )
                else:
                    ini_len = struct.unpack(">I", body[28:32])[0]
                    print(
                        f"      version={ver} window=({x},{y},{w}x{h}) maximized={maximized} "
                        f"imgui_ini_bytes={ini_len}"
                    )
    else:
        print("  NeoBAE editor layout (nBeT): none")

    for r in resources:
        if r["type"] == b"BePf":
            body: bytes = r["body"]  # type: ignore[assignment]
            print(f"  BePf id={r['id']} name={r['name']!r} len={len(body)} hex={body.hex()}")
        if r["type"] == b"DATe" and verbose:
            print(f"  DATe entries ({int(r['body_len'])} bytes):")
            for e in parse_date(r["body"]):  # type: ignore[arg-type]
                print(f"    {e['type']!r} id={e['id']} ts={e['timestamp']}")

    if verbose:
        print("  all resources:")
        for r in resources:
            t = r["type"].decode("latin-1", errors="replace")  # type: ignore[union-attr]
            n = r["name"].decode("latin-1", errors="replace")  # type: ignore[union-attr]
            print(f"    [{r['idx']}] {t!r} id={r['id']} name={n!r} len={r['body_len']}")

    return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", type=Path, help="Session/bank .bsn paths")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    rc = 0
    for path in args.paths:
        try:
            inspect(path, verbose=args.verbose)
        except Exception as exc:  # noqa: BLE001 - CLI tool
            print(f"{path}: {exc}", file=sys.stderr)
            rc = 1
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
