#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/NoteExpression.h"

#include <array>

namespace spinus::dsp
{

/** 3x Osc's settings (FL's 3xOsc): three oscillators mixed, an amp envelope and a filter. */
struct ThreeOscSettings
{
    enum class Shape
    {
        sine,
        triangle,
        saw,
        square,
        noise
    };

    struct Oscillator
    {
        Shape shape = Shape::saw;
        float coarse = 0.0f; // semitones
        float fine = 0.0f;   // cents
        float level = 1.0f;  // 0..1
        float pan = 0.0f;    // -1..1
        float phase = 0.0f;  // start phase, 0..1 of a cycle
    };

    std::array<Oscillator, 3> oscillators;
    float attack = 0.005f, decay = 0.3f, sustain = 0.8f, release = 0.2f;
    enum class Filter
    {
        off,
        lowPass,
        highPass,
        bandPass
    };
    Filter filter = Filter::off;
    float cutoffHz = 20000.0f, resonance = 0.0f;
    float envelopeAmount = 0.0f; // -1..1: the amp envelope opens (or closes) the filter by up to 6 octaves
    float gainDb = 0.0f, velocitySensitivity = 1.0f;
    /** Mono: one voice; a note played while another is held slides to it (without starting the envelope
        again) over `glide` seconds, 303-style, and back to a note still held when it is let go. */
    bool mono = false;
    float glide = 0.0f;
};

/** Plays 3x Osc: 16 voices of three band-limited oscillators (PolyBLEP saw and square) each.
    Real-time safe: render() does not allocate, lock or block. */
class ThreeOscEngine
{
public:
    static constexpr int maxVoices = 16;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void render (juce::AudioBuffer<float>&, const juce::MidiBuffer&, const ThreeOscSettings&, int startSample,
                 int numSamples) noexcept;
    int getNumActiveVoices() const noexcept;

private:
    struct Voice
    {
        int note = -1, channel = 1;
        bool active = false, held = false;
        double pitch = 60.0, target = 60.0, glideStep = 0.0; // the note it plays now, sliding to `target`
        float level = 1.0f;
        uint64_t age = 0;
        std::array<double, 3> phases {};
        juce::ADSR envelope;
        std::array<juce::dsp::StateVariableTPTFilter<float>, 2> filters;
        juce::Random noise;
    };

    void handle (const juce::MidiMessage&, const ThreeOscSettings&) noexcept;
    void renderVoice (Voice&, juce::AudioBuffer<float>&, int start, int count, const ThreeOscSettings&) noexcept;

    std::array<Voice, maxVoices> voices;
    double sampleRate = 44100.0;
    uint64_t nextAge = 0;
    float pitchBend = 0.0f;
    bool sustainPedal = false;
    std::array<int, 16> monoHeld {}; // mono: the keys held, the last one played last
    int monoCount = 0;
    void glideTo (Voice&, int note, const ThreeOscSettings&) noexcept;
    void startVoice (Voice&, const juce::MidiMessage&, const ThreeOscSettings&) noexcept;
    noteExpression::Tracker expressions; // per-note fine pitch, pan and volume
};

} // namespace spinus::dsp
