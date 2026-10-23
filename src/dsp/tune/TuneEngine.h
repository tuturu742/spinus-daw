#pragma once

#include "PitchDetector.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace spinus::dsp
{

/** Spinus Tune's real-time settings. */
struct TuneSettings
{
    enum class Scale
    {
        chromatic,
        major,
        minor,
        harmonicMinor,
        majorPentatonic,
        minorPentatonic,
        blues,
        dorian,
        mixolydian
    };

    enum class Range
    {
        normal, // voices down to about 75 Hz; 10 ms delay
        low     // deep voices down to about 50 Hz; 20 ms delay
    };

    int key = 0; // the scale's first note: 0 = C ... 11 = B
    Scale scale = Scale::chromatic;
    float retuneMs = 20.0f; // how quickly the pitch moves to the note: 0 = instantly ("robot")
    float humanize = 0.0f;  // 0..1: held notes are corrected more slowly, keeping their vibrato
    bool keepFormants = true;
    Range range = Range::normal;
    float mix = 1.0f; // 0 = the voice as it was, 1 = corrected
};

/** Pulls a voice (or any single-note sound) to the nearest note of a scale, or to the MIDI notes held,
    with a short fixed delay: pitch from PitchDetector, shifting with TD-PSOLA (grains one pitch period
    apart are taken from the input around its pitch marks and laid down at the new period, which keeps
    the voice's formants). Sounds without a pitch pass through unchanged.

    Real-time safe: process() does not allocate, lock or block. prepare() allocates.
*/
class TuneEngine
{
public:
    /** The delay for a range, in samples. */
    static int getLatency (double sampleRate, TuneSettings::Range);

    /** True if the MIDI note's pitch class is in the scale. */
    static bool isInScale (int note, int key, TuneSettings::Scale);

    /** The note the pitch (in MIDI notes, 69 = A4) is pulled to: the nearest note of the scale, or of
        `heldNotes` when any are given. `current` (or -1) is kept while the pitch stays near it. */
    static int chooseTarget (float pitch, int current, int key, TuneSettings::Scale,
                             const std::array<bool, 128>& heldNotes, bool anyHeld);

    void prepare (double sampleRate, int maximumBlockSize, int numChannels);
    void reset();

    /** Corrects `audio` in place (delayed by getLatency() for the settings' range). MIDI note-ons and
        note-offs in `midi` choose the target notes while they are held. */
    void process (juce::AudioBuffer<float>& audio, const juce::MidiBuffer& midi, const TuneSettings&) noexcept;

    //==============================================================================
    // For the window, from any thread.
    float getInputPitch() const noexcept { return inputPitch.load(); } // MIDI notes; 0 when unvoiced
    int getTargetNote() const noexcept { return targetNote.load(); }   // -1 when none
    float getCorrection() const noexcept { return correction.load(); } // semitones being applied

private:
    void analyse (const TuneSettings&) noexcept;
    void placeAnalysisMarks (int64_t upTo) noexcept;
    void placeGrains (int64_t upTo, const TuneSettings&) noexcept;
    size_t at (int64_t index) const noexcept { return (size_t)index & mask; } // ring position
    float readMono (int64_t index) const noexcept { return mono[at (index)]; }
    int64_t nearestMark (double time) const noexcept;
    int grainHalfLength (double inputPeriod) const noexcept;

    double sampleRate = 48000.0;
    int channels = 0;
    PitchDetector detectors[2]; // normal, low
    std::vector<float> frame;

    // Rings indexed by the input sample number (masked).
    size_t mask = 0;
    std::vector<std::vector<float>> input, output;
    std::vector<float> mono, windowSum;
    int64_t written = 0; // input samples received
    int latency = 0, hop = 0, sinceAnalysis = 0;
    TuneSettings::Range currentRange = TuneSettings::Range::normal;

    // Pitch.
    float period = 0.0f; // input period in samples (last voiced one)
    bool voiced = false;
    std::array<float, 3> recent {};
    int target = -1;
    float shift = 0.0f;       // semitones applied (smoothed)
    float heldSeconds = 0.0f; // how long the current target note has been held
    float wet = 0.0f;         // 0 = dry (no pitch), 1 = shifted
    float wetStep = 0.0f;
    std::array<bool, 128> held {};
    int numHeld = 0;

    // PSOLA.
    static constexpr int maxMarks = 64;
    std::array<int64_t, maxMarks> marks {};
    int numMarks = 0, newestMark = -1;
    double nextAnalysisMark = 0.0, nextSynthesisMark = 0.0;

    std::atomic<float> inputPitch { 0.0f }, correction { 0.0f };
    std::atomic<int> targetNote { -1 };
};

} // namespace spinus::dsp
