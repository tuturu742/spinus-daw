#pragma once

#include <juce_core/juce_core.h>

namespace spinus
{

/** The sound index's helper process (spinus-plugin-scanner --sound-index): what is measured runs here, out of
    Spinus's process, so a plugin or an audio file that crashes or hangs cannot take Spinus down
    (SoundIndexer starts it, watches it and kills it when it hangs).

    `request` is a JSON file:
    - `{"mode": "instrument", "format", "file", "name", "uniqueId", "stockType", "known": {presetId: version},
      "skip": [presetId]}`: loads the instrument (format "Spinus": one of Spinus's own, by identifier), finds
      its presets (InstrumentPresets), and plays the test notes with each preset not known at that version
      and not skipped (sounds::describeInstrument). An instrument without presets is measured as it is,
      as the preset "" (named "Default sound").
    - `{"mode": "samples", "files": [path]}`: reads each audio file and measures it (sounds::describeSample).

    Two more modes play and tune instruments for the Track creator (out of Spinus for the same reason): `{"mode":
    "render"}` and `{"mode": "tune"}` (SoundRender.cpp says what they take and write).

    `output` gets one JSON object per line, flushed as it goes: `{"presets": [{id, version}], "presetFiles"}`
    once the instrument is loaded and its presets found; `{"begin": id or path}` before each one is
    measured (so a crash names it); `{"sound": {...}}` or `{"sample": {...}}` after; `{"error": text}` when
    the instrument cannot be loaded; `{"done": true}` at the end. Returns the process's exit code. */
int runSoundIndexProcess (const juce::File& request, const juce::File& output);

} // namespace spinus
