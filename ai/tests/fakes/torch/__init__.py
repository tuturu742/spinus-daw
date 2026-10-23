"""A stand-in for PyTorch: enough for the worker's self-test, with NumPy doing the arithmetic.

FAKE_GPU says what the "GPU" does: "ok", "wrong" (bad results), "crash" (dies), or "none" (no GPU).
"""

import os
import signal

import numpy as np

__version__ = "2.14.0+fake"
_threads = [8]
_gpu = os.environ.get("FAKE_GPU", "none")


class version:
    hip = "7.16" if _gpu != "none" else None
    cuda = None
    xpu = None


class backends:
    class mkl:
        @staticmethod
        def is_available():
            return False


class cuda:
    @staticmethod
    def is_available():
        return _gpu != "none"

    @staticmethod
    def get_device_name(index):
        return "Fake Radeon"


class Generator:
    def manual_seed(self, seed):
        self.random = np.random.default_rng(seed)
        return self


def _v(x):
    return x.values if isinstance(x, Tensor) else x


class Tensor:
    """Enough of a tensor for the worker: NumPy underneath."""

    def __init__(self, values):
        self.values = np.asarray(values)

    shape = property(lambda self: self.values.shape)

    def __matmul__(self, other):
        return Tensor(self.values @ other.values)

    def __sub__(self, other):
        return Tensor(self.values - _v(other))

    def __add__(self, other):
        return Tensor(self.values + _v(other))

    def __mul__(self, other):
        return Tensor(self.values * _v(other))

    def __truediv__(self, other):
        return Tensor(self.values / _v(other))

    def __getitem__(self, index):
        return Tensor(self.values[index])

    def __iter__(self):
        return (Tensor(v) for v in self.values)

    def __len__(self):
        return len(self.values)

    def repeat(self, *counts):
        return Tensor(np.tile(self.values, counts))

    def mean(self, dim=None):
        return Tensor(self.values.mean(axis=dim))

    def std(self):
        return Tensor(self.values.std())

    def numpy(self):
        return self.values

    def abs(self):
        return Tensor(np.abs(self.values))

    def max(self):
        return float(self.values.max())

    def __float__(self):
        return float(self.values)

    def to(self, device):
        if _gpu == "crash":
            os.kill(os.getpid(), signal.SIGSEGV)
        return Tensor(self.values + (1.0 if _gpu == "wrong" else 0.0))

    def cpu(self):
        return self


def from_numpy(values):
    return Tensor(values)


def tensor(value):
    return Tensor(value)


class no_grad:
    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def randn(*shape, generator):
    return Tensor(generator.random.standard_normal(shape))


def set_num_threads(count):
    _threads[0] = count


def get_num_threads():
    return _threads[0]


def load(path, map_location=None, weights_only=False):
    """The weights of the fake CLAP (fakes/transformers): only checks that the file is there."""
    assert weights_only, "pickled weights must be loaded with weights_only=True"
    with open(path, "rb"):
        pass
    return {"audio_model.fake": Tensor(np.zeros(1)), "audio_projection.fake": Tensor(np.zeros(1))}
