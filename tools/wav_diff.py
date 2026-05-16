#!/usr/bin/env python3
"""
wav_diff — compare two WAVs (one usually rendered by tools/sfz_render,
one usually captured from DecentSampler) and report peak/RMS, onset
alignment, and broad spectral character.

Usage: python3 wav_diff.py <ref.wav> <ours.wav>

Doesn't try for sample-level identity (DS uses different envelopes,
interpolation, and FX) — just surfaces big-picture diffs we can chase.
"""
import math
import struct
import sys
import wave


def load(path):
    w = wave.open(path, "rb")
    assert w.getsampwidth() == 2, f"{path}: expected 16-bit, got {w.getsampwidth()*8}"
    sr = w.getframerate()
    ch = w.getnchannels()
    n = w.getnframes()
    raw = w.readframes(n)
    samples = struct.unpack("<" + "h" * (n * ch), raw)
    if ch == 2:
        L = samples[::2]; R = samples[1::2]
    else:
        L = R = samples
    return sr, n, ch, L, R


def peak(buf):
    return max(abs(x) for x in buf) if buf else 0


def rms(buf):
    return math.sqrt(sum(x * x for x in buf) / len(buf)) if buf else 0.0


def onset(buf, thresh=500):
    for i, s in enumerate(buf):
        if abs(s) > thresh:
            return i
    return -1


def silence_end(buf, thresh=200):
    """Last frame whose abs > thresh."""
    for i in range(len(buf) - 1, -1, -1):
        if abs(buf[i]) > thresh:
            return i
    return -1


def band_rms(buf, sr, lo, hi):
    """Crude band RMS via simple time-domain filtering at edge freqs."""
    # First-order one-pole HPF at lo, LPF at hi. Not surgical but good enough
    # to surface "where is the energy" diffs without a full DFT.
    out = list(buf)
    if lo > 0:
        a = math.exp(-2 * math.pi * lo / sr)
        prev_in = 0.0; prev_out = 0.0
        for i, s in enumerate(out):
            y = a * (prev_out + s - prev_in)
            prev_in = s; prev_out = y
            out[i] = y
    if hi < sr / 2:
        a = math.exp(-2 * math.pi * hi / sr)
        prev = 0.0
        for i, s in enumerate(out):
            y = a * prev + (1 - a) * s
            prev = y
            out[i] = y
    return math.sqrt(sum(s * s for s in out) / len(out))


def summarize(path, label):
    sr, n, ch, L, R = load(path)
    pk = max(peak(L), peak(R))
    rL = rms(L); rR = rms(R)
    on = onset(L) if peak(L) > peak(R) else onset(R)
    off = silence_end(L) if peak(L) > peak(R) else silence_end(R)
    print(f"== {label}: {path}")
    print(f"    sr={sr}  ch={ch}  n={n}  duration={n/sr:.2f}s")
    print(f"    peak={pk}  rms L={rL:.1f} R={rR:.1f}")
    print(f"    onset@{on}f ({on/sr*1000:.1f}ms)  last-audible@{off}f ({off/sr*1000:.1f}ms)")
    bands = [
        ("sub  20-100",   20,   100),
        ("low  100-500",  100,  500),
        ("mid  500-2k",   500,  2000),
        ("hi   2k-6k",    2000, 6000),
        ("air  6k-16k",   6000, 16000),
    ]
    print(f"    band-RMS (L):")
    for name, lo, hi in bands:
        b = band_rms(L, sr, lo, hi)
        print(f"      {name:13s}  {b:8.1f}")
    return sr, L, R


def main():
    if len(sys.argv) != 3:
        print("usage: wav_diff.py <ref.wav> <ours.wav>", file=sys.stderr)
        sys.exit(1)
    summarize(sys.argv[1], "REF")
    print()
    summarize(sys.argv[2], "OURS")


if __name__ == "__main__":
    main()
