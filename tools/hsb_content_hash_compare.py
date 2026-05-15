#!/usr/bin/env python3
"""Compare HSB/ZSB banks by normalized content.

Normalization rules:
- Includes resource body content for INST/ZINS, SONG/ZSNG, SND/ESND/CSND.
- Ignores resource names/IDs and ordering differences.
- For INST/ZINS, sample references are remapped by referenced sample payload hash,
  so banks can compare equal even when sample IDs differ.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from typing import Dict, Iterable, List, Tuple


@dataclass
class Resource:
    rtype: str
    rid: int
    body: bytes


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalize_type(raw_type: bytes) -> str:
    text = raw_type.decode("latin-1", errors="replace")
    return text.upper()


def parse_bank(path: str) -> List[Resource]:
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 12:
        raise ValueError(f"{path}: file too small")

    sig = data[0:4]
    if sig not in (b"IREZ", b"ZREZ"):
        raise ValueError(f"{path}: expected IREZ/ZREZ signature, got {sig!r}")

    claimed_count = struct.unpack(">I", data[8:12])[0]
    resources: List[Resource] = []

    offset = 12
    visited_offsets = set()
    max_iters = max(claimed_count + 2048, 4096)

    for _ in range(max_iters):
        if offset in visited_offsets:
            break
        visited_offsets.add(offset)

        if offset + 12 > len(data):
            break

        next_off = struct.unpack(">I", data[offset : offset + 4])[0]
        raw_type = data[offset + 4 : offset + 8]
        rid = struct.unpack(">I", data[offset + 8 : offset + 12])[0]

        p = offset + 12
        if p >= len(data):
            break

        name_len = data[p]
        p += 1
        if p + name_len + 4 > len(data):
            break

        p += name_len
        body_len = struct.unpack(">I", data[p : p + 4])[0]
        p += 4

        body_end = p + body_len
        if body_end > len(data):
            break

        resources.append(Resource(rtype=normalize_type(raw_type), rid=rid, body=data[p:body_end]))

        if next_off == 0:
            offset = body_end
        else:
            if next_off <= offset or next_off >= len(data):
                break
            offset = next_off

    return resources


def instrument_signature(body: bytes, sample_hash_by_id: Dict[int, str]) -> str:
    # Body includes ADSR and other synthesis parameters. Keep all bytes, but
    # neutralize sample ID fields and append sample-reference hashes.
    if len(body) < 14:
        return "short:" + sha256_hex(body)

    mutable = bytearray(body)

    inst_snd_id = struct.unpack(">H", body[0:2])[0]
    key_split_count = struct.unpack(">h", body[12:14])[0]
    if key_split_count < 0:
        key_split_count = 0

    refs: List[str] = []

    inst_sample_hash = sample_hash_by_id.get(inst_snd_id, f"missing:{inst_snd_id}")
    refs.append(inst_sample_hash)
    mutable[0:2] = b"\x00\x00"

    split_off = 14
    for _ in range(key_split_count):
        if split_off + 8 > len(body):
            break
        split_snd_id = struct.unpack(">H", body[split_off + 2 : split_off + 4])[0]

        effective_snd_id = split_snd_id if split_snd_id != 0 else inst_snd_id
        split_sample_hash = sample_hash_by_id.get(effective_snd_id, f"missing:{effective_snd_id}")
        refs.append(split_sample_hash)

        mutable[split_off + 2 : split_off + 4] = b"\x00\x00"
        split_off += 8

    payload = {
        "normalized_body_sha": sha256_hex(bytes(mutable)),
        "sample_refs": refs,
    }
    return sha256_hex(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("ascii"))


def collect_signatures(resources: Iterable[Resource]) -> Dict[str, Counter]:
    sample_types = {"SND ", "ESND", "CSND"}
    inst_types = {"INST", "ZINS"}
    song_types = {"SONG", "ZSNG"}

    sample_hash_by_id: Dict[int, str] = {}
    sample_sigs: List[str] = []
    song_sigs: List[str] = []
    inst_sigs: List[str] = []

    for res in resources:
        if res.rtype in sample_types:
            sig = sha256_hex(res.body)
            sample_hash_by_id[res.rid] = sig
            sample_sigs.append(sig)

    for res in resources:
        if res.rtype in inst_types:
            inst_sigs.append(instrument_signature(res.body, sample_hash_by_id))
        elif res.rtype in song_types:
            song_sigs.append(sha256_hex(res.body))

    return {
        "samples": Counter(sample_sigs),
        "songs": Counter(song_sigs),
        "instruments": Counter(inst_sigs),
    }


def counter_delta(a: Counter, b: Counter) -> List[Tuple[str, int, int]]:
    keys = sorted(set(a.keys()) | set(b.keys()))
    return [(k, a.get(k, 0), b.get(k, 0)) for k in keys if a.get(k, 0) != b.get(k, 0)]


def bank_fingerprint(sig_map: Dict[str, Counter]) -> str:
    packed = {
        key: sorted(counter.items()) for key, counter in sorted(sig_map.items())
    }
    return sha256_hex(json.dumps(packed, separators=(",", ":")).encode("ascii"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare HSB/ZSB by normalized INST/ZINS, SONG/ZSNG, and SND/ESND/CSND content",
    )
    parser.add_argument("bank_a", help="First .hsb/.zsb file")
    parser.add_argument("bank_b", help="Second .hsb/.zsb file")
    parser.add_argument("--max-show", type=int, default=20, help="Max mismatched signatures shown per section")
    parser.add_argument("--json", dest="json_out", action="store_true", help="Emit JSON result")
    args = parser.parse_args()

    try:
        res_a = parse_bank(args.bank_a)
        res_b = parse_bank(args.bank_b)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    sig_a = collect_signatures(res_a)
    sig_b = collect_signatures(res_b)

    fp_a = bank_fingerprint(sig_a)
    fp_b = bank_fingerprint(sig_b)

    deltas = {
        "samples": counter_delta(sig_a["samples"], sig_b["samples"]),
        "songs": counter_delta(sig_a["songs"], sig_b["songs"]),
        "instruments": counter_delta(sig_a["instruments"], sig_b["instruments"]),
    }

    equivalent = fp_a == fp_b

    if args.json_out:
        output = {
            "file_a": args.bank_a,
            "file_b": args.bank_b,
            "fingerprint_a": fp_a,
            "fingerprint_b": fp_b,
            "equivalent": equivalent,
            "counts_a": {k: sum(v.values()) for k, v in sig_a.items()},
            "counts_b": {k: sum(v.values()) for k, v in sig_b.items()},
            "diff_counts": {k: len(v) for k, v in deltas.items()},
            "deltas": deltas,
        }
        print(json.dumps(output, indent=2))
    else:
        print(f"A fingerprint: {fp_a}")
        print(f"B fingerprint: {fp_b}")
        print(f"Equivalent (normalized): {'YES' if equivalent else 'NO'}")
        print()

        for section in ("samples", "songs", "instruments"):
            d = deltas[section]
            print(f"{section}: {len(d)} mismatched signature buckets")
            for i, (sig, a_count, b_count) in enumerate(d[: max(args.max_show, 0)]):
                print(f"  [{i + 1}] {sig}  A={a_count}  B={b_count}")
            suppressed = len(d) - max(args.max_show, 0)
            if suppressed > 0:
                print(f"  ... {suppressed} more suppressed")
            print()

    return 0 if equivalent else 1


if __name__ == "__main__":
    raise SystemExit(main())
