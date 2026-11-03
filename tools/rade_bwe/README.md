# RADE feature predictor for 8 kHz mics

These scripts trained the model in `src/pipeline/rade_bwe_data.c`, which
`RADEFeatureExpander` uses to fill in the high band (4-8 kHz) of RADE's input
features when the mic only gives 8 kHz audio. Only the transmitter changes:
receivers decode the result like any other RADE signal.

The model is causal, adds no delay, and has about 208k parameters:
a convolution over the last three 10 ms frames, two 128-unit GRUs, and a
linear layer that predicts the change to the 18 band energies and the pitch
correlation. The pitch period is left as it is.

## Steps

All need Python 3 with numpy, scipy, soundfile, samplerate and PyTorch. `lpcnet_demo` is
built with RADE (`build/rade_build/build_opus-prefix/src/build_opus/lpcnet_demo`).

1. Features. These are paired features from LibriSpeech (16 kHz FLAC): each
   utterance as-is, and again after going down to 8 kHz and back. Some copies
   are band-limited to 300-3400 Hz, have a little noise added, or have their
   level changed.

       make_features.py lpcnet_demo LibriSpeech/train-clean-100 feats/train
       make_features.py lpcnet_demo LibriSpeech/dev-clean feats/dev
       make_features.py lpcnet_demo LibriSpeech/test-clean feats/test

2. Train on squared error (30k steps):

       train.py feats/train feats/dev models/h128_sr.pt

3. Fine-tune with an adversarial loss, so that the high band varies like real
   speech rather than being averaged out:

       gan.py feats/train feats/dev models/h128_sr.pt models/gan_a1_sr.pt --adv 1

4. Check it. `eval_features.py` gives the RMS error per band.
   `eval_dist.py` gives the error, spread, motion and distribution distance of
   the high band:

       eval_dist.py feats/test models/h128_sr.pt models/gan_a1_sr.pt

5. Export to C. This also writes reference frames for
   `RADEFeatureExpanderTest`:

       PYTHONPATH=<opus>/dnn/torch/weight-exchange \
           export_c.py models/gan_a1_sr.pt out feats/test

   Copy `out/rade_bwe_data.{c,h}` to `src/pipeline` and
   `out/rade_bwe_test_vectors.h` to `src/pipeline/test`.

## Results (test-clean)

The narrowband input here went through libsamplerate, as it does in FreeDV.
The last column is for speech sent through `RADETransmitStep` itself, then
RADE V1 on a clean channel, then FARGAN.

| | error, bands 3.2 kHz and up | bias | spread | motion | energy above 4 kHz at the far end |
|---|---|---|---|---|---|
| 8 kHz mic | 33.0 dB | -27.1 dB | 0.53 | 0.64 | -44.2 dB |
| squared error only (`h128_sr`) | 7.03 dB | -0.47 dB | 0.90 | 0.87 | |
| adversarial (`gan_a1_sr`, shipped) | 7.92 dB | -0.26 dB | 0.96 | 1.00 | -20.9 dB |
| 16 kHz mic | | | | | -19.1 dB |

Spread and motion are ratios to real speech; 1.0 is ideal. In earlier
prototypes the high band held up over AWGN and fading channels, and with
RADE V1 and V2.

A model trained on input upsampled with `scipy.signal.resample_poly` instead
reached only -31.1 dB at the far end once it was in FreeDV: libsamplerate
leaves about 9 dB less near and above 4 kHz, and the model had learned to
rely on what was left.
