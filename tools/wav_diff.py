#!/usr/bin/env python3
"""
wav_diff — compare a DS reference WAV against our renderer output.

Usage: python3 wav_diff.py <reference.wav> <ours.wav>

Per-feature parity metrics:
1.  Gain   — peak / RMS difference (dB)
2.  Period — dominant modulation period via autocorrelation (Hz)
             surfaces LFO-rate mismatches (amp LFO, pan LFO, chorus,
             phaser, etc.), and also delay echo spacing.
3.  Tail   — time from peak to -40 dB drop (seconds). RT60-ish for
             reverb / delay tail comparisons.
4.  Bands  — band-RMS Δ (dB) for spectral character.
"""
import math
import struct
import sys
import wave


SILENCE_THRESH = 200
BANDS = [
    ("sub  20-100",   20,   100),
    ("low  100-500",  100,  500),
    ("mid  500-2k",   500,  2000),
    ("hi   2k-6k",    2000, 6000),
    ("air  6k-16k",   6000, 16000),
]


def load(path):
    w = wave.open(path, "rb")
    sr = w.getframerate()
    ch = w.getnchannels()
    n = w.getnframes()
    raw = w.readframes(n)
    s = struct.unpack("<" + "h" * (n * ch), raw)
    if ch == 2:
        L = list(s[::2]); R = list(s[1::2])
    else:
        L = list(s); R = list(s)
    return sr, ch, L, R


def onset(buf, thresh=SILENCE_THRESH):
    for i, v in enumerate(buf):
        if abs(v) > thresh:
            return i
    return -1


def trim_to_onset(L, R):
    oL = onset(L); oR = onset(R)
    if oL < 0 and oR < 0: return L, R, 0
    o = min(x for x in (oL, oR) if x >= 0)
    return L[o:], R[o:], o


def rms(buf):
    return math.sqrt(sum(x * x for x in buf) / len(buf)) if buf else 0.0


def peak(buf):
    return max((abs(x) for x in buf), default=0)


def db(x):
    return 20 * math.log10(max(1e-9, x))


def envelope(buf, sr, smooth_ms=10.0):
    """Simple amplitude-envelope: abs() + one-pole LP at 1000/smooth Hz."""
    fc = 1000.0 / smooth_ms
    a = math.exp(-2 * math.pi * fc / sr)
    env = [0.0] * len(buf)
    y = 0.0
    for i, v in enumerate(buf):
        y = (1 - a) * abs(v) + a * y
        env[i] = y
    return env


def tail_to_minus_n_db(env, drop_db=40.0):
    """Frames from peak to first time env falls drop_db below peak."""
    if not env: return 0
    pk = max(env)
    if pk <= 0: return 0
    pk_i = env.index(pk)
    target = pk * (10 ** (-drop_db / 20))
    for i in range(pk_i, len(env)):
        if env[i] < target:
            return i - pk_i
    return len(env) - pk_i


def dominant_period_hz(env, sr, min_hz=0.5, max_hz=30.0):
    """Estimate the dominant LFO/modulation frequency by removing the
    DC component of the envelope and finding the lag with the highest
    autocorrelation peak in the [min_hz..max_hz] range.

    Returns Hz (0.0 if no clear period)."""
    if len(env) < int(sr / min_hz): return 0.0
    mean = sum(env) / len(env)
    centered = [e - mean for e in env]
    # Decimate to speed up — 1 kHz is plenty for sub-30 Hz periods.
    step = max(1, sr // 1000)
    sig = centered[::step]
    sr_d = sr / step
    best = (0.0, 0.0)  # (correlation, lag_samples)
    min_lag = int(sr_d / max_hz)
    max_lag = int(sr_d / min_hz)
    max_lag = min(max_lag, len(sig) - 1)
    for lag in range(max_lag, min_lag, -1):
        # Avoid the heavy O(N×lag) full ACF — sample 2000 points.
        stride = max(1, (len(sig) - lag) // 2000)
        c = 0.0
        cnt = 0
        for i in range(0, len(sig) - lag, stride):
            c += sig[i] * sig[i + lag]
            cnt += 1
        if cnt == 0: continue
        c /= cnt
        if c > best[0]:
            best = (c, lag)
    if best[1] <= 0 or best[0] <= 0: return 0.0
    return sr_d / best[1]


def band_rms(buf, sr, lo, hi):
    out = list(buf)
    if lo > 0:
        a = math.exp(-2 * math.pi * lo / sr)
        pi_, po = 0.0, 0.0
        for i, s in enumerate(out):
            y = a * (po + s - pi_)
            pi_ = s; po = y
            out[i] = y
    if hi < sr / 2:
        a = math.exp(-2 * math.pi * hi / sr)
        p = 0.0
        for i, s in enumerate(out):
            y = a * p + (1 - a) * s
            p = y
            out[i] = y
    return math.sqrt(sum(s * s for s in out) / len(out)) if out else 0.0


def metrics(L, R, sr):
    n = min(len(L), len(R))
    pk = max(peak(L), peak(R))
    r  = (rms(L) + rms(R)) / 2
    env = envelope(L, sr)
    tail_frames = tail_to_minus_n_db(env)
    period_hz = dominant_period_hz(env, sr)
    return {
        "peak": pk, "rms": r,
        "tail_s": tail_frames / sr,
        "period_hz": period_hz,
        "bands": [(name, band_rms(L, sr, lo, hi)) for name, lo, hi in BANDS],
    }


def main():
    if len(sys.argv) != 3:
        print("usage: wav_diff.py <ref.wav> <ours.wav>", file=sys.stderr)
        sys.exit(2)
    rs, _, rL, rR = load(sys.argv[1])
    os_, _, oL, oR = load(sys.argv[2])
    if rs != os_:
        print(f"sample-rate mismatch: ref={rs} ours={os_}", file=sys.stderr)
        sys.exit(2)
    sr = rs
    rL, rR, _ = trim_to_onset(rL, rR)
    oL, oR, _ = trim_to_onset(oL, oR)
    n = min(len(rL), len(oL))
    rL, rR = rL[:n], rR[:n]
    oL, oR = oL[:n], oR[:n]

    rm = metrics(rL, rR, sr)
    om = metrics(oL, oR, sr)

    print(f"trimmed window: {n} frames ({n/sr*1000:.1f} ms)")
    print(f"{'metric':16s}  {'REF':>10s}  {'OURS':>10s}  {'Δ':>10s}")
    print(f"{'peak':16s}  {rm['peak']:10d}  {om['peak']:10d}  "
          f"{db(om['peak']) - db(rm['peak']):+8.2f} dB")
    print(f"{'rms':16s}  {rm['rms']:10.1f}  {om['rms']:10.1f}  "
          f"{db(om['rms']) - db(rm['rms']):+8.2f} dB")
    print(f"{'tail (-40 dB)':16s}  {rm['tail_s']:10.3f}  {om['tail_s']:10.3f}  "
          f"{om['tail_s'] - rm['tail_s']:+8.3f} s")
    print(f"{'period Hz':16s}  {rm['period_hz']:10.2f}  {om['period_hz']:10.2f}  "
          f"{om['period_hz'] - rm['period_hz']:+8.2f} Hz")
    print()
    print(f"{'band':16s}  {'REF':>10s}  {'OURS':>10s}  {'Δ':>10s}")
    worst = 0.0
    for (rname, rb), (oname, ob) in zip(rm['bands'], om['bands']):
        d = db(ob) - db(rb)
        worst = max(worst, abs(d))
        print(f"{rname:16s}  {rb:10.1f}  {ob:10.1f}  {d:+8.2f} dB")
    print()
    # Combined parity: pass if every axis is within thresholds.
    gain_db   = abs(db(om['rms']) - db(rm['rms']))
    tail_diff = abs(om['tail_s'] - rm['tail_s'])
    period_ok = True
    if rm['period_hz'] > 0 and om['period_hz'] > 0:
        period_ratio = max(om['period_hz'], rm['period_hz']) \
                     / min(om['period_hz'], rm['period_hz'])
        period_ok = period_ratio <= 1.5  # 50% tolerance
    print(f"PARITY: gain Δ={gain_db:.2f} dB  tail Δ={tail_diff:.3f} s  "
          f"period {'OK' if period_ok else 'MISMATCH'}  "
          f"max band Δ={worst:.2f} dB")
    # Tail detection picks up DS capture's BlackHole noise floor — easy
    # to false-trigger by half a second when audio is mostly silent.
    # Period detection on near-DC envelopes also wobbles. Generous
    # thresholds keep the suite useful as a regression net without
    # drowning real fails in noise.
    fail = (gain_db > 3.0) or (tail_diff > 1.0) or (not period_ok) or (worst > 6.0)
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
