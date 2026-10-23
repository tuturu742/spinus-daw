# Audio settings (F10)

Open with **F10** or the **⚙** button in the top bar.

| Setting | What it means |
|---|---|
| **Sound system** | How Spinus talks to the sound card. On a modern Arch system with PipeWire, Spinus uses **JACK** when the `pipewire-jack` package is installed (the lowest latency), and otherwise **ALSA** through PipeWire. On the very first start Spinus picks this for you. |
| **Play through** | What plays Spinus's sound. "PipeWire Sound Server" (or "PipeWire: the system's default output") follows your desktop's choice of speakers or headphones. **Test** plays a short beep. |
| **Speakers on** | Only when the device has more than two outputs: which pair Spinus plays on. **Outputs 1 + 2** is the usual. |
| **Record from** | What [Record audio](audio-tracks.md#recording-audio) and [Record idea](piano-roll.md#record-an-idea) record. With PipeWire (the usual on Arch), the top of the list has **PipeWire: the system's default input** and every microphone by its name (for example "fifine Microphone Analog Stereo", "Webcam C270 Mono", the laptop's "Digital Microphone"): choose one and Spinus records from it without changing the system's default. Below, **Directly through ALSA** lists the sound cards themselves; a USB microphone there usually fails because PipeWire is using it, so choose it by name above. Or **Nothing**. |
| **Microphone on** | The input your microphone or instrument is plugged into. The bar beside it shows its level: talk or play and it should move (red means too loud). |
| **Sample rate** | 48 kHz is the default and what PipeWire uses. |
| **Buffer size** | How much sound is prepared at a time. 256 samples is the safe default; 128 responds quicker, for playing an instrument live. If you hear crackles or dropouts, choose a larger buffer. |

The line under the settings shows what they mean: the device, the buffer in milliseconds, the total
output latency, and how many dropouts have happened since the device was opened.

Spinus remembers your choice in `~/.config/Spinus DAW/Settings.xml`.

**Lower latency on Arch:** installing `pipewire-jack` (it replaces `jack2`) lets Spinus use PipeWire's
JACK interface, which has less latency than the ALSA path:

```sh
sudo pacman -S pipewire-jack
```

Then choose **JACK** as the device type here.
