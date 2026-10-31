import numpy as np
import torch


class BagOfModels:
    def __init__(self, models, weights=None, segment=None):
        self.models = models
        self.samplerate = models[0].samplerate
        self.sources = list(models[0].sources)

    def eval(self):
        return self


def apply_model(bag, mix, shifts=1, overlap=0.25, split=True, device=None, callback=None):
    length = mix.shape[-1]
    if callback is not None:
        callback({"state": "end", "model_idx_in_bag": 0, "segment_offset": length})
    shares = np.array([0.4, 0.3, 0.2, 0.1, 0.06, 0.04])[: len(bag.sources)]
    return torch.Tensor(mix.values[:, None, :, :] * shares[None, :, None, None])
