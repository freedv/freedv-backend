"""Shared pieces of the 8 kHz-mic feature predictor: the feature layout, the
model and the data."""
import numpy as np
import torch
from torch import nn

NB_TOTAL_FEATURES = 36
NB_BANDS = 18
PITCH = NB_BANDS        # pitch period, as lpcnet's dnn_pitch
CORR = NB_BANDS + 1     # pitch correlation - 0.5
N_IN = NB_BANDS + 2     # what RADE's encoder uses
N_OUT = NB_BANDS + 1    # what the model corrects: the bands and the correlation

# Band centres in Hz (eband5ms * 200).
BAND_HZ = np.array([0, 200, 400, 600, 800, 1000, 1200, 1400, 1600, 2000, 2400, 2800, 3200, 4000, 4800, 5600, 6800, 8000])


def dct_matrix():
    """lpcnet's dct_table as a matrix D, with cepstrum = D @ log10 band energies."""
    n = np.arange(NB_BANDS)
    d = np.cos(np.outer(n, n + 0.5) * np.pi / NB_BANDS) * np.sqrt(2.0 / NB_BANDS)
    d[0] *= np.sqrt(0.5)
    return d


DCT = dct_matrix()


def bands_db(features):
    """Log band energies in dB from the first 18 features of each frame."""
    c = np.array(features[..., :NB_BANDS], dtype=np.float64)
    c[..., 0] += 4
    return 10 * (c @ DCT)


class Predictor(nn.Module):
    """Guesses the wideband features from the narrowband ones, frame by frame
    and causally (lookahead frames of delay, if any, are added by the caller)."""

    def __init__(self, hidden=128, layers=2, mean=None, std=None, lookahead=0):
        super().__init__()
        self.lookahead = lookahead
        self.register_buffer("mean", torch.zeros(N_IN) if mean is None else torch.as_tensor(mean, dtype=torch.float32))
        self.register_buffer("std", torch.ones(N_IN) if std is None else torch.as_tensor(std, dtype=torch.float32))
        self.conv = nn.Conv1d(N_IN, hidden, 3)
        self.gru = nn.GRU(hidden, hidden, layers, batch_first=True)
        self.out = nn.Linear(hidden, N_OUT)

    def forward(self, nb, state=None):
        """nb: (batch, frames, N_IN) narrowband features. Returns the
        predicted (batch, frames, N_OUT) and the GRU state. The convolution
        sees the two frames before each one, padded with the first. With
        lookahead L, output t is the guess for input frame t - L."""
        x = (nb[..., :N_IN] - self.mean) / self.std
        x = x.transpose(1, 2)
        x = torch.cat([x[..., :1], x[..., :1], x], dim=2)
        x = torch.tanh(self.conv(x)).transpose(1, 2)
        y, state = self.gru(x, state)
        base = torch.cat([nb[..., :NB_BANDS], nb[..., CORR:CORR + 1]], dim=-1)
        if self.lookahead:
            base = torch.cat([base[:, :1].expand(-1, self.lookahead, -1), base[:, :-self.lookahead]], dim=1)
        return base + self.out(y), state


def apply(model, nb, device="cpu"):
    """Runs the model over a whole utterance's features (frames, 36) and
    returns them with the bands and correlation replaced."""
    L = model.lookahead
    with torch.no_grad():
        x = torch.as_tensor(nb[None, :, :N_IN], dtype=torch.float32, device=device)
        x = torch.cat([x, x[:, -1:].expand(1, L, -1)], dim=1)
        y = model(x)[0][0, L:].cpu().numpy()
    out = np.array(nb, dtype=np.float32)
    out[:, :NB_BANDS] = y[:, :NB_BANDS]
    out[:, CORR] = y[:, NB_BANDS]
    return out


def load_pairs(prefix):
    wb = np.fromfile(prefix + ".wb.f32", dtype=np.float32).reshape(-1, NB_TOTAL_FEATURES)
    nb = np.fromfile(prefix + ".nb.f32", dtype=np.float32).reshape(-1, NB_TOTAL_FEATURES)
    idx = np.fromfile(prefix + ".idx", dtype=np.int64)
    return wb, nb, idx


def speech_frames(wb, margin_db=30):
    """Frames with speech: within margin_db of the utterance set's loud frames."""
    e = bands_db(wb).max(axis=-1)
    return e > np.percentile(e, 99) - margin_db
