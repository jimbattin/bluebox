#!/usr/bin/env -S uv run
"""Compare two renders by level, spectrum and timing, per channel.

Usage: wav_compare.py REF.wav TEST.wav [--rms DB] [--band DB]

The emulated machine is chaotic in its input (the 8080 branches on DSP data),
so two correct renders need not match sample for sample. This check uses the
tolerances of the Daisy-port verification instead. Each channel passes if:
- its total RMS is within --rms dB of REF (default 0.3);
- every third-octave band at 125 Hz ... 12.5 kHz is within --band dB (default
  1.0). Bands come from a Welch estimate (8192-point Hann, 50% hop);
- over the first 0.5 s after REF's onset, the cross-correlation peak within
  ±50 samples is at lag 0.

Both files must be 16-bit PCM with the same rate, channel count and length.
Exit status 0 when every channel passes. Needs numpy (in the repo's uv
environment: `uv sync`, see tools/README.md).
"""

import argparse
import sys
import wave

import numpy as np

BANDS = [125, 250, 500, 1000, 2000, 4000, 8000, 10000, 12500]
NFFT = 8192


def load(path):
    with wave.open(path) as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: only 16-bit PCM is supported")
        ch, fs = w.getnchannels(), w.getframerate()
        d = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return d.reshape(-1, ch).astype(np.float64) / 32768, fs


def welch(x):
    win = np.hanning(NFFT)
    segs = [x[i:i + NFFT] * win for i in range(0, len(x) - NFFT + 1, NFFT // 2)]
    return np.mean([np.abs(np.fft.rfft(s)) ** 2 for s in segs], axis=0)


def db(v):
    return 10 * np.log10(max(v, 1e-30))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("--rms", type=float, default=0.3)
    ap.add_argument("--band", type=float, default=1.0)
    a = ap.parse_args()
    g, fs = load(a.ref)
    t, fs_t = load(a.test)
    if fs != fs_t or g.shape != t.shape:
        sys.exit(f"shape/rate differ: {g.shape} @ {fs} vs {t.shape} @ {fs_t}")
    if len(g) < NFFT:
        sys.exit("files are shorter than one analysis window")
    f = np.fft.rfftfreq(NFFT, 1 / fs)
    ok = True
    for c in range(g.shape[1]):
        rms = 20 * np.log10(np.sqrt(np.mean(t[:, c] ** 2)) / np.sqrt(np.mean(g[:, c] ** 2)))
        pg, pt = welch(g[:, c]), welch(t[:, c])
        worst = 0.0
        for fc in BANDS:
            m = (f >= fc * 2 ** (-1 / 6)) & (f < fc * 2 ** (1 / 6))
            if m.any():
                worst = max(worst, db(pt[m].sum()) - db(pg[m].sum()), key=abs)
        on = int(np.argmax(np.abs(g[:, c]) > 1e-3))  # leading silence carries no timing
        x, y = g[on:on + int(0.5 * fs), c], t[on:on + int(0.5 * fs), c]
        n = len(x)
        lags = range(-50, 51)
        xc = [np.dot(x[max(0, -L):n - max(0, L)], y[max(0, L):n - max(0, -L)]) for L in lags]
        peak = lags[int(np.argmax(xc))]
        r = max(xc) / max(np.sqrt(np.dot(x, x) * np.dot(y, y)), 1e-30)
        p = abs(rms) <= a.rms and abs(worst) <= a.band and peak == 0
        ok &= p
        print(f"ch{c}: rms {rms:+.3f} dB, worst band {worst:+.3f} dB, xcorr peak lag {peak} "
              f"(r={r:.3f}) {'ok' if p else 'FAIL'}")
    print("ALL OK" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
