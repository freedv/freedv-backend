#!/usr/bin/env python3
"""Trains the 8 kHz-mic feature predictor.

Usage: train.py <train prefix> <valid prefix> <out.pt> [--hidden N] [--lookahead N] [--steps N]
"""
import argparse
import time

import numpy as np
import torch

import bwe

p = argparse.ArgumentParser()
p.add_argument("train")
p.add_argument("valid")
p.add_argument("out")
p.add_argument("--hidden", type=int, default=128)
p.add_argument("--layers", type=int, default=2)
p.add_argument("--lookahead", type=int, default=0, help="frames of delay the model may use")
p.add_argument("--steps", type=int, default=30000)
p.add_argument("--batch", type=int, default=128)
p.add_argument("--frames", type=int, default=300)
p.add_argument("--lr", type=float, default=2e-3)
p.add_argument("--corr-weight", type=float, default=4.0)
args = p.parse_args()

dev = "cuda"
torch.manual_seed(1)
rng = np.random.default_rng(1)

wb, nb, _ = bwe.load_pairs(args.train)
vwb, vnb, vidx = bwe.load_pairs(args.valid)
print("train %.1f h, valid %.1f h" % (len(wb) / 360000, len(vwb) / 360000), flush=True)

# Bands the model has to get right: what RADE sends, in band-energy terms
# (orthonormal DCT, so squared cepstral error is squared log-band error).
mean = nb[:, :bwe.N_IN].mean(axis=0)
std = nb[:, :bwe.N_IN].std(axis=0) + 1e-3
model = bwe.Predictor(args.hidden, args.layers, mean, std, args.lookahead).to(dev)
print("parameters: %d" % sum(p.numel() for p in model.parameters()), flush=True)

target_cols = list(range(bwe.NB_BANDS)) + [bwe.CORR]
wb_t = torch.as_tensor(wb[:, target_cols])
nb_t = torch.as_tensor(nb[:, :bwe.N_IN])
L = args.lookahead


def batch():
    starts = rng.integers(0, len(wb) - args.frames - L - 1, args.batch)
    i = torch.as_tensor(starts[:, None] + np.arange(args.frames + L)[None, :])
    x = nb_t[i]
    # With lookahead, the output for frame t comes L frames later.
    y = wb_t[i[:, :args.frames]]
    return x.to(dev, non_blocking=True), y.to(dev, non_blocking=True)


def loss_fn(pred, y):
    d = pred - y
    return (d[..., :bwe.NB_BANDS] ** 2).mean() + args.corr_weight * (d[..., bwe.NB_BANDS] ** 2).mean()


def run(model, x):
    pred, _ = model(x)
    return pred[:, L:]


# A fixed set of 10 s windows of the validation set, scored after 1 s of
# warm-up (the full per-utterance comparison is eval_features.py's job).
vstarts = np.random.default_rng(2).integers(0, len(vwb) - 1000 - L, 256)
vi = torch.as_tensor(vstarts[:, None] + np.arange(1000 + L)[None, :])
vx = torch.as_tensor(vnb[:, :bwe.N_IN])[vi].to(dev)
vy = torch.as_tensor(vwb[:, target_cols])[vi[:, :1000]].to(dev)


def validate():
    model.eval()
    with torch.no_grad():
        v = loss_fn(run(model, vx)[:, 100:], vy[:, 100:]).item()
    model.train()
    return v


opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=args.steps, pct_start=0.05)
best = None
t0 = time.time()
running = 0.0
for step in range(1, args.steps + 1):
    x, y = batch()
    loss = loss_fn(run(model, x), y)
    opt.zero_grad()
    loss.backward()
    torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
    opt.step()
    sched.step()
    running += loss.item()
    if step % 1000 == 0:
        v = validate()
        print("step %d train %.5f valid %.5f (%.0f s)" % (step, running / 1000, v, time.time() - t0), flush=True)
        running = 0.0
        if best is None or v < best:
            best = v
            torch.save({"args": vars(args), "state": model.state_dict()}, args.out)
print("best valid %.5f" % best)
