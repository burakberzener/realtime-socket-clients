#!/usr/bin/env python3
"""
analyze.py

Usage: python3 analyze.py [out.jsonl]

Reads a JSON-lines file produced by `client1` (timestamp,out1,out2,out3),
plots the three signals, computes dominant frequency, peak-to-peak amplitude,
RMS and makes a simple waveform-shape estimate for each signal.

Produces a PNG `analysis.png` and prints numeric results.
"""

import sys
import json
import math
import numpy as np
import matplotlib.pyplot as plt
from scipy.fft import rfft, rfftfreq


def parse_file(path):
    timestamps = []
    streams = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            timestamps.append(obj.get('timestamp'))
            streams.append((obj.get('out1'), obj.get('out2'), obj.get('out3')))
    return np.array(timestamps, dtype=float), streams


def to_numeric(arr):
    # arr is list of strings like '1.23' or '--'
    out = np.full(len(arr), np.nan, dtype=float)
    for i, v in enumerate(arr):
        if v is None:
            out[i] = np.nan
            continue
        if isinstance(v, (int, float)):
            out[i] = float(v)
            continue
        try:
            out[i] = float(v)
        except Exception:
            out[i] = np.nan
    return out


def resample_uniform(t_ms, x, fs):
    # t_ms in milliseconds
    t = (t_ms - t_ms[0]) / 1000.0
    dt = 1.0 / fs
    n = int(np.floor((t[-1] - t[0]) / dt)) + 1
    t_uniform = np.linspace(0, (n-1)*dt, n)
    x_interp = np.interp(t_uniform, t, x, left=np.nan, right=np.nan)
    return t_uniform, x_interp


def analyze_signal(t_ms, x_raw):
    # convert and interpolate small gaps
    x = x_raw.copy()
    # simple forward/backward fill for NaNs
    n = len(x)
    if np.all(np.isnan(x)):
        return None
    inds = np.where(np.isnan(x))[0]
    good = np.where(~np.isnan(x))[0]
    if good.size == 0:
        return None
    # linear interpolate NaNs
    notnan = ~np.isnan(x)
    x[~notnan] = np.interp(np.flatnonzero(~notnan), np.flatnonzero(notnan), x[notnan])

    # estimate sampling rate from median diff
    dt_ms = np.median(np.diff(t_ms)) if len(t_ms) > 1 else 100.0
    fs = 1000.0 / dt_ms

    # resample to uniform grid at fs (to be safe)
    t_uniform, x_u = resample_uniform(t_ms, x, fs)

    # detrend mean
    x_u = x_u - np.mean(x_u)

    # FFT
    N = len(x_u)
    if N < 4:
        return None
    yf = rfft(x_u)
    xf = rfftfreq(N, 1.0/fs)
    mag = np.abs(yf)
    # ignore DC
    mag[0] = 0
    peak_idx = np.argmax(mag)
    freq = xf[peak_idx]

    # amplitude estimates
    peak_to_peak = np.nanmax(x_u) - np.nanmin(x_u)
    rms = math.sqrt(np.mean(x_u**2))

    # harmonic analysis for shape
    # examine up to 8 harmonics
    harmonics = []
    if freq > 0:
        for k in range(1, 9):
            target = k * freq
            # find nearest bin
            idx = np.argmin(np.abs(xf - target))
            harmonics.append(mag[idx])
    else:
        harmonics = [0]*8
    harmonics = np.array(harmonics)

    # compute decay exponent by fitting log amplitude vs log(n)
    nonzero = harmonics > 1e-12
    if np.count_nonzero(nonzero) >= 3:
        ns = np.arange(1, len(harmonics)+1)[nonzero]
        amps = harmonics[nonzero]
        # avoid zeros
        logn = np.log(ns)
        loga = np.log(amps)
        coef = np.polyfit(logn, loga, 1)
        decay_exp = coef[0]
    else:
        decay_exp = 0.0

    # shape heuristic
    shape = 'unknown'
    if np.max(harmonics) == 0:
        shape = 'flat'
    else:
        h1 = harmonics[0]
        others_norm = harmonics[1:] / (h1 + 1e-12)
        if np.all(others_norm < 0.2):
            shape = 'sine-like'
        else:
            # check odd-only significant -> square-like
            odd_energy = np.sum(harmonics[0::2])  # note index 0 is 1st harmonic
            even_energy = np.sum(harmonics[1::2])
            if odd_energy > 2*even_energy:
                # examine decay
                if decay_exp < -1.5:
                    shape = 'triangle-like'
                else:
                    shape = 'square-like'
            else:
                if decay_exp < -1.5:
                    shape = 'triangle-like'
                elif decay_exp < -0.7:
                    shape = 'sawtooth-like'
                else:
                    shape = 'complex/unknown'

    return {
        'fs': fs,
        'freq': freq,
        'peak_to_peak': float(peak_to_peak),
        'rms': float(rms),
        'harmonics': harmonics.tolist(),
        'decay_exp': float(decay_exp),
        'shape': shape,
        't': t_uniform,
        'x': x_u,
        'xf': xf,
        'mag': mag,
    }


def plot_signals(t_ms, sigs, results):
    plt.figure(figsize=(12, 8))
    n = len(sigs)
    for i in range(n):
        res = results[i]
        plt.subplot(n, 2, 2*i+1)
        plt.plot(res['t'], res['x'])
        plt.title(f'out{i+1} time (shape={res["shape"]})')
        plt.xlabel('time (s)')
        plt.grid(True)

        plt.subplot(n, 2, 2*i+2)
        plt.semilogy(res['xf'], res['mag']+1e-12)
        plt.title(f'out{i+1} spectrum (peak {res["freq"]:.3f} Hz)')
        plt.xlabel('freq (Hz)')
        plt.grid(True)

    plt.tight_layout()
    plt.savefig('analyzer/analysis.png', dpi=150)
    print('Saved plot to analysis.png')


def main(argv):
    if len(argv) > 1:
        path = argv[1]
    else:
        path = 'out2.json'
    t_ms, streams = parse_file(path)
    if len(t_ms) == 0:
        print('No data found in', path)
        return 1

    # separate streams
    out1 = [s[0] for s in streams]
    out2 = [s[1] for s in streams]
    out3 = [s[2] for s in streams]

    x1 = to_numeric(out1)
    x2 = to_numeric(out2)
    x3 = to_numeric(out3)

    results = []
    for x in (x1, x2, x3):
        r = analyze_signal(t_ms, x)
        results.append(r)

    # print summary
    for i, r in enumerate(results):
        print(f'--- out{i+1} ---')
        if r is None:
            print('no signal')
            continue
        print(f'Estimated sampling rate: {r["fs"]:.3f} Hz')
        print(f'Dominant frequency: {r["freq"]:.6f} Hz')
        print(f'Peak-to-peak amplitude: {r["peak_to_peak"]:.6f}')
        print(f'RMS: {r["rms"]:.6f}')
        print(f'Shape: {r["shape"]} (decay_exp {r["decay_exp"]:.3f})')
        print(f'Harmonics (first 8): {[round(h,6) for h in r["harmonics"]]}')

    plot_signals(t_ms, (x1, x2, x3), results)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
