#include <catch2/catch_test_macros.hpp>

#include "engine/PipeWireInput.h"
#include "ui/AudioSettings.h"

using namespace spinus;

namespace
{
// What pw-dump prints, cut down: a laptop microphone, a USB microphone, a webcam, a speaker, Spinus's own
// recording stream (process 4242) and another program's.
const char* dumpJson = R"([
  { "id": 66, "type": "PipeWire:Interface:Node", "info": { "props": {
      "media.class": "Audio/Source", "node.name": "alsa_input.pci.HiFi__Mic__source",
      "node.description": "Audio Coprocessor Digital Microphone" } } },
  { "id": 111, "type": "PipeWire:Interface:Node", "info": { "props": {
      "media.class": "Audio/Source", "node.name": "alsa_input.usb-fifine.analog-stereo",
      "node.description": "fifine Microphone Analog Stereo" } } },
  { "id": 133, "type": "PipeWire:Interface:Node", "info": { "props": {
      "media.class": "Audio/Source", "node.name": "alsa_input.usb-046d_0825.mono-fallback", "node.nick": "Webcam" } } },
  { "id": 65, "type": "PipeWire:Interface:Node", "info": { "props": {
      "media.class": "Audio/Sink", "node.name": "alsa_output.headphones", "node.description": "Headphones" } } },
  { "id": 77, "type": "PipeWire:Interface:Node", "info": { "props": {
      "media.class": "Stream/Input/Audio", "node.name": "alsa_capture.spinus", "application.process.id": "4242" } } },
  { "id": 78, "type": "PipeWire:Interface:Node", "info": { "props": {
      "media.class": "Stream/Input/Audio", "node.name": "other", "application.process.id": 99 } } },
  { "id": 3, "type": "PipeWire:Interface:Client", "info": null }
])";

struct FakePipeWire
{
    juce::StringArray commands;
    juce::String dump = dumpJson;

    PipeWireInput::Runner runner()
    {
        return [this] (const juce::StringArray& command, bool& ok)
        {
            commands.add (command.joinIntoString (" "));
            ok = true;
            return command[0] == "pw-dump" ? dump : juce::String();
        };
    }
};
} // namespace

TEST_CASE ("PipeWire's microphones by name, and which recording stream is Spinus's", "[engine][pipewire]")
{
    const auto dump = juce::JSON::parse (dumpJson);
    const auto sources = PipeWireInput::parseSources (dump);
    REQUIRE (sources.size() == 3);
    CHECK (sources[0] ==
           PipeWireInput::Source { "alsa_input.pci.HiFi__Mic__source", "Audio Coprocessor Digital Microphone" });
    CHECK (sources[1].description == "fifine Microphone Analog Stereo");
    CHECK (sources[2].description == "Webcam"); // no description: its short name
    CHECK (PipeWireInput::findCaptureStream (dump, 4242) == 77);
    CHECK (PipeWireInput::findCaptureStream (dump, 99) == 78);
    CHECK (PipeWireInput::findCaptureStream (dump, 1) == -1);
    CHECK (PipeWireInput::parseSources (juce::var()).empty()); // pw-dump missing

    CHECK (PipeWireInput::isPipeWireDevice ("default"));
    CHECK (PipeWireInput::isPipeWireDevice ("PipeWire Sound Server"));
    CHECK (! PipeWireInput::isPipeWireDevice ("fifine Microphone, USB Audio; Front output / input"));
    CHECK (ui::AudioSettings::isClutter ("Rate Converter Plugin Using Samplerate Library"));
    CHECK (ui::AudioSettings::isClutter ("Plugin for channel upmix (4,6,8)"));
    CHECK (! ui::AudioSettings::isClutter ("fifine Microphone, USB Audio; Front output / input"));
}

TEST_CASE ("The chosen microphone: remembered, and Spinus's recording stream connected to it", "[engine][pipewire]")
{
    juce::AudioDeviceManager manager; // no device open: nothing to connect yet
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-pipewire-" + juce::Uuid().toString() + ".settings");
    juce::PropertiesFile::Options options;
    auto settings = std::make_unique<juce::PropertiesFile> (file, options);
    FakePipeWire fake;
    {
        PipeWireInput input (manager, settings.get(), fake.runner());
        CHECK (input.getChosen().isEmpty());
        CHECK (input.listSources().size() == 3);
        input.choose ("alsa_input.usb-fifine.analog-stereo");
        CHECK (input.routeNow()); // no device recording through PipeWire: nothing to do
        fake.commands.clear();

        // The stream of this process goes to the fifine microphone, the system's default left alone.
        CHECK (input.connect (4242));
        CHECK (fake.commands ==
               juce::StringArray { "pw-dump", "pw-metadata 77 target.object alsa_input.usb-fifine.analog-stereo" });
        // Not recording yet: tried again later.
        CHECK (! input.connect (5));
        // Back to the system's default input.
        input.choose ({});
        fake.commands.clear();
        CHECK (input.connect (4242));
        CHECK (fake.commands[1] == "pw-metadata -d 77 target.object");
        input.choose ("alsa_input.usb-046d_0825.mono-fallback");
    }
    // Remembered for the next start.
    settings = std::make_unique<juce::PropertiesFile> (file, options);
    PipeWireInput again (manager, settings.get(), fake.runner());
    CHECK (again.getChosen() == "alsa_input.usb-046d_0825.mono-fallback");
    settings = nullptr;
    file.deleteFile();

    // Without PipeWire's tools: no microphones listed, nothing breaks.
    PipeWireInput without (manager, nullptr,
                           [] (const juce::StringArray&, bool& ok)
                           {
                               ok = false;
                               return juce::String();
                           });
    CHECK (without.listSources().empty());
    CHECK (! without.connect (4242));
}
