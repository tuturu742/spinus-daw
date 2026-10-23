"""A stand-in for Hugging Face transformers' CLAP audio tower (closest preset): a sound's
"embedding" is its spectrum in 64 bands, so alike sounds still come out alike."""

import numpy as np
import torch


class _Config:
    audio_config = object()


class ClapConfig:
    @staticmethod
    def from_pretrained(folder):
        return _Config()


class _Output:
    def __init__(self, embeds):
        self.audio_embeds = embeds


class ClapAudioModelWithProjection:
    def __init__(self, config):
        pass

    def load_state_dict(self, state, strict=True):
        assert set(state) == {"audio_model.fake", "audio_projection.fake"}

    def eval(self):
        return self

    def to(self, device):
        return self

    def __call__(self, input_features):
        return _Output(input_features)


class ClapFeatureExtractor:
    @staticmethod
    def from_pretrained(folder):
        return ClapFeatureExtractor()

    def __call__(self, audio, sampling_rate, return_tensors):
        rows = []
        for clip in audio:
            spectrum = np.abs(np.fft.rfft(np.asarray(clip, dtype=float), n=8192))
            bands = np.array_split(np.log1p(spectrum[:4096]), 64)
            rows.append([b.mean() for b in bands])
        return {"input_features": torch.Tensor(np.array(rows) + 1e-6)}
