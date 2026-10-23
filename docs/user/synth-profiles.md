# Synth profiles

The Analyzer can tune a synth until it sounds like a part of a song ("Tuned to it", see
[the Analyzer](analyzer.md)). A **synth profile** says which of the synth's settings it may change, and how
far. Spinus has profiles for **3x Osc**, **Surge XT**, **Odin 2** and **OB-Xf**.

The Analyzer is **experimental**: it shows only while **Show experimental features** is ticked in
**Options › AI...**.

Any other instrument plugin gets a profile made on the spot from the names of its settings ("Tuned to it
(experimental)"): Spinus takes up to 20 settings whose names say they shape the sound, the filter's
cutoff and resonance first, then the envelopes (attack, decay, sustain, release), then the oscillators
(wave, level, pulse width, detune), and leaves out the overall volume, tuning and transpose, voices,
on/off switches, effects (reverb, delay, chorus), the arpeggiator, LFOs and presets. Envelope times and
levels are kept in a useful range. A plugin with fewer than 3 such settings is not tuned. If the
settings Spinus picks are not the right ones for your synth, write a profile for it as below: yours is
used instead.

## Your own profile

Put a `.json` file in `~/.local/share/spinus/synth-profiles/` (make the folder if it is not there). A
profile there replaces Spinus's own for the same synth. For example, to let the tuning change only 3x
Osc's filter and envelope:

```json
{
  "name": "3x Osc, filter and envelope",
  "stockType": "spinus.3x-osc",
  "start": "default",
  "settings": [
    { "id": "cutoff" },
    { "id": "resonance", "max": 0.8 },
    { "id": "attack", "max": 0.3 },
    { "id": "decay", "max": 0.5 },
    { "id": "sustain" },
    { "id": "release", "max": 0.3 }
  ]
}
```

- **stockType**: one of Spinus's synths: `spinus.3x-osc`, `bundled.surge-xt`, `bundled.odin2` or
  `bundled.ob-xf`. Or, for a plugin installed on your computer, leave `stockType` out and write
  **plugin** instead: the plugin's name as the Browser shows it, without the "(VST3)" after it (any mix
  of capitals). Add **manufacturer** (its maker) or **format** (`VST3`, `CLAP`, `LV2`, `VST`) only if two
  plugins have the same name.
- **start**: `default` (the synth's own sound) or `closestPreset` (the synth's preset that sounds most
  like the part, for any synth with presets Spinus can choose from, as the Analyzer's Closest preset finds
  them; one without plays from its own sound).
- **settings**: each one by its `id` (3x Osc: `osc1_shape`, `osc1_level`, `cutoff`, `attack`...) or its
  `name` as the synth shows it (Surge XT: `"A Filter 1 Cutoff"`, `"A Amp EG Attack"`...; for a plugin, the
  names its settings have in the automation lists). `min` and `max` (0 to 1, the synth's whole range)
  keep it inside part of its range; `steps` makes it a choice (3x Osc's oscillator shape has 5).
- **tries** (optional): how many sounds the tuning may try (Surge XT 800, 3x Osc 500, the others 600,
  each about half a minute).

A profile for an installed plugin, for example:

```json
{
  "name": "My synth",
  "plugin": "Some Synth",
  "settings": [
    { "name": "Filter Cutoff" },
    { "name": "Filter Resonance", "max": 0.8 },
    { "name": "Amp Attack", "max": 0.4 },
    { "name": "Amp Release", "max": 0.5 },
    { "name": "Osc 1 Wave", "steps": 4 }
  ]
}
```

Fewer settings tune faster. Settings the synth does not have are skipped. Spinus reads the profiles each
time it tunes, so a changed profile counts from the next "Add to the project".

Tuning a plugin Spinus has no profile of its own for is experimental, with your profile or without: the
plugin is tuned inside Spinus, so a plugin that crashes takes Spinus with it. Save your project first.
