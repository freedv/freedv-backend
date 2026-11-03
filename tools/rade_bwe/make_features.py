#!/usr/bin/env python3
"""Builds paired RADE input features for training the 8 kHz-mic feature
predictor.

For every LibriSpeech utterance under <root>, the 16 kHz audio is run
through lpcnet_demo -features twice:
  wb  as-is (what a 16 kHz mic gives RADE's encoder)
  nb  after going down to 8 kHz and back up to 16 kHz, as FreeDV does with an
      8 kHz mic (back up with libsamplerate's SRC_SINC_MEDIUM_QUALITY, as
      FreeDV's ResampleStep does: the model is sensitive to how much is left
      near and above 4 kHz, which differs between resamplers). Some copies are also given a telephone-style band (300 Hz-
      3.4 kHz), a little noise or a level change, so that the model doesn't
      only see perfectly clean narrowband speech.

Writes <out>.wb.f32 and <out>.nb.f32 (each NB_TOTAL_FEATURES=36 floats per
10 ms frame, frames of both files aligned) and <out>.idx (frame offset of
each utterance, as int64), in a fixed utterance order.

Usage: make_features.py <lpcnet_demo> <root> <out> [max_hours] [seed]
"""
import os
import subprocess
import sys
import tempfile
from multiprocessing import Pool

import numpy as np
import samplerate
import soundfile as sf
from scipy.signal import butter, resample_poly, sosfilt

NB_TOTAL_FEATURES = 36
RATE = 16000

def features(demo, pcm16):
    """Runs lpcnet_demo -features on 16 kHz int16 samples."""
    with tempfile.NamedTemporaryFile(suffix=".pcm") as fin, tempfile.NamedTemporaryFile(suffix=".f32") as fout:
        pcm16.tofile(fin.name)
        subprocess.run([demo, "-features", fin.name, fout.name], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return np.fromfile(fout.name, dtype=np.float32).reshape(-1, NB_TOTAL_FEATURES)


def to_pcm16(x):
    return np.clip(np.round(x * 32768), -32768, 32767).astype(np.int16)


def narrowband(x, rng):
    """What FreeDV hands RADE's feature extraction from an 8 kHz mic."""
    y = resample_poly(x, 1, 2)
    kind = rng.random()
    if kind < 0.25:
        # Telephone/CVSD-like band.
        y = sosfilt(butter(4, [300, 3400], btype="bandpass", fs=8000, output="sos"), y)
    elif kind < 0.4:
        # A bit of hiss, 30-50 dB below the speech.
        level = np.sqrt(np.mean(y ** 2)) + 1e-9
        y = y + rng.normal(0, level * 10 ** (-rng.uniform(30, 50) / 20), len(y))
    y = samplerate.resample(y, 2.0, "sinc_medium")[:len(x)]
    return np.pad(y, (0, len(x) - len(y)))


def process(args):
    demo, path, seed = args
    rng = np.random.default_rng(seed)
    x, rate = sf.read(path, dtype="float64")
    assert rate == RATE, path
    # Level as varied as real mics: -10 to +6 dB, kept from clipping.
    x = x * 10 ** (rng.uniform(-10, 6) / 20)
    peak = np.max(np.abs(x)) + 1e-9
    if peak > 0.99:
        x = x * 0.99 / peak
    wb = features(demo, to_pcm16(x))
    nb = features(demo, to_pcm16(narrowband(x, rng)))
    n = min(len(wb), len(nb))
    return wb[:n], nb[:n]


def main():
    demo, root, out = sys.argv[1:4]
    max_hours = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    paths = sorted(os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".flac"))
    rng = np.random.default_rng(seed)
    rng.shuffle(paths)
    budget = int(max_hours * 3600 * 100)

    frames = 0
    offsets = []
    with open(out + ".wb.f32", "wb") as fwb, open(out + ".nb.f32", "wb") as fnb, Pool(os.cpu_count()) as pool:
        jobs = [(demo, p, seed * 1000003 + i) for i, p in enumerate(paths)]
        for wb, nb in pool.imap(process, jobs, chunksize=4):
            if frames >= budget:
                break
            offsets.append(frames)
            wb.tofile(fwb)
            nb.tofile(fnb)
            frames += len(wb)
            if len(offsets) % 2000 == 0:
                print("%d utterances, %.1f h" % (len(offsets), frames / 360000), flush=True)
        pool.terminate()
    offsets.append(frames)
    np.array(offsets, dtype=np.int64).tofile(out + ".idx")
    print("done: %d utterances, %.1f h" % (len(offsets) - 1, frames / 360000))


if __name__ == "__main__":
    main()
