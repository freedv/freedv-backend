#!/usr/bin/env python3
"""Compares narrowband features as FreeDV sends them today, and after the
predictor, against the wideband features of the same speech.

Usage: eval_features.py <prefix> [model.pt ...]
Reports, over frames with speech, the RMS error in dB of each band's
energy, the pitch correlation error and how often the pitch differs.
"""
import sys

import numpy as np
import torch

import bwe


def load(path):
    ck = torch.load(path, map_location="cpu")
    a = ck["args"]
    m = bwe.Predictor(a["hidden"], a["layers"], lookahead=a["lookahead"])
    m.load_state_dict(ck["state"])
    return m.eval()


def report(name, wb, est, active):
    d = bwe.bands_db(est)[active] - bwe.bands_db(wb)[active]
    rms = np.sqrt((d ** 2).mean(axis=0))
    lo = np.sqrt((d[:, :12] ** 2).mean())
    hi = np.sqrt((d[:, 12:] ** 2).mean())
    corr = np.sqrt(((est[active, bwe.CORR] - wb[active, bwe.CORR]) ** 2).mean())
    print("%-14s %6.2f %6.2f %6.3f  " % (name, lo, hi, corr) + " ".join("%5.1f" % v for v in rms))


def main():
    wb, nb, idx = bwe.load_pairs(sys.argv[1])
    active = bwe.speech_frames(wb)
    print("%.2f h, %d utterances, %.0f%% of frames with speech" % (len(wb) / 360000, len(idx) - 1, 100 * active.mean()))
    pitch = lambda f: 256.0 / 2 ** ((f[:, bwe.PITCH] + 1.5))
    voiced = active & (wb[:, bwe.CORR] > 0.0)
    ratio = pitch(nb)[voiced] / pitch(wb)[voiced]
    print("voiced frames with pitch off by >5%%: %.1f%%" % (100 * np.mean(np.abs(np.log(ratio)) > np.log(1.05))))
    print("%-14s %6s %6s %6s   per-band RMS error (dB), centres %s Hz" % ("", "0-2.8k", "3.2k+", "corr", " ".join(str(b) for b in bwe.BAND_HZ)))
    report("narrowband", wb, nb, active)
    for path in sys.argv[2:]:
        model = load(path)
        est = np.concatenate([bwe.apply(model, nb[a:b]) for a, b in zip(idx[:-1], idx[1:])])
        report(path.rsplit("/", 1)[-1], wb, est, active)


if __name__ == "__main__":
    main()
