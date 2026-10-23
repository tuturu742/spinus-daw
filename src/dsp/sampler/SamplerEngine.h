#pragma once

#include "SampleSet.h"
#include "dsp/NoteExpression.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <span>
#include <atomic>

namespace spinus::dsp
{

/** The Sampler's sound settings that apply to every zone (the knobs; automatable). */
struct SamplerSettings
{
    float gainDb = 0.0f;
    float attack = 0.001f, decay = 0.3f, sustain = 1.0f, release = 0.05f; // seconds, sustain 0..1
    enum class Filter
    {
        off,
        lowPass,
        highPass,
        bandPass
    };
    Filter filter = Filter::off;
    float cutoffHz = 20000.0f;
    float resonance = 0.0f;           // 0..1
    float velocitySensitivity = 1.0f; // 0: every note at full level; 1: level follows velocity
    float coarse = 0.0f, fine = 0.0f; // semitones, cents
    float pan = 0.0f;                 // -1 (left) .. 1 (right)
    bool oneShot = false;             // ignore note-offs: each note plays to the end of its sample
};

/** Plays a SampleSet: up to 32 voices, each a zone at a pitch with an ADSR envelope and a filter. Each
    zone uses the settings at its settingsIndex (the Sampler has one set of settings; the Drum Machine
    one per pad).

    Real-time safe: render() does not allocate, lock or block. The set is passed in by the owner each
    block (a pointer the owner keeps alive while it is in use); when it changes, playing voices stop
    with a short fade, since their zones belong to the old set.
*/
class SamplerEngine
{
public:
    static constexpr int maxVoices = 32;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Renders `numSamples` into the (stereo or mono) buffer, adding the voices, with the MIDI events
        at their sample positions. `set` may be nullptr (silence; notes are ignored). */
    void render (juce::AudioBuffer<float>&, const juce::MidiBuffer&, const SampleSet* set,
                 std::span<const SamplerSettings> settings, int startSample, int numSamples) noexcept;

    int getNumActiveVoices() const noexcept;

    /** Handles a MIDI message at the start of the next render (e.g. a pad clicked in a window). */
    void handleNow (const juce::MidiMessage& m, const SampleSet* set,
                    std::span<const SamplerSettings> settings) noexcept
    {
        useSet (set, fadeSamples);
        handle (m, set, settings);
    }

private:
    struct Voice
    {
        const SampleZone* zone = nullptr;
        int note = -1, channel = 1;
        double position = 0.0, increment = 1.0;
        float level = 1.0f;
        bool held = false;
        int fadeOut = 0; // > 0: being stolen or cut, samples left of the fade
        uint64_t age = 0;
        juce::ADSR envelope;
        std::array<juce::dsp::StateVariableTPTFilter<float>, 2> filters;

        bool isActive() const noexcept { return zone != nullptr; }
    };

    using Settings = std::span<const SamplerSettings>;
    static const SamplerSettings& settingsFor (const SampleZone&, Settings) noexcept;
    void handle (const juce::MidiMessage&, const SampleSet*, Settings) noexcept;
    void startVoice (int note, int velocity, int channel, const SampleSet&, Settings) noexcept;
    void renderVoice (Voice&, juce::AudioBuffer<float>&, int start, int count, const SamplerSettings&) noexcept;
    void applySettings (Voice&, const SamplerSettings&) noexcept;
    void useSet (const SampleSet*, int maxFade) noexcept;
    double incrementFor (const Voice&, const SamplerSettings&) const noexcept;

    std::array<Voice, maxVoices> voices;
    const SampleSet* lastSet = nullptr;
    double sampleRate = 44100.0;
    uint32_t roundRobin = 0;
    uint64_t nextAge = 0;
    float pitchBend = 0.0f; // semitones
    bool sustainPedal = false;
    noteExpression::Tracker expressions; // per-note fine pitch, pan and volume

    static constexpr int fadeSamples = 64;
};

} // namespace spinus::dsp
