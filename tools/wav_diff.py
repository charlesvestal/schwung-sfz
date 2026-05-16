#!/usr/bin/env python3
"""
wav_diff — compare a DS reference WAV against our renderer output.

Per-preset usage:
    python3 wav_diff.py reference.wav ours.wav

Strategy:
1.  Detect onset (first frame above silence threshold) in both files.
2.  Slice both to a common audible window starting from onset.
3.  Compare aligned peak / RMS / band energy.
4.  Compute a single parity score (max log-magnitude diff per band, dB).
"""
import math
import struct
import sys
import wave


SILENCE = 200      # int16 magnitude considered silence
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
        L = s[::2]; R = s[1::2]
    else:
        L = R = s
    return sr, ch, L, R


def onset(buf, thresh=SILENCE):
    for i, v in enumerate(buf):
        if abs(v) > thresh:
            return i
    return -1


def trim_to_onset(L, R, pad_pre=0):
    o = min(onset(L), onset(R)) if onset(L) != -1 and onset(R) != -1 else onset(L)
    if o < 0: return L, R, 0
    start = max(0, o - pad_pre)
    return L[start:], R[start:], start


def rms(buf):
    return math.sqrt(sum(x * x for x in buf) / len(buf)) if buf else 0.0


def peak(buf):
    return max((abs(x) for x in buf), default=0)


def band_rms(buf, sr, lo, hi):
    """Crude band-RMS: HP at lo, then LP at hi (1-pole each)."""
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
    return math.sqrt(sum(s * s for s in out) / len(out)) if out else 0.0


def db(x):
    return 20 * math.log10(max(1e-9, x))


def main():
    if len(sys.argv) != 3:
        print("usage: wav_diff.py <ref.wav> <ours.wav>", file=sys.stderr)
        sys.exit(2)
    ref_path, our_path = sys.argv[1], sys.argv[2]
    rs, rc, rL, rR = load(ref_path)
    os_, oc, oL, oR = load(our_path)
    if rs != os_:
        print(f"sample-rate mismatch: ref={rs} ours={os_}", file=sys.stderr)
        sys.exit(2)
    sr = rs

    # Trim leading silence in both so they start at the same musical moment.
    rL_t, rR_t, _ = trim_to_onset(rL, rR)
    oL_t, oR_t, _ = trim_to_onset(oL, oR)
    # Trim to the shorter length so summary numbers compare like-for-like.
    n = min(len(rL_t), len(oL_t))
    rL_t = rL_t[:n]; rR_t = rR_t[:n]; oL_t = oL_t[:n]; oR_t = oR_t[:n]

    print(f"trimmed window: {n} frames ({n/sr*1000:.1f} ms)")
    print(f"{'metric':16s}  {'REF':>10s}  {'OURS':>10s}  {'Δ dB':>8s}")
    pk_r = max(peak(rL_t), peak(rR_t))
    pk_o = max(peak(oL_t), peak(oR_t))
    print(f"{'peak':16s}  {pk_r:10d}  {pk_o:10d}  {db(pk_o)-db(pk_r):+8.2f}")
    r_rL = rms(rL_t); o_rL = rms(oL_t)
    print(f"{'rms L':16s}  {r_rL:10.1f}  {o_rL:10.1f}  {db(o_rL)-db(r_rL):+8.2f}")
    r_rR = rms(rR_t); o_rR = rms(oR_t)
    print(f"{'rms R':16s}  {r_rR:10.1f}  {o_rR:10.1f}  {db(o_rR)-db(r_rR):+8.2f}")

    worst = 0.0
    for name, lo, hi in BANDS:
        br = band_rms(rL_t, sr, lo, hi)
        bo = band_rms(oL_t, sr, lo, hi)
        d  = db(bo) - db(br)
        worst = max(worst, abs(d))
        print(f"{name:16s}  {br:10.1f}  {bo:10.1f}  {d:+8.2f}")
    print()
    print(f"PARITY SCORE: max band Δ = {worst:.2f} dB")
    # Exit non-zero so the runner can flag failures.
    sys.exit(0 if worst <= 6.0 else 1)


if __name__ == "__main__":
    main()
