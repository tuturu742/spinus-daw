#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

namespace spinus::dsp
{

/** The Delay's settings: echoes at a time locked to the song's tempo (a note length) or free in
    milliseconds, fed back through a low cut and a high cut, in stereo, ping-pong or mono. */
struct DelaySettings
{
    enum class Mode
    {
        stereo,   // each side echoes itself
        pingPong, // the echoes bounce left, right, left...
        mono      // both sides echo the sum, in the middle
    };

    bool sync = true;       // the time is a note length at the song's tempo
    double beats = 0.75;    // when synced: the time in quarter notes (0.75: a dotted 1/8)
    float timeMs = 350.0f;  // when not synced
    float feedback = 0.35f; // 0..0.95: how much of each echo comes back as the next one
    Mode mode = Mode::stereo;
    static constexpr float lowCutOff = 20.0f, highCutOff = 20000.0f; // the ends of the cuts' ranges: off

    float lowCutHz = 80.0f;    // on the echoes (and so on every repeat); lowCutOff or below: off
    float highCutHz = 8000.0f; // on the echoes; highCutOff or above: off
    float width = 1.0f;        // the echoes' stereo width: 0 in the middle, 1 as they are
    float mix = 0.3f;          // 0 dry, 0.5 both at full, 1 only the echoes (for a send bus)
};

/** A feedback delay with a line per side, preallocated in prepare() for the longest time (maxSeconds) at
    that sample rate. A new time (the knob, a note length, the song's tempo) crossfades from the old echo
    to the new one, so it never clicks. Real-time safe: process() does not allocate, lock or block. */
class DelayEngine
{
public:
    static constexpr int maxChannels = 2;
    static constexpr double maxSeconds = 4.0;
    static constexpr double crossfadeSeconds = 0.05;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    /** Delays the buffer in place (mono or stereo). `bpm` is the song's tempo now. */
    void process (juce::AudioBuffer<float>&, const DelaySettings&, double bpm) noexcept;

    /** The delay time for settings at a tempo, in milliseconds (up to maxSeconds). */
    static double timeMsFor (const DelaySettings&, double bpm) noexcept;

    /** The time of the echoes the last block played, in milliseconds; negative before the first block. */
    float getTimeMs() const noexcept { return timeNow.load(); }
    /** The tempo the last block used. */
    float getBpm() const noexcept { return bpmNow.load(); }

private:
    struct OnePole // Zavalishin's TPT one-pole: low pass, and high pass as the input less it
    {
        float s = 0.0f;
        float lowPass (float x, float g) noexcept
        {
            const auto v = (x - s) * g;
            const auto y = v + s;
            s = y + v;
            return y;
        }
    };

    float read (int channel, double delaySamples) const noexcept;
    static float coefficientFor (float hz, double sampleRate) noexcept;

    double sampleRate = 44100.0;
    std::array<std::vector<float>, maxChannels> lines;
    int size = 0, write = 0;
    std::array<OnePole, maxChannels> lowCut {}, highCut {};

    // The crossfade between the echo at the old time and the one at the new.
    double delayNow = -1.0, delayNext = -1.0;
    int fadeLength = 1, fadeLeft = 0;

    juce::SmoothedValue<float> feedback, dryGain, wetGain, width;
    bool smoothersStarted = false;
    std::atomic<float> timeNow { -1.0f }, bpmNow { 120.0f };
};

} // namespace spinus::dsp
