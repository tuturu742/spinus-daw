class _Model:
    segment = 7.8
    samplerate = 44100

    def __init__(self, sources):
        self.sources = sources


def load_safetensors_model(path):
    open(path, "rb").close()  # it must be there
    # htdemucs_6s's weights file: six stems, as the real model; any other: the 4-stem htdemucs.
    if "5c90dfd2" in str(path):
        return _Model(("drums", "bass", "other", "vocals", "guitar", "piano"))
    return _Model(("drums", "bass", "other", "vocals"))
