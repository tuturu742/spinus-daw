#pragma once

#include "HiddenInstrument.h"

namespace spinus
{

/** Short recordings of an instrument's presets (for finding the closest preset): each preset plays the same
    phrase (C2, C3 and C4, nearly a second each), so the AI service can compare how presets sound with a
    stem. A hidden instrument of its own does the playing, in an edit that never plays, on the message
    thread a few presets at a time; the song's own instruments are not touched.

    Presets are anything InstrumentPresets finds: files the instrument loads itself (Surge XT's .fxp
    patches, through CLAP preset-load), states (OB-Xf's patches, Odin 2's built-in presets), programs and
    presets listed through CLAP preset discovery. */
class PresetPreviews : private juce::Timer
{
public:
    static constexpr double sampleRate = HiddenInstrument::sampleRate;

    /** Makes the hidden instrument (a stock type such as "bundled.surge-xt"). */
    PresetPreviews (Session&, const juce::String& stockType);
    /** Any instrument: Spinus's own, a downloaded one or an installed plugin. */
    PresetPreviews (Session&, const model::Device&);
    ~PresetPreviews() override;

    /** Empty when the instrument is ready, otherwise why not (e.g. Surge XT is not downloaded). */
    juce::String getProblem() const { return instrument.getProblem(); }
    /** The instrument's plugin file (to find its presets). */
    juce::File getPluginFile() const { return instrument.getPluginFile(); }
    /** The hidden instrument's plugin (to list its presets); nullptr when there is a problem. */
    juce::AudioPluginInstance* getInstance() const noexcept { return instrument.getInstance(); }

    struct PhraseNote
    {
        int note = 60;
        double start = 0.0, length = 1.0; // seconds
    };
    /** C2, C3 and C4, 0.9 s each, a second apart. */
    static const std::vector<PhraseNote>& getPhrase();

    /** Plays one preset's phrase (or these notes) into a mono 16-bit WAV file, with half a second of tail. */
    juce::Result renderOne (const juce::File& preset, const juce::File& output,
                            const std::vector<PhraseNote>& notes = getPhrase());
    juce::Result renderOne (const InstrumentPreset& preset, const juce::File& output,
                            const std::vector<PhraseNote>& notes = getPhrase());
    /** The same with the instrument's sound as it is now (no preset loaded first). */
    juce::Result renderAsIs (const juce::File& output, const std::vector<PhraseNote>& notes = getPhrase());

    struct Item
    {
        InstrumentPreset preset;
        juce::File output;
    };

    /** Renders the items a few per timer tick; `progress` gets (done, total) and `finished` the result
        (stopping by cancel() is not an error). */
    void render (std::vector<Item>, std::function<void (int, int)> progress,
                 std::function<void (juce::Result)> finished);
    void cancel();
    bool isRendering() const noexcept { return isTimerRunning(); }

private:
    void timerCallback() override;

    HiddenInstrument instrument;

    std::vector<Item> queue;
    size_t next = 0;
    std::function<void (int, int)> onProgress;
    std::function<void (juce::Result)> onFinished;
};

/** Loads a preset into a channel's instrument, as its own preset browser does (a file through CLAP
    preset-load, a state, a program or a discovered preset): the preset becomes part of the instrument's saved
    state. */
juce::Result loadInstrumentPreset (Session&, model::ChannelId, const InstrumentPreset&);

} // namespace spinus
