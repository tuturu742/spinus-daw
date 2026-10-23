import struct


def encode_wav(wav, path, samplerate, bits_per_sample=16, as_float=False):
    values = wav.values.astype("<f4")
    data = values.T.tobytes()
    channels = values.shape[0]
    fmt = struct.pack("<HHIIHH", 3, channels, samplerate, samplerate * channels * 4, channels * 4, 32)
    with open(path, "wb") as out:
        out.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt)
        out.write(b"data" + struct.pack("<I", len(data)) + data)
