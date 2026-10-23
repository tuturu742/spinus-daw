class _Model:
    segment = 7.8
    samplerate = 44100
    sources = ("drums", "bass", "other", "vocals")


def load_safetensors_model(path):
    open(path, "rb").close()  # it must be there
    return _Model()
