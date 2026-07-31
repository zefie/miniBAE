#!/usr/bin/env python3
"""Compare two audio files for regression without relying on hashes.

Metrics:
  Pitch          – CREPE pitch tracking, per-onset note deviation, gross drift
  Instrument     – CLAP timbre embedding (global + per-onset), chroma activity map
  Reverb/Effects – RT60 tail estimate, multi-band energy envelope correlation,
                   stereo width, spectral flatness

Usage:
  python3 tools/audio_tester.py ~/Downloads/thrilling_lloyd.m4a ~/Downloads/thrilling_lloyd.wav

Dependencies (install in your conda/torch env):
  pip install librosa torch onnxruntime transformers scipy
  pip install soundfile   # optional, better WAV loading

Model files:
  - CREPE ONNX: script downloads crepe-full.onnx on first run (cache dir ~/.cache/neobae)
  - CLAP:       AutoModel.from_pretrained("laion/clap-htsat-unfused") on first run
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np

CACHE_DIR = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "neobae")

import warnings
warnings.filterwarnings("ignore", category=FutureWarning)
os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")
os.environ.setdefault("TRANSFORMERS_VERBOSITY", "error")


def _resample(y: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    import librosa
    return librosa.resample(y, orig_sr=orig_sr, target_sr=target_sr)


def _to_mono(y: np.ndarray) -> np.ndarray:
    if y.ndim == 1:
        return y
    return np.mean(y, axis=0)


def load_audio(path: str, sr: int = 48000, mono: bool = False) -> Tuple[np.ndarray, int]:
    """Load and resample to target sr. Returns stereo by default (ndim=2)."""
    import librosa
    y, native_sr = librosa.load(path, sr=sr, mono=mono)
    return y.astype(np.float32), sr


# ─────────────────────────────────────────
#  Onset detection
# ─────────────────────────────────────────
def detect_onsets(y: np.ndarray, sr: int) -> np.ndarray:
    """Return onset times (seconds)."""
    import librosa
    onset_frames = librosa.onset.onset_detect(y=_to_mono(y), sr=sr, units="frames")
    return librosa.frames_to_time(onset_frames, sr=sr)


# ─────────────────────────────────────────
#  1. Pitch – CREPE  (ONNX)
# ─────────────────────────────────────────
CREPE_MODEL_URL = "https://github.com/yqzhishen/onnxcrepe/releases/download/v1.1.0/full.onnx"
CREPE_SR = 16000
CREPE_WINDOW = 1024
CREPE_HOP = 160
CREPE_CENTS_PER_BIN = 20
CREPE_FMIN = 32.7

# Pitch-domain validation: if the downloaded model is the old two-input ("audio","frames")
# version from keras2onnx, clear it and re-download the onnxcrepe single-input variant.
def _crepe_model_path() -> str:
    cache = os.path.join(CACHE_DIR, "crepe-full.onnx")
    os.makedirs(CACHE_DIR, exist_ok=True)
    if os.path.isfile(cache):
        import onnxruntime as ort
        try:
            s = ort.InferenceSession(cache, providers=["CPUExecutionProvider"])
            names = [i.name for i in s.get_inputs()]
            if "audio" in names:  # old two-input model, discard
                print("[setup] Removing old two-input CREPE model, re-downloading ...", file=sys.stderr)
                os.remove(cache)
        except Exception:
            pass
    if not os.path.isfile(cache):
        print(f"[setup] Downloading CREPE ONNX model to {cache} ...", file=sys.stderr)
        urllib.request.urlretrieve(CREPE_MODEL_URL, cache)
        print("[setup] Done.", file=sys.stderr)
    return cache


def _crepe_session():
    import onnxruntime as ort
    return ort.InferenceSession(_crepe_model_path(), providers=["CPUExecutionProvider"])


@dataclass
class PitchResult:
    pitch_diff_hz: float          # median abs Hz difference (resistant to octave errors)
    mean_diff_hz: float           # mean abs Hz
    max_drift_hz: float
    max_drift_pct: float          # max relative pitch deviation (cents-based)
    per_onset_drift: Dict[int, float] = field(default_factory=dict)
    frame_count: int = 0


def _frames_from_audio(y16: np.ndarray) -> np.ndarray:
    """Sliding-window: shape [n_frames, 1024] float32."""
    n = (len(y16) - CREPE_WINDOW) // CREPE_HOP + 1
    if n <= 0:
        n = 1
        y16 = np.pad(y16, (0, CREPE_WINDOW - len(y16)), mode="constant")
    frames = np.empty((n, CREPE_WINDOW), dtype=np.float32)
    for i in range(n):
        start = i * CREPE_HOP
        frames[i] = y16[start : start + CREPE_WINDOW]
    return frames


_CREPE_BIN_HZ = np.array(
    [CREPE_FMIN * (2 ** (cent / 1200)) for cent in range(0, 360 * CREPE_CENTS_PER_BIN, CREPE_CENTS_PER_BIN)],
    dtype=np.float32,
)


def _probs_to_pitch(probs: np.ndarray) -> np.ndarray:
    """Weighted-argmax decoder: 9-bin Gaussian window around max per frame -> Hz."""
    n_frames, n_bins = probs.shape
    pitch = np.empty(n_frames, dtype=np.float32)
    for t in range(n_frames):
        peak = int(np.argmax(probs[t]))
        lo = max(peak - 4, 0)
        hi = min(peak + 5, n_bins)
        w = probs[t, lo:hi]
        if np.sum(w) == 0:
            pitch[t] = 0.0
        else:
            cents = np.average(np.arange(lo, hi) * CREPE_CENTS_PER_BIN, weights=w)
            pitch[t] = CREPE_FMIN * (2 ** (cents / 1200))
    return pitch


def crepe_pitch(y: np.ndarray, sr: int) -> np.ndarray:
    """Return per-frame pitch (Hz) via CREPE ONNX."""
    y16 = _resample(y, orig_sr=sr, target_sr=CREPE_SR).astype(np.float32)
    session = _crepe_session()
    frames = _frames_from_audio(y16)
    batch_size = 256
    all_probs: List[np.ndarray] = []
    for start in range(0, len(frames), batch_size):
        batch = frames[start : start + batch_size]
        out = session.run(None, {"frames": batch})
        all_probs.append(out[0])
    probs = np.concatenate(all_probs, axis=0)
    return _probs_to_pitch(probs)


def crepe_pitch_with_confidence(y: np.ndarray, sr: int) -> Tuple[np.ndarray, np.ndarray]:
    """Return (pitch_hz, confidence) arrays from CREPE ONNX."""
    y16 = _resample(_to_mono(y), orig_sr=sr, target_sr=CREPE_SR).astype(np.float32)
    session = _crepe_session()
    frames = _frames_from_audio(y16)
    batch_size = 256
    all_probs: List[np.ndarray] = []
    for start in range(0, len(frames), batch_size):
        batch = frames[start : start + batch_size]
        out = session.run(None, {"frames": batch})
        all_probs.append(out[0])
    probs = np.concatenate(all_probs, axis=0)
    confidence = np.max(probs, axis=1).astype(np.float32)
    pitch = _probs_to_pitch(probs)
    return pitch, confidence


def _cross_correlate_lag(a: np.ndarray, b: np.ndarray, max_lag: int) -> int:
    """Find best lag so that a[t] aligns with b[t - lag] via NCC."""
    n = min(len(a), len(b))
    if n - max_lag < 10:
        return 0
    a_trim = a.astype(np.float64)
    best_lag = 0
    best_r = -np.inf
    for lag in range(-max_lag, max_lag + 1):
        lo_a = max(0, -lag)
        hi_a = n - max(0, lag)
        lo_b = max(0, lag)
        hi_b = n - max(0, -lag)
        m = min(hi_a - lo_a, hi_b - lo_b)
        if m < 10:
            continue
        a_seg = a_trim[lo_a : lo_a + m]
        b_seg = b[lo_b : lo_b + m].astype(np.float64)
        a_c = a_seg - np.mean(a_seg)
        b_c = b_seg - np.mean(b_seg)
        r = np.dot(a_c, b_c) / (np.sqrt(np.sum(a_c ** 2) * np.sum(b_c ** 2)) + 1e-10)
        if r > best_r:
            best_r = r
            best_lag = lag
    return best_lag


def compare_pitch(y_src: np.ndarray, y_tgt: np.ndarray, sr: int) -> PitchResult:
    print("  Extracting pitch (src) ...", file=sys.stderr)
    pitch_src, conf_src = crepe_pitch_with_confidence(y_src, sr)
    print("  Extracting pitch (tgt) ...", file=sys.stderr)
    pitch_tgt, conf_tgt = crepe_pitch_with_confidence(y_tgt, sr)

    raw_n = min(len(pitch_src), len(pitch_tgt))
    voiced_mask = (conf_src[:raw_n] > 0.3) & (conf_tgt[:raw_n] > 0.3)
    pitched_src = pitch_src[:raw_n].copy()
    pitched_tgt = pitch_tgt[:raw_n].copy()
    pitched_src[~voiced_mask] = 0
    pitched_tgt[~voiced_mask] = 0

    lag = _cross_correlate_lag(pitched_src, pitched_tgt, max_lag=100)
    if lag != 0:
        print(f"  Frame alignment: offset by {lag} frames ({lag * CREPE_HOP / CREPE_SR:.3f}s)", file=sys.stderr)

    n = raw_n
    if lag > 0:
        aligned_src = pitch_src[:n - lag]
        aligned_tgt = pitch_tgt[lag:n]
    elif lag < 0:
        lag_abs = -lag
        aligned_src = pitch_src[lag_abs:n]
        aligned_tgt = pitch_tgt[:n - lag_abs]
    else:
        aligned_src = pitch_src[:n]
        aligned_tgt = pitch_tgt[:n]

    aligned_n = min(len(aligned_src), len(aligned_tgt))
    aligned_src = aligned_src[:aligned_n]
    aligned_tgt = aligned_tgt[:aligned_n]
    diff = np.abs(aligned_src - aligned_tgt)
    voiced = (aligned_src > 35) & (aligned_tgt > 35)
    diff_voiced = diff[voiced] if np.any(voiced) else diff

    result = PitchResult(
        pitch_diff_hz=float(np.median(diff_voiced)) if len(diff_voiced) else 0.0,
        mean_diff_hz=float(np.mean(diff_voiced)) if len(diff_voiced) else 0.0,
        max_drift_hz=float(np.max(diff_voiced)) if len(diff_voiced) else 0.0,
        max_drift_pct=_max_pitch_drift_pct(aligned_src[voiced], aligned_tgt[voiced]),
        frame_count=aligned_n,
    )

    onsets = detect_onsets(y_src, sr)
    for i, onset_sec in enumerate(onsets):
        frame_idx = int(onset_sec * CREPE_SR / CREPE_HOP)
        if 0 <= frame_idx < aligned_n and voiced[frame_idx]:
            result.per_onset_drift[i] = float(diff[frame_idx])

    return result


def _max_pitch_drift_pct(src: np.ndarray, tgt: np.ndarray) -> float:
    """Max cents deviation across voiced frame pairs."""
    if len(src) == 0:
        return 0.0
    ratio = tgt / (src + 1e-8)
    cents_diff = np.abs(1200 * np.log2(np.clip(ratio, 1e-2, 1e2)))
    return float(np.max(cents_diff))


# ─────────────────────────────────────────
#  2. Timbre / Instrument – CLAP
# ─────────────────────────────────────────
CLAP_SR = 48000
_clap_model_cache: Optional[tuple] = None


def _get_clap():
    global _clap_model_cache
    if _clap_model_cache is not None:
        return _clap_model_cache
    from transformers import ClapModel, ClapFeatureExtractor, logging
    logging.set_verbosity_error()
    model = ClapModel.from_pretrained("laion/clap-htsat-unfused")
    extractor = ClapFeatureExtractor.from_pretrained("laion/clap-htsat-unfused")
    model.eval()
    _clap_model_cache = (model, extractor)
    return _clap_model_cache


@dataclass
class InstrumentResult:
    cosine_global: float
    cosine_per_onset: Dict[int, float] = field(default_factory=dict)
    inactive_chroma_bands: List[int] = field(default_factory=list)


def clap_embed(y: np.ndarray, sr: int, segment_sec: Optional[Tuple[float, float]] = None) -> np.ndarray:
    """Return CLAP audio embedding. If segment_sec=(start,end), embed only that slice."""
    import torch

    model, extractor = _get_clap()
    y48 = _resample(_to_mono(y), orig_sr=sr, target_sr=CLAP_SR)

    if segment_sec is not None:
        start_samp = int(segment_sec[0] * CLAP_SR)
        end_samp = int(segment_sec[1] * CLAP_SR)
        y48 = y48[start_samp:end_samp]

    if len(y48) < CLAP_SR // 10:
        y48 = np.pad(y48, (0, CLAP_SR // 10 - len(y48)), mode="constant")

    inputs = extractor(raw_speech=y48, sampling_rate=CLAP_SR, return_tensors="pt")
    with torch.no_grad():
        output = model.get_audio_features(**inputs)
    return output.pooler_output.squeeze().cpu().numpy()


def compare_instrument(y_src: np.ndarray, y_tgt: np.ndarray, sr: int) -> InstrumentResult:
    print("  Computing full-file timbre embedding ...", file=sys.stderr)
    emb_src = clap_embed(y_src, sr)
    emb_tgt = clap_embed(y_tgt, sr)

    cosine = np.dot(emb_src, emb_tgt) / (np.linalg.norm(emb_src) * np.linalg.norm(emb_tgt))
    result = InstrumentResult(cosine_global=float(cosine))

    onsets = detect_onsets(y_src, sr)
    win = 0.5
    total_onset = sum(1 for onset in onsets if onset + win <= len(y_src) / sr)
    print(f"  Computing per-onset timbre ({total_onset} onsets) ...", file=sys.stderr)
    done = 0
    for i, onset in enumerate(onsets):
        if onset + win > len(y_src) / sr:
            break
        try:
            emb_onset_src = clap_embed(y_src, sr, segment_sec=(onset, onset + win))
            emb_onset_tgt = clap_embed(y_tgt, sr, segment_sec=(onset, onset + win))
            cos = np.dot(emb_onset_src, emb_onset_tgt) / (
                np.linalg.norm(emb_onset_src) * np.linalg.norm(emb_onset_tgt)
            )
            result.cosine_per_onset[i] = float(cos)
        except Exception:
            result.cosine_per_onset[i] = -1.0
        done += 1
        if done % 10 == 0:
            print(f"    onset {done}/{total_onset}", file=sys.stderr)

    import librosa
    print("  Computing chroma activity ...", file=sys.stderr)
    chroma_src = librosa.feature.chroma_cqt(y=_to_mono(y_src), sr=sr)
    chroma_tgt = librosa.feature.chroma_cqt(y=_to_mono(y_tgt), sr=sr)
    src_energy = np.mean(chroma_src, axis=1)
    tgt_energy = np.mean(chroma_tgt, axis=1)
    rel = tgt_energy / (src_energy + 1e-8)
    for band in range(12):
        if rel[band] < 0.3:
            result.inactive_chroma_bands.append(int(band))

    return result


# ─────────────────────────────────────────
#  3. Reverb / Effects
# ─────────────────────────────────────────
@dataclass
class EffectResult:
    rt60_src: float
    rt60_tgt: float
    rt60_diff: float
    brightness_src: float
    brightness_tgt: float
    brightness_diff: float
    stereo_width_src: float
    stereo_width_tgt: float
    stereo_width_diff: float
    env_corr: float                # multi-band energy envelope Pearson r
    env_corr_per_band: Dict[str, float] = field(default_factory=dict)


def estimate_rt60(y: np.ndarray, sr: int) -> float:
    """Estimate RT60 from the tail using exponential decay fit."""
    import librosa
    y = _to_mono(y)

    # Work with the tail (last 20%)
    tail_start = int(len(y) * 0.8)
    if tail_start < sr:
        tail_start = 0
    y_tail = y[tail_start:] if tail_start > 0 else y

    # Smooth STFT energy envelope
    n_fft = 2048
    hop = n_fft // 4
    S = np.abs(librosa.stft(y_tail, n_fft=n_fft, hop_length=hop))
    energy = np.mean(S, axis=0)
    if len(energy) < 4:
        return 0.0

    # Fit exponential decay: energy ≈ A * exp(-t / tau)
    t = np.arange(len(energy)) * hop / sr
    energy_db = librosa.power_to_db(energy, ref=np.max)
    peak = np.max(energy_db)
    mask = energy_db > peak - 60
    if np.sum(mask) < 4:
        return 0.0

    # Linear regression on log-space: dB = b0 + b1*t
    # RT60 = -60 / b1 (assuming b1 is negative slope in dB/s)
    t_masked = t[mask]
    e_masked = energy_db[mask]
    coeffs = np.polyfit(t_masked, e_masked, 1)
    slope = coeffs[0]
    if slope >= 0:
        return 0.0
    rt60 = -60.0 / slope
    return float(rt60)


def spectral_flatness(y: np.ndarray, sr: int) -> float:
    import librosa
    flat = librosa.feature.spectral_flatness(y=_to_mono(y))[0]
    return float(np.mean(flat))


def stereo_width(y: np.ndarray, sr: int) -> float:
    """0 = mono, 1 = full out-of-phase stereo."""
    import librosa
    if y.ndim == 1:
        return 0.0
    y_l = y[0]
    y_r = y[1]
    n = min(len(y_l), len(y_r))
    cross_corr = np.correlate(y_l[:n], y_r[:n])[0]
    auto_l = np.correlate(y_l[:n], y_l[:n])[0]
    auto_r = np.correlate(y_r[:n], y_r[:n])[0]
    denom = np.sqrt(auto_l * auto_r) + 1e-8
    return float(1.0 - cross_corr / denom)


def multi_band_envelope_correlation(y_src: np.ndarray, y_tgt: np.ndarray, sr: int) -> Dict[str, float]:
    """Compute Pearson r between energy envelopes in 4 frequency bands.
    Low correlation in a band signals a missing or altered effect (chorus, flanger, delay, EQ).
    """
    from scipy.stats import pearsonr
    import librosa

    y_src = _to_mono(y_src)
    y_tgt = _to_mono(y_tgt)

    # Band limits in Hz
    bands = {
        "sub":   (20,    250),
        "low_mid": (250,  1000),
        "high_mid": (1000, 4000),
        "high":  (4000,  20000),
    }
    result: Dict[str, float] = {}
    n_fft = 2048
    hop = n_fft // 4

    for name, (lo, hi) in bands.items():
        env_src = _bandpass_energy_envelope(y_src, sr, lo, hi, n_fft, hop)
        env_tgt = _bandpass_energy_envelope(y_tgt, sr, lo, hi, n_fft, hop)
        n = min(len(env_src), len(env_tgt))
        if n < 4:
            result[name] = 1.0
            continue
        r, _ = pearsonr(env_src[:n], env_tgt[:n])
        result[name] = float(r if not np.isnan(r) else 0.0)

    overall = float(np.mean(list(result.values()))) if result else 1.0
    result["overall"] = overall
    return result


def _bandpass_energy_envelope(y: np.ndarray, sr: int, lo: float, hi: float,
                              n_fft: int, hop: int) -> np.ndarray:
    """Return energy over time in frequency band (dB-scale, for correlation robustness)."""
    import librosa
    D = librosa.stft(y, n_fft=n_fft, hop_length=hop)
    S = np.abs(D)
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    mask = (freqs >= lo) & (freqs <= hi)
    band_energy = np.mean(S[mask], axis=0)
    return librosa.amplitude_to_db(band_energy + 1e-10, ref=1.0)


def compare_effects(y_src: np.ndarray, y_tgt: np.ndarray, sr: int) -> EffectResult:
    print("  Estimating RT60 (reverb tail) ...", file=sys.stderr)
    rt60_src = estimate_rt60(y_src, sr)
    rt60_tgt = estimate_rt60(y_tgt, sr)
    print("  Computing spectral flatness ...", file=sys.stderr)
    bright_src = spectral_flatness(y_src, sr)
    bright_tgt = spectral_flatness(y_tgt, sr)
    print("  Computing stereo width ...", file=sys.stderr)
    sw_src = stereo_width(y_src, sr)
    sw_tgt = stereo_width(y_tgt, sr)
    print("  Computing multi-band envelope correlation ...", file=sys.stderr)
    env = multi_band_envelope_correlation(y_src, y_tgt, sr)

    return EffectResult(
        rt60_src=rt60_src,
        rt60_tgt=rt60_tgt,
        rt60_diff=rt60_tgt - rt60_src,
        brightness_src=bright_src,
        brightness_tgt=bright_tgt,
        brightness_diff=bright_tgt - bright_src,
        stereo_width_src=sw_src,
        stereo_width_tgt=sw_tgt,
        stereo_width_diff=sw_tgt - sw_src,
        env_corr=env.get("overall", 1.0),
        env_corr_per_band=env,
    )


# ─────────────────────────────────────────
#  Diagnosis / Reporting
# ─────────────────────────────────────────
@dataclass
class Diagnosis:
    name: str
    status: str        # "pass", "warn", "fail"
    detail: str
    value: float = 0.0


def diagnose(pitch: PitchResult, instrument: InstrumentResult,
             effect: EffectResult) -> List[Diagnosis]:
    results: List[Diagnosis] = []

    # Pitch — median is resistant to octave errors; <1 Hz median = identical pitch
    if pitch.pitch_diff_hz < 1:
        results.append(Diagnosis("pitch_overall", "pass", "Pitch matches (median diff)", pitch.pitch_diff_hz))
    elif pitch.pitch_diff_hz < 3:
        results.append(Diagnosis("pitch_overall", "warn", "Slight pitch deviation (cent-range, median)", pitch.pitch_diff_hz))
    else:
        results.append(Diagnosis("pitch_overall", "fail", "Significant pitch drift or wrong notes (median)", pitch.pitch_diff_hz))

    bad_onsets = {k: v for k, v in pitch.per_onset_drift.items() if v > 10}
    # Also collect relative errors: flag onsets where diff > 25% of source pitch
    n_bad_pct = 0
    total_onsets = len(pitch.per_onset_drift)
    if total_onsets > 0:
        n_bad_pct = sum(1 for v in pitch.per_onset_drift.values() if v > 10)
    pct_bad = n_bad_pct / total_onsets if total_onsets else 0
    if pct_bad > 0.10 and n_bad_pct > 2:
        ids = sorted(bad_onsets.keys())[:10]
        results.append(Diagnosis(
            "pitch_per_note",
            "fail",
            f"{n_bad_pct} notes off by >10 Hz ({pct_bad*100:.0f}% of onsets: {ids}...)",
            float(n_bad_pct),
        ))
    elif pct_bad > 0.05 and n_bad_pct > 1:
        ids = sorted(bad_onsets.keys())[:10]
        results.append(Diagnosis(
            "pitch_per_note",
            "warn",
            f"{n_bad_pct} notes off by >10 Hz ({pct_bad*100:.0f}% of onsets: {ids}...)",
            float(n_bad_pct),
        ))

    # Instrument
    if instrument.cosine_global >= 0.95:
        results.append(Diagnosis("instrument_overall", "pass", "Timbre matches", instrument.cosine_global))
    elif instrument.cosine_global >= 0.85:
        results.append(Diagnosis("instrument_overall", "warn", "Timbre slightly different", instrument.cosine_global))
    else:
        results.append(Diagnosis("instrument_overall", "fail", "Timbre substantially changed (wrong or missing instrument)", instrument.cosine_global))

    bad_segments = {k: v for k, v in instrument.cosine_per_onset.items() if v > 0 and v < 0.88}
    if bad_segments:
        ids = sorted(bad_segments.keys())[:10]
        results.append(Diagnosis(
            "instrument_per_onset",
            "warn",
            f"{len(bad_segments)} segments have weak timbre match (onsets: {ids}...)",
            float(len(bad_segments)),
        ))

    if instrument.inactive_chroma_bands:
        bands = instrument.inactive_chroma_bands
        results.append(Diagnosis(
            "instrument_missing_bands",
            "fail",
            f"Pitch class(es) {bands} largely absent in target (possible missing instrument)",
            float(len(bands)),
        ))

    # Reverb — use relative threshold (5% of src RT60 = codec noise, >15% = real difference)
    rel_diff = abs(effect.rt60_diff) / (effect.rt60_src + 1e-8) if effect.rt60_src > 0 else abs(effect.rt60_diff)
    if rel_diff < 0.05:
        results.append(Diagnosis("reverb_tail", "pass", "Reverb tail matches", rel_diff))
    elif rel_diff < 0.15:
        tag, desc = ("warn", "+") if effect.rt60_diff > 0 else ("warn", "-")
        results.append(Diagnosis("reverb_tail", tag, f"Reverb tail {desc}{rel_diff*100:.1f}% (subtle difference)", rel_diff))
    elif effect.rt60_diff > 0:
        results.append(Diagnosis("reverb_tail", "fail", f"Reverb tail is LONGER (+{rel_diff*100:.1f}%, wetter or added reverb)", rel_diff))
    else:
        results.append(Diagnosis("reverb_tail", "fail", f"Reverb tail is SHORTER ({rel_diff*100:.1f}%, drier or missing reverb)", rel_diff))

    # Brightness / EQ
    if abs(effect.brightness_diff) < 0.01:
        results.append(Diagnosis("brightness", "pass", "Brightness / EQ matches", effect.brightness_diff))
    elif effect.brightness_diff > 0:
        results.append(Diagnosis("brightness", "warn", "Target is BRIGHTER (more HF energy)", effect.brightness_diff))
    else:
        results.append(Diagnosis("brightness", "warn", "Target is DARKER (less HF energy)", effect.brightness_diff))

    # Stereo
    if abs(effect.stereo_width_diff) < 0.10:
        results.append(Diagnosis("stereo_width", "pass", "Stereo width matches", effect.stereo_width_diff))
    elif effect.stereo_width_diff > 0:
        results.append(Diagnosis("stereo_width", "fail", "Target has wider stereo (added or changed stereo effect)", effect.stereo_width_diff))
    else:
        results.append(Diagnosis("stereo_width", "fail", "Target is narrower/mono (missing stereo effect)", effect.stereo_width_diff))

    # Multi-band envelope (effect detection)
    if effect.env_corr >= 0.85:
        results.append(Diagnosis("effects_envelope", "pass", "Per-band energy envelopes match", effect.env_corr))
    elif effect.env_corr >= 0.70:
        results.append(Diagnosis("effects_envelope", "warn", "Energy envelopes slightly differ (possible subtle effect change)", effect.env_corr))
    else:
        results.append(Diagnosis("effects_envelope", "fail", "Energy envelopes diverge (likely missing or added effect)", effect.env_corr))

    # Per-band details
    for band_name, corr in effect.env_corr_per_band.items():
        if band_name == "overall":
            continue
        if corr < 0.80:
            results.append(Diagnosis(
                f"effects_env_{band_name}",
                "warn",
                f"Band '{band_name}': low envelope correlation ({corr:.3f})",
                corr,
            ))

    return results


# ─────────────────────────────────────────
#  Main
# ─────────────────────────────────────────
def main() -> int:
    parser = argparse.ArgumentParser(description="AI-based audio comparison")
    parser.add_argument("source", help="Reference (expected) audio file")
    parser.add_argument("target", help="Test (actual) audio file")
    parser.add_argument("--sr", type=int, default=48000, help="Analysis sample rate (default: 48000)")
    parser.add_argument("--json", dest="json_out", action="store_true", help="Emit JSON output")
    parser.add_argument("--exit-code", action="store_true",
                        help="Exit with non-zero if any test is 'fail'")
    args = parser.parse_args()

    for f in [args.source, args.target]:
        if not os.path.isfile(f):
            print(f"error: file not found: {f}", file=sys.stderr)
            return 2

    print(f"Loading {args.source} ...", file=sys.stderr)
    y_src, sr = load_audio(args.source, sr=args.sr)
    print(f"Loading {args.target} ...", file=sys.stderr)
    y_tgt, _ = load_audio(args.target, sr=args.sr)

    dur_src = len(y_src) / sr
    dur_tgt = len(y_tgt) / sr
    print(f"  Source duration: {dur_src:.1f}s  Target duration: {dur_tgt:.1f}s", file=sys.stderr)

    print("\n[1/3] Analyzing pitch ...", file=sys.stderr)
    pitch = compare_pitch(y_src, y_tgt, sr)

    print("\n[2/3] Analyzing instrument / timbre ...", file=sys.stderr)
    instrument = compare_instrument(y_src, y_tgt, sr)

    print("\n[3/3] Analyzing effects / reverb ...", file=sys.stderr)
    effect = compare_effects(y_src, y_tgt, sr)

    results = diagnose(pitch, instrument, effect)

    if args.json_out:
        output = {
            "source": args.source,
            "target": args.target,
            "diagnostics": [{"name": d.name, "status": d.status, "detail": d.detail, "value": d.value}
                            for d in results],
            "raw": {
                "pitch_diff_hz": pitch.pitch_diff_hz,
                "pitch_max_drift_hz": pitch.max_drift_hz,
                "timbre_cosine": instrument.cosine_global,
                "inactive_chroma_bands": instrument.inactive_chroma_bands,
                "rt60_src": effect.rt60_src,
                "rt60_tgt": effect.rt60_tgt,
                "rt60_diff": effect.rt60_diff,
                "brightness_src": effect.brightness_src,
                "brightness_tgt": effect.brightness_tgt,
                "brightness_diff": effect.brightness_diff,
                "stereo_width_src": effect.stereo_width_src,
                "stereo_width_tgt": effect.stereo_width_tgt,
                "stereo_width_diff": effect.stereo_width_diff,
                "env_corr_overall": effect.env_corr,
                "env_corr_per_band": effect.env_corr_per_band,
            },
        }
        print(json.dumps(output, indent=2))
    else:
        status_icon = {"pass": "✓", "warn": "⚠", "fail": "✗"}
        print("\n=== NeoBAE Audio Comparison ===")
        print(f"Reference : {args.source}")
        print(f"Target    : {args.target}")
        print()
        for d in results:
            icon = status_icon.get(d.status, "?")
            print(f"  {icon} {d.name:30s} [{d.status:>4}]  {d.detail}")

        print("\n--- Raw values ---")
        print(f"  pitch_diff_hz (med) : {pitch.pitch_diff_hz:.3f}")
        print(f"  pitch_diff_hz (mean): {pitch.mean_diff_hz:.3f}")
        print(f"  pitch_max_drift_hz  : {pitch.max_drift_hz:.3f}")
        print(f"  pitch_max_drift_pct : {pitch.max_drift_pct:.1f} cents")
        print(f"  timbre_cosine       : {instrument.cosine_global:.4f}")
        if instrument.inactive_chroma_bands:
            print(f"  inactive_chroma     : {instrument.inactive_chroma_bands}")
        print(f"  rt60_src            : {effect.rt60_src:.3f}s")
        print(f"  rt60_tgt            : {effect.rt60_tgt:.3f}s")
        print(f"  rt60_diff           : {effect.rt60_diff:+.3f}s")
        print(f"  brightness_src      : {effect.brightness_src:.4f}")
        print(f"  brightness_tgt      : {effect.brightness_tgt:.4f}")
        print(f"  brightness_diff     : {effect.brightness_diff:+.4f}")
        print(f"  stereo_width_src    : {effect.stereo_width_src:.4f}")
        print(f"  stereo_width_tgt    : {effect.stereo_width_tgt:.4f}")
        print(f"  stereo_width_diff   : {effect.stereo_width_diff:+.4f}")
        print(f"  env_corr_overall    : {effect.env_corr:.4f}")
        for band, corr in effect.env_corr_per_band.items():
            if band != "overall":
                print(f"  env_corr_{band:>10} : {corr:.4f}")

    fail_count = sum(1 for d in results if d.status == "fail")
    warn_count = sum(1 for d in results if d.status == "warn")
    pass_count = sum(1 for d in results if d.status == "pass")
    print(f"\nSummary: {pass_count} pass, {warn_count} warn, {fail_count} fail")

    if args.exit_code and fail_count > 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
