"""The Inverse Drum Machine's transcription, for telling a drum stem's hits apart (kick, snare, hats, cymbals, toms).

Inference only, from the Inverse Drum Machine (https://github.com/bernardo-torres/inverse-drum-machine, Apache-2.0,
Copyright (c) 2025 Bernardo Torres; B. Torres, G. Peeters and G. Richard, "The Inverse Drum Machine: Source
Separation Through Joint Transcription and Analysis-by-Synthesis", IEEE TASLP 2025): its encoder's log-mel front
end, ConvNeXt backbone (itself after Meta's ConvNeXt, MIT) and transcription head, with the same parameter names so
the published checkpoint loads. Not taken: the drum-kit classifier, the one-shot synthesizer and the sequencer
(they make the separated drum sounds, which Spinus does not use), training and hydra. The mel filters and the
window come from the checkpoint, so torchaudio is not needed. Runs in the analysis engine's environment.
"""

from __future__ import annotations

from typing import Any

import torch
import torch.nn.functional as F
from torch import nn

CLASSES = ("CY_CR", "CY_RD", "HH_CHH", "HH_OHH", "KD", "SD", "TT_HFT", "TT_HMT", "TT_LMT")
RATE = 44100
HOP = 256
N_FFT = 1024
FRAME_RATE = RATE / HOP


class LayerNorm(nn.Module):
    """Layer norm over the channels of (N, C, H, W) ("channels_first") or the last axis."""

    def __init__(self, dim: int, eps: float = 1e-6, channels_first: bool = False) -> None:
        super().__init__()
        self.weight = nn.Parameter(torch.ones(dim))
        self.bias = nn.Parameter(torch.zeros(dim))
        self.eps = eps
        self.channels_first = channels_first

    def forward(self, x: Any) -> Any:
        if not self.channels_first:
            return F.layer_norm(x, (x.shape[-1],), self.weight, self.bias, self.eps)
        u = x.mean(1, keepdim=True)
        s = (x - u).pow(2).mean(1, keepdim=True)
        x = (x - u) / torch.sqrt(s + self.eps)
        return self.weight[:, None, None] * x + self.bias[:, None, None]


class Block(nn.Module):
    def __init__(self, dim: int) -> None:
        super().__init__()
        self.dwconv = nn.Conv2d(dim, dim, kernel_size=7, padding=3, groups=dim)
        self.norm = LayerNorm(dim)
        self.pwconv1 = nn.Linear(dim, 4 * dim)
        self.act = nn.GELU()
        self.pwconv2 = nn.Linear(4 * dim, dim)
        self.gamma = nn.Parameter(torch.ones(dim))

    def forward(self, x: Any) -> Any:
        y = self.dwconv(x).permute(0, 2, 3, 1)
        y = self.gamma * self.pwconv2(self.act(self.pwconv1(self.norm(y))))
        return x + y.permute(0, 3, 1, 2)


class ConvNeXt(nn.Module):
    """Downsamples the mel bands only (not time): one feature vector per frame."""

    def __init__(self, dims: tuple[int, ...] = (4, 8, 16, 32), depths: tuple[int, ...] = (3, 3, 9, 3),
                 kernel_size: int = 7) -> None:  # fmt: skip
        super().__init__()
        padding = (0, (kernel_size - 1) // 2)
        self.downsample_layers = nn.ModuleList(
            [
                nn.Sequential(
                    nn.Conv2d(1, dims[0], kernel_size=kernel_size, stride=(4, 1), padding=padding),
                    LayerNorm(dims[0], channels_first=True),
                )
            ]
        )
        for i in range(3):
            self.downsample_layers.append(
                nn.Sequential(
                    LayerNorm(dims[i], channels_first=True),
                    nn.Conv2d(
                        dims[i], dims[i + 1], kernel_size=max(3, kernel_size // 2), stride=(2, 1), padding=padding
                    ),
                )
            )
        self.stages = nn.ModuleList([nn.Sequential(*[Block(dims[i]) for _ in range(depths[i])]) for i in range(4)])
        self.norm = nn.LayerNorm(dims[-1], eps=1e-6)

    def forward(self, x: Any) -> Any:
        x = x.unsqueeze(1)
        for i in range(4):
            x = self.stages[i](self.downsample_layers[i](x))
        return self.norm(x.mean(-2).permute(0, 2, 1))  # (batch, frames, features)


class TranscriptionHead(nn.Module):
    def __init__(self, dim: int = 32, classes: int = len(CLASSES), kernel_size: int = 7) -> None:
        super().__init__()
        self.conv = nn.Conv1d(dim, dim, kernel_size, padding="same")
        self.velocity_mlp = nn.Linear(dim, classes)
        self.onset_mlp = nn.Linear(dim, classes)

    def forward(self, x: Any) -> tuple[Any, Any]:
        x = F.relu(self.conv(x.permute(0, 2, 1)).permute(0, 2, 1))
        onset = torch.sigmoid(self.onset_mlp(x)).permute(0, 2, 1)
        # exp_sigmoid (the original's velocity activation): 2 * sigmoid(v) ** log(10) + 1e-7
        velocity = (2.0 * torch.sigmoid(self.velocity_mlp(x)) ** 2.302585092994046 + 1e-7).permute(0, 2, 1)
        return onset, velocity


class MelSpectrogram(nn.Module):
    """torchaudio's MelSpectrogram (power 2, centred, reflected at the ends), with the window and the filters
    loaded from the checkpoint; then the log."""

    def __init__(self) -> None:
        super().__init__()
        self.window = nn.Parameter(torch.zeros(N_FFT), requires_grad=False)
        self.fb = nn.Parameter(torch.zeros(N_FFT // 2 + 1, 128), requires_grad=False)

    def forward(self, x: Any) -> Any:
        x = F.pad(x, (0, (-x.shape[-1]) % HOP))
        spec = torch.stft(x, N_FFT, HOP, N_FFT, self.window, center=True, pad_mode="reflect", return_complex=True)
        mel = torch.matmul(spec.abs().pow(2).transpose(-1, -2), self.fb).transpose(-1, -2)
        return torch.log(mel + 1e-6)


class DrumTranscriber(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.mel = MelSpectrogram()
        self.backbone = ConvNeXt()
        self.transcription_head = TranscriptionHead()

    def forward(self, audio: Any) -> tuple[Any, Any]:
        return self.transcription_head(self.backbone(self.mel(audio)))  # type: ignore[no-any-return]


def load(checkpoint: str, device: str) -> DrumTranscriber:
    """The transcriber from the published checkpoint (a Lightning pickle: read with weights_only=True, after
    its hash was checked). Only the encoder's weights are used."""
    state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    state = state.get("state_dict", state)
    renamed = {
        "encoder.transform.mel_transform.spectrogram.window": "mel.window",
        "encoder.transform.mel_transform.mel_scale.fb": "mel.fb",
    }
    mine = {}
    for key, value in state.items():
        if key in renamed:
            mine[renamed[key]] = value
        elif key.startswith(("encoder.backbone.", "encoder.transcription_head.")):
            mine[key.removeprefix("encoder.")] = value
    model = DrumTranscriber()
    model.load_state_dict(mine, strict=True)
    return model.to(device).eval()


def activations(model: DrumTranscriber, mono: Any, device: str, chunk_seconds: float = 10.0) -> tuple[Any, Any]:
    """Onset probabilities and velocities (each classes x frames, at FRAME_RATE) of mono audio at RATE: 10-second
    pieces with a second more on each side, so hits at their edges are heard whole."""
    import numpy as np

    x = torch.from_numpy(np.asarray(mono, dtype=np.float32))
    chunk, pad = int(chunk_seconds * RATE), RATE
    onsets, velocities = [], []
    with torch.no_grad():
        for start in range(0, len(x), chunk):
            a, b = max(0, start - pad), min(len(x), start + chunk + pad)
            onset, velocity = model(x[a:b].unsqueeze(0).to(device))
            first = round((start - a) / HOP)
            count = round(min(chunk, len(x) - start) / HOP)
            onsets.append(onset[0, :, first : first + count].cpu().numpy())
            velocities.append(velocity[0, :, first : first + count].cpu().numpy())
    if not onsets:
        empty = np.zeros((len(CLASSES), 0), dtype=np.float32)
        return empty, empty
    return np.concatenate(onsets, axis=1), np.concatenate(velocities, axis=1)
