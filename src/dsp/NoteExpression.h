#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace spinus::dsp::noteExpression
{

/** Per-note fine pitch, pan and volume as Spinus sends them in MIDI.

    A note with any of these set plays on its own MIDI channel (2-14, the member channels Tracktion
    tracks for MPE clips). Just before its note-on, the channel gets a pitch bend (fine pitch, with the
    MPE member range of 48 semitones), CC 10 (pan) and CC 7 (volume). Spinus's instruments read them
    with a Tracker; the CLAP host turns them into CLAP note expressions. Notes on channel 1 have none. */
constexpr int firstChannel = 2, lastChannel = 14;
constexpr double pitchBendRange = 48.0; // semitones (also MPE's default for member channels)
constexpr int panController = 10, volumeController = 7;

/** MPE controllers (ROLI, LinnStrument, Osmose...) play each note on its own channel too, 2 to 16, with
    pitch bend, channel pressure and CC 74 ("timbre", the slide up and down a key). Spinus's instruments
    and its CLAP host follow those per note as well. */
constexpr int lastMpeChannel = 16, timbreController = 74;

struct Values
{
    float cents = 0.0f;  // -100..100 from Spinus's notes; up to +-4800 from an MPE controller's pitch bend
    float pan = 0.0f;    // -1 (left) .. 1 (right)
    float volume = 1.0f; // 0..1, linear gain
    float pressure = 0.0f, timbre = 0.5f; // 0..1, from an MPE controller only
    bool hasPressure = false, hasTimbre = false;

    /** Spinus's own per-note values (pressure and timbre come only from controllers). */
    bool isDefault() const noexcept
    {
        return juce::exactlyEqual (cents, 0.0f) && juce::exactlyEqual (pan, 0.0f) && juce::exactlyEqual (volume, 1.0f);
    }
};

inline bool isMemberChannel (int channel) noexcept { return channel >= firstChannel && channel <= lastChannel; }
/** A channel whose messages are per note: Spinus's member channels, and MPE's (2 to 16). */
inline bool isPerNoteChannel (int channel) noexcept { return channel >= firstChannel && channel <= lastMpeChannel; }

inline int centsToPitchWheel (float cents) noexcept
{
    return juce::jlimit (0, 16383, 8192 + juce::roundToInt ((double)cents / (100.0 * pitchBendRange) * 8192.0));
}

inline float pitchWheelToCents (int position) noexcept
{
    return (float)((position - 8192) / 8192.0 * pitchBendRange * 100.0);
}

inline int panToController (float pan) noexcept { return juce::jlimit (0, 127, 64 + juce::roundToInt (pan * 63.0f)); }
inline float controllerToPan (int value) noexcept { return juce::jlimit (-1.0f, 1.0f, (float)(value - 64) / 63.0f); }
inline int volumeToController (float volume) noexcept
{
    return juce::jlimit (0, 127, juce::roundToInt (volume * 127.0f));
}
inline float controllerToVolume (int value) noexcept { return (float)value / 127.0f; }

/** The three messages that go before a note-on on `channel`. */
inline std::array<juce::MidiMessage, 3> encode (int channel, const Values& v)
{
    return { juce::MidiMessage::pitchWheel (channel, centsToPitchWheel (v.cents)),
             juce::MidiMessage::controllerEvent (channel, panController, panToController (v.pan)),
             juce::MidiMessage::controllerEvent (channel, volumeController, volumeToController (v.volume)) };
}

/** Follows the per-note values of each member channel. Real-time safe. */
class Tracker
{
public:
    /** Returns true if the message set a per-note value (pitch bend, CC 10, CC 7, channel pressure or
        CC 74 on a per-note channel): the caller does not treat it as a channel-wide message. */
    bool handle (const juce::MidiMessage& m) noexcept
    {
        const auto channel = m.getChannel();
        if (! isPerNoteChannel (channel))
            return false;

        auto& v = values[(size_t)channel];
        if (m.isPitchWheel())
            v.cents = pitchWheelToCents (m.getPitchWheelValue());
        else if (m.isControllerOfType (panController))
            v.pan = controllerToPan (m.getControllerValue());
        else if (m.isControllerOfType (volumeController))
            v.volume = controllerToVolume (m.getControllerValue());
        else if (m.isChannelPressure())
        {
            v.pressure = (float)m.getChannelPressureValue() / 127.0f;
            v.hasPressure = true;
        }
        else if (m.isControllerOfType (timbreController))
        {
            v.timbre = (float)m.getControllerValue() / 127.0f;
            v.hasTimbre = true;
        }
        else
            return false;
        return true;
    }

    /** The values for notes on this channel (defaults outside the per-note channels). */
    const Values& get (int channel) const noexcept
    {
        static const Values defaults;
        return isPerNoteChannel (channel) ? values[(size_t)channel] : defaults;
    }

    void reset() noexcept { values.fill ({}); }

private:
    std::array<Values, 17> values {};
};

/** Left and right gains for a per-note pan, as the instruments' own pan knobs do it (balance: the far
    side goes down as the pan moves away from it). */
inline std::array<float, 2> panGains (float pan) noexcept
{
    return { juce::jmin (1.0f, 1.0f - pan), juce::jmin (1.0f, 1.0f + pan) };
}

} // namespace spinus::dsp::noteExpression
