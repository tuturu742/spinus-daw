#pragma once

#include "InstrumentPresets.h"
#include "OfflineInstrument.h"
#include "Session.h"

namespace spinus
{

/** An instrument that nobody hears (Spinus's own, a downloaded one or an installed plugin): made by the
    session's device factory in an edit that never plays, and driven directly, offline, to record presets and
    settings. The song's own instruments are not touched. A plugin runs inside Spinus here, unless it is set
    to run in a separate process; its libraries are noted for the crash guard as the song's are.

    loadPreset() runs on the message thread; render() and the parameters may then be used from one other
    thread at a time (it is processed like an audio thread would). */
class HiddenInstrument
{
public:
    static constexpr double sampleRate = OfflineInstrument::sampleRate;
    static constexpr int blockSize = OfflineInstrument::blockSize;

    HiddenInstrument (Session&, const juce::String& stockType);
    /** Any instrument: Spinus's own, a downloaded one or an installed plugin. */
    HiddenInstrument (Session&, const model::Device&);
    ~HiddenInstrument();

    /** Empty when the instrument is ready, otherwise why not (e.g. Surge XT is not downloaded, or a plugin is not
     * installed). */
    juce::String getProblem() const { return problem; }
    juce::AudioPluginInstance* getInstance() const noexcept { return instance; }
    /** The plugin file (to find its presets); empty for Spinus's own instruments. */
    juce::File getPluginFile() const;

    /** Loads a preset file (Surge XT's .fxp, through CLAP preset-load) and waits until it is in place. */
    juce::Result loadPreset (const juce::File&);
    /** Loads any preset (a file, a state, a program or a discovered one) and waits until it is in place. */
    juce::Result loadPreset (const InstrumentPreset&);

    using Note = OfflineInstrument::Note;
    /** Plays the notes from silence and returns the sound (mono, at sampleRate), `tail` seconds after the
        last note ends. */
    juce::AudioBuffer<float> render (const std::vector<Note>&, double tail = 0.4);

private:
    juce::String problem;
    std::unique_ptr<tracktion::Edit> edit;
    tracktion::Plugin::Ptr plugin;
    juce::AudioPluginInstance* instance = nullptr;
    std::unique_ptr<OfflineInstrument> player; // plays `instance` (Spinus's and the sound index's way)
};

} // namespace spinus
