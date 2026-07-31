#!/usr/bin/env python3
"""Subtract two audio files with sample-accurate alignment.

Aligns target to source via cross-correlation, normalizes levels,
subtracts, and writes the difference to a WAV file.

Usage:
  python3 tools/audio_subtract.py source.wav target.wav [-o diff.wav] [--sr 48000]
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Tuple

import numpy as np

CACHE_DIR = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "neobae")


def load_audio(path: str, sr: int = 48000) -> Tuple[np.ndarray, int]:
    import librosa
    y, native_sr = librosa.load(path, sr=sr, mono=False)
    if y.ndim == 1:
        return y.astype(np.float32), sr
    return y.astype(np.float32), sr


def _cross_correlate_lag(a: np.ndarray, b: np.ndarray, max_lag_samples: int) -> int:
    """FFT-based cross-correlation to find integer lag aligning a to b."""
    from scipy.signal import correlate
    n = min(len(a), len(b))
    a_norm = (a[:n] - np.mean(a[:n])).astype(np.float64)
    b_norm = (b[:n] - np.mean(b[:n])).astype(np.float64)

    # Correlate centered signals
    corr = correlate(a_norm, b_norm, mode="same", method="fft")
    lags = np.arange(-n // 2, n // 2)
    # Restrict to ±max_lag
    mask = np.abs(lags) <= max_lag_samples
    best = np.argmax(corr[mask])
    return int(lags[mask][best])


def subsample_align(a: np.ndarray, b: np.ndarray, sr: int) -> float:
    """Two-stage alignment: integer-sample lag (FFT) then parabolic sub-sample refinement."""
    max_lag_s = 2.0
    max_lag_samples = int(max_lag_s * sr)
    lag_int = _cross_correlate_lag(a, b, max_lag_samples)

    # Sub-sample refinement: fit quadratic to 3 points around peak
    from scipy.signal import correlate
    n = min(len(a), len(b))
    a_norm = (a[:n] - np.mean(a[:n])).astype(np.float64)
    b_pad = np.pad(b, (max_lag_samples, max_lag_samples))

    def ncc_at(l):
        li = int(l)
        seg = b_pad[max_lag_samples + li : max_lag_samples + li + n].astype(np.float64)
        seg = seg - np.mean(seg)
        return np.dot(a_norm, seg) / (np.sqrt(np.sum(a_norm ** 2)) * np.sqrt(np.sum(seg ** 2)) + 1e-10)

    y0 = ncc_at(lag_int - 1)
    y1 = ncc_at(lag_int)
    y2 = ncc_at(lag_int + 1)
    denom = (y0 - 2 * y1 + y2)
    if abs(denom) > 1e-12:
        peak = lag_int + 0.5 * (y0 - y2) / denom
    else:
        peak = float(lag_int)

    return peak


def main() -> int:
    parser = argparse.ArgumentParser(description="Subtract two audio files with alignment")
    parser.add_argument("source", help="Reference audio file")
    parser.add_argument("target", help="Audio file to subtract from source")
    parser.add_argument("-o", "--output", default=None, help="Output difference WAV (default: <target>_diff.wav)")
    parser.add_argument("--sr", type=int, default=48000, help="Target sample rate (default: 48000)")
    parser.add_argument("--gain", type=float, default=3.0, help="Difference gain multiplier (default: 3.0)")
    parser.add_argument("--no-normalize", action="store_true", help="Skip level normalization before subtraction")
    parser.add_argument("--mono", action="store_true", help="Downmix to mono before subtraction")
    args = parser.parse_args()

    for f in [args.source, args.target]:
        if not os.path.isfile(f):
            print(f"error: file not found: {f}", file=sys.stderr)
            return 2

    print(f"Loading: {args.source}", file=sys.stderr)
    y_src, sr = load_audio(args.source, sr=args.sr)
    print(f"Loading: {args.target}", file=sys.stderr)
    y_tgt, _ = load_audio(args.target, sr=args.sr)

    is_src_mono = y_src.ndim == 1
    is_tgt_mono = y_tgt.ndim == 1

    if args.mono:
        if not is_src_mono:
            y_src = np.mean(y_src, axis=0)
        if not is_tgt_mono:
            y_tgt = np.mean(y_tgt, axis=0)
        channels = 1
        y_src_work = y_src
        y_tgt_work = y_tgt
    elif is_src_mono and is_tgt_mono:
        channels = 1
        y_src_work = y_src
        y_tgt_work = y_tgt
    elif is_src_mono:
        # Source is mono, target is stereo — duplicate source to both channels
        channels = y_tgt.shape[0]
        y_src_work = np.tile(y_src, (channels, 1))
        y_tgt_work = y_tgt
    elif is_tgt_mono:
        channels = y_src.shape[0]
        y_src_work = y_src
        y_tgt_work = np.tile(y_tgt, (channels, 1))
    else:
        channels = min(y_src.shape[0], y_tgt.shape[0])
        y_src_work = y_src[:channels]
        y_tgt_work = y_tgt[:channels]

    dur_src = y_src_work.shape[-1] / sr
    dur_tgt = y_tgt_work.shape[-1] / sr
    print(f"Source: {dur_src:.2f}s  Target: {dur_tgt:.2f}s  Channels: {channels}", file=sys.stderr)

    # Align using first channel (or mono mix)
    if channels > 1:
        ref_src = np.mean(y_src_work, axis=0)
        ref_tgt = np.mean(y_tgt_work, axis=0)
    else:
        ref_src = y_src_work
        ref_tgt = y_tgt_work

    print("Aligning ...", file=sys.stderr)
    lag_samples = subsample_align(ref_src, ref_tgt, sr)

    # Level normalization per-channel
    if not args.no_normalize:
        print("Normalizing levels ...", file=sys.stderr)
        lag_int_for_norm = int(lag_samples)
        n_align = min(y_src_work.shape[-1], y_tgt_work.shape[-1]) - abs(lag_int_for_norm)
        if n_align < sr:
            n_align = int(sr * 0.5)

        for ch in range(channels):
            if channels == 1:
                src_sel = y_src_work
                tgt_sel = y_tgt_work
            else:
                src_sel = y_src_work[ch]
                tgt_sel = y_tgt_work[ch]

            if lag_int_for_norm >= 0:
                a_seg = src_sel[:n_align]
                b_seg = tgt_sel[lag_int_for_norm : lag_int_for_norm + n_align]
            else:
                lag_abs = -lag_int_for_norm
                a_seg = src_sel[lag_abs : lag_abs + n_align]
                b_seg = tgt_sel[:n_align]
            rms_src = np.sqrt(np.mean(a_seg ** 2)) + 1e-10
            rms_tgt = np.sqrt(np.mean(b_seg ** 2)) + 1e-10
            gain = rms_src / rms_tgt
            if channels == 1:
                y_tgt_work *= gain
            else:
                y_tgt_work[ch] *= gain

    # Apply alignment shift and subtract
    lag_int = int(round(lag_samples))
    print(f"Alignment: {lag_samples:.3f} samples ({lag_samples / sr * 1000:.2f} ms)", file=sys.stderr)

    if lag_int > 0:
        pad_width = (lag_int, 0) if channels == 1 else ((0, 0), (lag_int, 0))
        tgt_shifted = np.pad(y_tgt_work, pad_width, mode="constant")
        n_comp = min(y_src_work.shape[-1], tgt_shifted.shape[-1])
        diff = y_src_work[..., :n_comp] - tgt_shifted[..., :n_comp]
    elif lag_int < 0:
        lag_abs = -lag_int
        pad_width = (lag_abs, 0) if channels == 1 else ((0, 0), (lag_abs, 0))
        src_shifted = np.pad(y_src_work, pad_width, mode="constant")
        n_comp = min(src_shifted.shape[-1], y_tgt_work.shape[-1])
        diff = src_shifted[..., :n_comp] - y_tgt_work[..., :n_comp]
    else:
        n_comp = min(y_src_work.shape[-1], y_tgt_work.shape[-1])
        diff = y_src_work[..., :n_comp] - y_tgt_work[..., :n_comp]

    # Apply gain to make differences audible
    diff *= args.gain
    diff = np.clip(diff, -1.0, 1.0)

    if channels == 1:
        diff = diff.flatten()

    rms_diff = np.sqrt(np.mean(diff ** 2))
    peak_diff = np.max(np.abs(diff))
    print(f"Difference: RMS={rms_diff:.4f}  peak={peak_diff:.4f}", file=sys.stderr)

    out_path = args.output or (os.path.splitext(args.target)[0] + "_diff.wav")
    import soundfile as sf
    sf.write(out_path, diff.T if channels > 1 else diff, sr, subtype="PCM_16")
    print(f"Wrote: {out_path}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
