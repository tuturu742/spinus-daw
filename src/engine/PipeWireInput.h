#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>

namespace spinus
{

/** Which microphone Spinus records from when it records through PipeWire (a laptop
    often has a built-in, a USB and a webcam microphone). Through ALSA a USB microphone is usually busy, since
    PipeWire has it open, and its ALSA names are cryptic. PipeWire itself offers every microphone by name,
    but ALSA's PipeWire device records from the system's default one. So Spinus records from that device and
    asks PipeWire to connect its recording stream to the chosen microphone instead (the stream's
    target.object, as a volume control app does), without changing the system's default.

    Uses pw-dump and pw-metadata (part of PipeWire); without them the list is empty and nothing changes.
    Message thread only. */
class PipeWireInput : private juce::ChangeListener, private juce::Timer
{
public:
    struct Source
    {
        juce::String name;        // node.name, e.g. "alsa_input.usb-...analog-stereo"
        juce::String description; // e.g. "fifine Microphone Analog Stereo"

        bool operator== (const Source&) const = default;
    };

    /** Runs a program and returns what it printed; ok is false if it could not run. Replaceable (tests). */
    using Runner = std::function<juce::String (const juce::StringArray& command, bool& ok)>;

    /** Remembers the choice in `settings` (nullptr: not remembered) and connects the recording stream to it
        whenever the device (re)opens. */
    PipeWireInput (juce::AudioDeviceManager&, juce::PropertiesFile* settings, Runner = {});
    ~PipeWireInput() override;

    /** The microphones and other inputs PipeWire offers, by name (empty without PipeWire). */
    std::vector<Source> listSources() const;

    /** The chosen source's node.name; empty: the system's default input. */
    juce::String getChosen() const { return chosen; }
    void choose (const juce::String& sourceName);

    /** Whether recording from this ALSA device name goes through PipeWire (its default device, or the
        PulseAudio one that PipeWire also serves). */
    static bool isPipeWireDevice (const juce::String& alsaDeviceName);

    // The parts that read pw-dump's JSON (tests).
    static std::vector<Source> parseSources (const juce::var& dump);
    /** The node ID of a recording stream of this process, or -1. */
    static int findCaptureStream (const juce::var& dump, int processId);

    /** Tries now to connect the recording stream; true once done, or when there is nothing to connect. */
    bool routeNow();
    /** Connects the recording stream of a process to the chosen source (or back to the default); false
        while it has none (tests). */
    bool connect (int processId);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    juce::var dump() const;
    bool recordsThroughPipeWire() const;

    juce::AudioDeviceManager& manager;
    juce::PropertiesFile* settings;
    Runner run;
    juce::String chosen;
    int attempts = 0;

    JUCE_DECLARE_NON_COPYABLE (PipeWireInput)
};

} // namespace spinus
