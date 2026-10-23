"""BS-RoFormer, for separating a song into many instruments (the MVSep Mega 53 stems model).

Inference only, from Music-Source-Separation-Training's models/bs_roformer (MIT, Copyright (c) 2024 Roman
Solovyev (ZFTurbo); itself after Phil Wang's BS-RoFormer, MIT): the same modules and parameter names, so
the published checkpoints load, without the training losses, gradient checkpointing, linear attention and
PoPE (the model does not use them) and without beartype. Runs in the analysis engine's environment
(PyTorch, einops and rotary-embedding-torch, which Beat This! already needs).
"""

from __future__ import annotations

import itertools
from typing import Any

import torch
import torch.nn.functional as F
from einops import pack, rearrange, unpack
from rotary_embedding_torch import RotaryEmbedding
from torch import nn


class RMSNorm(nn.Module):
    def __init__(self, dim: int) -> None:
        super().__init__()
        self.scale = dim**0.5
        self.gamma = nn.Parameter(torch.ones(dim))

    def forward(self, x: Any) -> Any:
        return F.normalize(x, dim=-1) * self.scale * self.gamma


class FeedForward(nn.Module):
    def __init__(self, dim: int, mult: int = 4) -> None:
        super().__init__()
        inner = int(dim * mult)
        self.net = nn.Sequential(
            RMSNorm(dim), nn.Linear(dim, inner), nn.GELU(), nn.Dropout(0.0), nn.Linear(inner, dim), nn.Dropout(0.0)
        )

    def forward(self, x: Any) -> Any:
        return self.net(x)


class Attention(nn.Module):
    def __init__(self, dim: int, heads: int, dim_head: int, rotary_embed: Any) -> None:
        super().__init__()
        self.heads = heads
        inner = heads * dim_head
        self.rotary_embed = rotary_embed
        self.norm = RMSNorm(dim)
        self.to_qkv = nn.Linear(dim, inner * 3, bias=False)
        self.to_gates = nn.Linear(dim, heads)
        self.to_out = nn.Sequential(nn.Linear(inner, dim, bias=False), nn.Dropout(0.0))

    def forward(self, x: Any) -> Any:
        x = self.norm(x)
        q, k, v = rearrange(self.to_qkv(x), "b n (qkv h d) -> qkv b h n d", qkv=3, h=self.heads)
        q = self.rotary_embed.rotate_queries_or_keys(q)
        k = self.rotary_embed.rotate_queries_or_keys(k)
        out = F.scaled_dot_product_attention(q, k, v)
        gates = self.to_gates(x)
        out = out * rearrange(gates, "b n h -> b h n 1").sigmoid()
        return self.to_out(rearrange(out, "b h n d -> b n (h d)"))


class Transformer(nn.Module):
    def __init__(self, *, dim: int, depth: int, dim_head: int, heads: int, rotary_embed: Any) -> None:
        super().__init__()
        self.layers = nn.ModuleList(
            [nn.ModuleList([Attention(dim, heads, dim_head, rotary_embed), FeedForward(dim)]) for _ in range(depth)]
        )
        self.norm = nn.Identity()

    def forward(self, x: Any) -> Any:
        for attn, ff in self.layers:
            x = attn(x) + x
            x = ff(x) + x
        return self.norm(x)


class BandSplit(nn.Module):
    def __init__(self, dim: int, dim_inputs: tuple[int, ...]) -> None:
        super().__init__()
        self.dim_inputs = dim_inputs
        self.to_features = nn.ModuleList([nn.Sequential(RMSNorm(d), nn.Linear(d, dim)) for d in dim_inputs])

    def forward(self, x: Any) -> Any:
        parts = x.split(self.dim_inputs, dim=-1)
        return torch.stack([f(p) for p, f in zip(parts, self.to_features, strict=True)], dim=-2)


def mlp(dim_in: int, dim_out: int, dim_hidden: int, depth: int) -> nn.Sequential:
    dims = (dim_in, *((dim_hidden,) * (depth - 1)), dim_out)
    layers: list[nn.Module] = []
    for i, (a, b) in enumerate(itertools.pairwise(dims)):
        layers.append(nn.Linear(a, b))
        if i < len(dims) - 2:
            layers.append(nn.Tanh())
    return nn.Sequential(*layers)


class MaskEstimator(nn.Module):
    def __init__(self, dim: int, dim_inputs: tuple[int, ...], depth: int, mlp_expansion_factor: int) -> None:
        super().__init__()
        hidden = dim * mlp_expansion_factor
        self.to_freqs = nn.ModuleList(
            [nn.Sequential(mlp(dim, d * 2, hidden, depth), nn.GLU(dim=-1)) for d in dim_inputs]
        )

    def forward(self, x: Any) -> Any:
        return torch.cat([f(band) for band, f in zip(x.unbind(dim=-2), self.to_freqs, strict=True)], dim=-1)


class BSRoformer(nn.Module):
    """Takes [batch, channels, samples] at 44.1 kHz; gives [batch, stems, channels, samples]."""

    def __init__(
        self,
        dim: int,
        *,
        depth: int,
        stereo: bool = False,
        num_stems: int = 1,
        time_transformer_depth: int = 2,
        freq_transformer_depth: int = 2,
        freqs_per_bands: tuple[int, ...] = (),
        dim_head: int = 64,
        heads: int = 8,
        stft_n_fft: int = 2048,
        stft_hop_length: int = 512,
        stft_win_length: int = 2048,
        stft_normalized: bool = False,
        mask_estimator_depth: int = 2,
        mlp_expansion_factor: int = 4,
        **unused: Any,  # training settings in the model's configuration
    ) -> None:
        super().__init__()
        if unused.get("linear_transformer_depth", 0) or unused.get("use_pope") or unused.get("skip_connection"):
            raise ValueError("This BS-RoFormer variant is not supported.")
        self.audio_channels = 2 if stereo else 1
        self.num_stems = num_stems
        time_rotary = RotaryEmbedding(dim=dim_head)
        freq_rotary = RotaryEmbedding(dim=dim_head)
        common = {"dim": dim, "heads": heads, "dim_head": dim_head}
        self.layers = nn.ModuleList(
            [
                nn.ModuleList(
                    [
                        Transformer(depth=time_transformer_depth, rotary_embed=time_rotary, **common),
                        Transformer(depth=freq_transformer_depth, rotary_embed=freq_rotary, **common),
                    ]
                )
                for _ in range(depth)
            ]
        )
        self.final_norm = RMSNorm(dim)
        self.stft_kwargs = {
            "n_fft": stft_n_fft,
            "hop_length": stft_hop_length,
            "win_length": stft_win_length,
            "normalized": stft_normalized,
        }
        self.win_length = stft_win_length
        if sum(freqs_per_bands) != stft_n_fft // 2 + 1:
            raise ValueError("The bands do not cover the spectrum.")
        with_complex = tuple(2 * f * self.audio_channels for f in freqs_per_bands)
        self.band_split = BandSplit(dim, with_complex)
        self.mask_estimators = nn.ModuleList(
            [MaskEstimator(dim, with_complex, mask_estimator_depth, mlp_expansion_factor) for _ in range(num_stems)]
        )

    def forward(self, raw_audio: Any) -> Any:
        device = raw_audio.device
        raw_audio, shape = pack([raw_audio], "* t")
        window = torch.hann_window(self.win_length, device=device)
        stft = torch.view_as_real(torch.stft(raw_audio, **self.stft_kwargs, window=window, return_complex=True))
        stft = unpack(stft, shape, "* f t c")[0]
        stft = rearrange(stft, "b s f t c -> b (f s) t c")
        x = self.band_split(rearrange(stft, "b f t c -> b t (f c)"))

        for time_transformer, freq_transformer in self.layers:
            x = rearrange(x, "b t f d -> b f t d")
            x, ps = pack([x], "* t d")
            x = time_transformer(x)
            x = unpack(x, ps, "* t d")[0]
            x = rearrange(x, "b f t d -> b t f d")
            x, ps = pack([x], "* f d")
            x = freq_transformer(x)
            x = unpack(x, ps, "* f d")[0]
        x = self.final_norm(x)

        mask = torch.stack([estimator(x) for estimator in self.mask_estimators], dim=1)
        mask = torch.view_as_complex(rearrange(mask, "b n t (f c) -> b n f t c", c=2).contiguous())
        stft = torch.view_as_complex(rearrange(stft, "b f t c -> b 1 f t c").contiguous())
        separated = rearrange(stft * mask, "b n (f s) t -> (b n s) f t", s=self.audio_channels)
        separated[:, 0] = 0.0  # no DC
        audio = torch.istft(separated, **self.stft_kwargs, window=window, return_complex=False,
                            length=raw_audio.shape[-1])  # fmt: skip
        return rearrange(audio, "(b n s) t -> b n s t", s=self.audio_channels, n=self.num_stems)


def demix(model: BSRoformer, mix: Any, chunk: int, overlap: int, device: str, progress: Any) -> Any:
    """The model over a whole song [channels, samples] in overlapping chunks, crossfaded (as
    Music-Source-Separation-Training's demix does): [stems, channels, samples], float32 on the CPU."""
    step = chunk // overlap
    fade = chunk // 10
    border = chunk - step
    length = mix.shape[-1]
    if length > 2 * border and border > 0:
        mix = F.pad(mix[None], (border, border), mode="reflect")[0]
    ramp_in = torch.linspace(0, 1, fade)
    window = torch.ones(chunk)
    window[:fade] = ramp_in
    window[-fade:] = ramp_in.flip(0)

    total = mix.shape[-1]
    result = torch.zeros((model.num_stems, mix.shape[0], total), dtype=torch.float32)
    counter = torch.zeros(total, dtype=torch.float32)
    starts = list(range(0, total, step))
    for n, start in enumerate(starts):
        part = mix[:, start : start + chunk]
        size = part.shape[-1]
        if size < chunk:
            mode = "reflect" if size > chunk // 2 + 1 else "constant"
            part = F.pad(part[None], (0, chunk - size), mode=mode)[0]
        with torch.no_grad():
            out = model(part[None].to(device))[0].float().cpu()
        w = window.clone()
        if start == 0:
            w[:fade] = 1
        if start + step >= total:
            w[-fade:] = 1
        result[..., start : start + size] += out[..., :size] * w[:size]
        counter[start : start + size] += w[:size]
        progress((n + 1) / len(starts))
    result /= counter.clamp(min=1e-8)
    if length > 2 * border and border > 0:
        result = result[..., border : border + length]
    return result
