#!/usr/bin/env python3
"""Sanity checks for BE2 Session .bsn probe heuristics (no GUI)."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from bsn_session_inspect import (  # noqa: E402
    casd_entries,
    is_session_document,
    parse_irez,
    session_user_songs,
)


def main() -> int:
    base = ROOT / "reference" / "bsn"
    cases = [
        ("blank.bsn", True, 0, 0),  # thin session, no bank body
        ("empty.bsn", True, 0, 0),
        ("1song.bsn", True, 1, 0),
        ("1song_1compressedsample.bsn", True, 1, 1),
        ("1song_1compressedsample_nocompresscache_nouncompressed.bsn", True, 1, 0),
        ("1song_1compressedsample_nocompresscache_nouncompressed_multisong.bsn", True, 2, 0),
    ]
    failed = 0
    for name, expect_session, expect_songs, expect_casd in cases:
        path = base / name
        _, _, _, resources, _ = parse_irez(path)
        session = is_session_document(resources)
        songs = session_user_songs(resources)
        casd = casd_entries(resources)
        ok = (
            session == expect_session
            and len(songs) == expect_songs
            and len(casd) == expect_casd
        )
        status = "OK" if ok else "FAIL"
        print(
            f"[{status}] {name}: session={session} songs={len(songs)} casd={len(casd)} "
            f"(expected {expect_session}/{expect_songs}/{expect_casd})"
        )
        if not ok:
            failed += 1

    bank = ROOT / "neobae" / "src" / "banks" / "patches111" / "patches111.bsn"
    if bank.is_file():
        _, _, _, resources, _ = parse_irez(bank)
        session = is_session_document(resources)
        print(f"[{'OK' if not session else 'FAIL'}] patches111.bsn: session={session} (expected False)")
        if session:
            failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
