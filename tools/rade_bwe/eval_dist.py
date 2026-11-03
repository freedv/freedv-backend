#!/usr/bin/env python3
"""Compares how the predicted high band behaves with the real one's, over
frames with speech in the test set.

Usage: eval_dist.py <prefix> <model.pt> ...

For bands 3.2 kHz and up, averaged over those bands:
  rms     RMS error in dB against the real band energies (the regression
          model is the best possible here by construction)
  bias    mean predicted minus real level, in dB
  spread  standard deviation of the predicted levels over that of the real ones
  motion  the same ratio for frame-to-frame changes (how much the band moves)
  W1      Wasserstein-1 distance between predicted and real level
          distributions, in dB (0 when they're distributed alike)
and the same for fricative-like frames only (where the real high band is
within 10 dB of the low band), which is where a dull high band shows most.
"""
import sys

import numpy as np

import bwe
from eval_features import load


def w1(a, b):
    q = np.linspace(0.005, 0.995, 199)
    return float(np.mean(np.abs(np.quantile(a, q) - np.quantile(b, q))))


def stats(real, pred, mask, dmask):
    r, p = real[mask][:, 12:], pred[mask][:, 12:]
    dr = np.diff(real, axis=0)[dmask][:, 12:]
    dp = np.diff(pred, axis=0)[dmask][:, 12:]
    return (np.sqrt(((p - r) ** 2).mean()),
            (p - r).mean(),
            np.mean(p.std(axis=0) / r.std(axis=0)),
            np.mean(dp.std(axis=0) / dr.std(axis=0)),
            np.mean([w1(p[:, k], r[:, k]) for k in range(r.shape[1])]))


def main():
    wb, nb, idx = bwe.load_pairs(sys.argv[1])
    real = bwe.bands_db(wb)
    speech = bwe.speech_frames(wb)
    fric = speech & (real[:, 12:].max(axis=1) > real[:, :12].max(axis=1) - 10)
    # Frame-to-frame changes within an utterance, both frames with speech.
    same = np.ones(len(wb) - 1, bool)
    same[idx[1:-1] - 1] = False
    print("%.0f%% of speech frames are fricative-like" % (100 * fric.sum() / speech.sum()))
    print("%-16s %28s   %s" % ("", "all speech", "fricative-like"))
    print("%-16s" % "" + ("%7s" * 5 % ("rms", "bias", "spread", "motion", "W1")) + "   " + ("%7s" * 5 % ("rms", "bias", "spread", "motion", "W1")))
    rows = [("narrowband", nb)]
    for path in sys.argv[2:]:
        model = load(path)
        rows.append((path.rsplit("/", 1)[-1], np.concatenate([bwe.apply(model, nb[a:b]) for a, b in zip(idx[:-1], idx[1:])])))
    for name, f in rows:
        pred = bwe.bands_db(f)
        s1 = stats(real, pred, speech, same & speech[1:] & speech[:-1])
        s2 = stats(real, pred, fric, same & fric[1:] & fric[:-1])
        print("%-16s" % name + ("%7.2f" * 5 % s1) + "   " + ("%7.2f" * 5 % s2))


if __name__ == "__main__":
    main()
