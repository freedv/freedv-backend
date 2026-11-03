#!/usr/bin/env python3
"""Fine-tunes the predictor with an adversarial loss, so that the high band
it fills in looks like real speech rather than the average of everything
plausible.

A discriminator sees sequences of band energies in dB (all 18 bands and the
pitch correlation) together with the narrowband features they go with, and
scores each frame as real (wideband features of the same speech) or
predicted. The predictor is trained to fool it (least-squares GAN), to match
its internal activations on real input (feature matching), and still, with
less weight, to be close to the real features, which keeps the low band and
the overall level where they belong.

Usage: gan.py <train prefix> <valid prefix> <init.pt> <out.pt> [--adv W] [--rec W] [--steps N]
"""
import argparse
import time

import numpy as np
import torch
from torch import nn
from torch.nn.utils.parametrizations import spectral_norm

import bwe

p = argparse.ArgumentParser()
p.add_argument("train")
p.add_argument("valid")
p.add_argument("init")
p.add_argument("out")
p.add_argument("--adv", type=float, default=1.0, help="weight of the adversarial loss")
p.add_argument("--fm", type=float, default=2.0, help="weight of the feature-matching loss")
p.add_argument("--rec", type=float, default=1.0, help="weight of the regression loss on the high band")
p.add_argument("--rec-low", type=float, default=10.0, help="weight of the regression loss on the low band")
p.add_argument("--steps", type=int, default=20000)
p.add_argument("--batch", type=int, default=128)
p.add_argument("--frames", type=int, default=300)
p.add_argument("--lr", type=float, default=2e-4)
args = p.parse_args()

dev = "cuda"
torch.manual_seed(3)
rng = np.random.default_rng(3)

ck = torch.load(args.init, map_location="cpu")
a = ck["args"]
model = bwe.Predictor(a["hidden"], a["layers"], lookahead=a["lookahead"])
model.load_state_dict(ck["state"])
model = model.to(dev)
L = model.lookahead

wb, nb, _ = bwe.load_pairs(args.train)
target_cols = list(range(bwe.NB_BANDS)) + [bwe.CORR]
wb_t = torch.as_tensor(wb[:, target_cols])
nb_t = torch.as_tensor(nb[:, :bwe.N_IN])

# The discriminator works in dB per band, so the cepstral DCT is undone first.
idct = torch.as_tensor(bwe.DCT, dtype=torch.float32, device=dev)
LOW = 12  # bands 0-11, below 3 kHz, are what an 8 kHz mic still gives


def to_db(f):
    c = f[..., :bwe.NB_BANDS].clone()
    c[..., 0] = c[..., 0] + 4
    return torch.cat([10 * (c @ idct), 10 * f[..., bwe.NB_BANDS:]], dim=-1)


class DilatedConv(nn.Module):
    """A 1-D convolution of kernel size 3 written as shifted copies of the
    input and a matrix multiply: MIOpen's dilated convolutions are
    pathologically slow on this GPU (about 100x slower than the arithmetic)."""

    def __init__(self, n_in, n_out, dilation):
        super().__init__()
        self.d = dilation
        self.lin = spectral_norm(nn.Linear(3 * n_in, n_out))

    def forward(self, x):
        # x: (batch, frames, channels)
        d = self.d
        p = nn.functional.pad(x, (0, 0, d, d))
        return self.lin(torch.cat([p[:, :-2 * d], p[:, d:-d], p[:, 2 * d:]], dim=-1))


class Discriminator(nn.Module):
    def __init__(self, ch=128):
        super().__init__()
        n_in = (bwe.NB_BANDS + 1) + bwe.N_IN
        self.layers = nn.ModuleList(
            [DilatedConv(n_in if i == 0 else ch, ch, d) for i, d in enumerate([1, 2, 4, 8, 1])])
        self.out = spectral_norm(nn.Linear(ch, 1))

    def forward(self, cand_db, nb_cond):
        # nb_cond is normalized like the predictor's input.
        x = torch.cat([cand_db / 10, nb_cond], dim=-1)
        acts = []
        for layer in self.layers:
            x = nn.functional.leaky_relu(layer(x), 0.2)
            acts.append(x)
        return self.out(x).squeeze(-1), acts


disc = Discriminator().to(dev)
opt_g = torch.optim.AdamW(model.parameters(), lr=args.lr, betas=(0.5, 0.9))
opt_d = torch.optim.AdamW(disc.parameters(), lr=args.lr, betas=(0.5, 0.9))


def batch():
    starts = rng.integers(0, len(wb) - args.frames - L - 1, args.batch)
    i = torch.as_tensor(starts[:, None] + np.arange(args.frames + L)[None, :])
    return nb_t[i].to(dev), wb_t[i[:, :args.frames]].to(dev)


def condition(x):
    return ((x - model.mean) / model.std)[:, :args.frames]


# Fixed validation windows, as in train.py.
vwb, vnb, _ = bwe.load_pairs(args.valid)
vstarts = np.random.default_rng(2).integers(0, len(vwb) - 1000 - L, 256)
vi = torch.as_tensor(vstarts[:, None] + np.arange(1000 + L)[None, :])
vx = torch.as_tensor(vnb[:, :bwe.N_IN])[vi].to(dev)
vy = torch.as_tensor(vwb[:, target_cols])[vi[:, :1000]].to(dev)


def validate():
    """RMS dB error and the predicted/real spread ratio of the high band."""
    model.eval()
    with torch.no_grad():
        pred = to_db(model(vx)[0][:, L:])[:, 100:, LOW:bwe.NB_BANDS]
        real = to_db(vy)[:, 100:, LOW:bwe.NB_BANDS]
        rms = ((pred - real) ** 2).mean().sqrt().item()
        spread = (pred.std(dim=(0, 1)) / real.std(dim=(0, 1))).mean().item()
    model.train()
    return rms, spread


t0 = time.time()
logs = np.zeros(4)
for step in range(1, args.steps + 1):
    x, y = batch()
    cond = condition(x)
    pred = model(x)[0][:, L:]
    real_db, pred_db = to_db(y), to_db(pred)

    # Discriminator: real -> 1, predicted -> 0.
    d_real, _ = disc(real_db, cond)
    d_fake, _ = disc(pred_db.detach(), cond)
    loss_d = ((d_real - 1) ** 2).mean() + (d_fake ** 2).mean()
    opt_d.zero_grad()
    loss_d.backward()
    opt_d.step()

    # Predictor.
    d_fake, acts_fake = disc(pred_db, cond)
    with torch.no_grad():
        _, acts_real = disc(real_db, cond)
    adv = ((d_fake - 1) ** 2).mean()
    fm = sum((af - ar).abs().mean() for af, ar in zip(acts_fake, acts_real)) / len(acts_fake)
    diff = pred_db - real_db
    rec = args.rec_low * (diff[..., :LOW] ** 2).mean() / 100 + args.rec * (diff[..., LOW:bwe.NB_BANDS] ** 2).mean() / 100 \
        + 4 * (diff[..., bwe.NB_BANDS] ** 2).mean() / 100
    loss_g = args.adv * adv + args.fm * fm + rec
    opt_g.zero_grad()
    loss_g.backward()
    torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
    opt_g.step()

    logs += [loss_d.item(), adv.item(), fm.item(), rec.item()]
    if step % 1000 == 0:
        rms, spread = validate()
        print("step %d  d %.3f  adv %.3f  fm %.3f  rec %.4f  | valid high-band rms %.2f dB, spread ratio %.2f  (%.0f s)"
              % ((step,) + tuple(logs / 1000) + (rms, spread, time.time() - t0)), flush=True)
        logs[:] = 0
        torch.save({"args": a, "state": model.state_dict(), "gan_args": vars(args)}, args.out)
