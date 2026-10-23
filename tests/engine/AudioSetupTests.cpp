#include <catch2/catch_test_macros.hpp>

#include "engine/AudioSetup.h"
#include "engine/EngineHost.h"
#include "ui/AudioSettings.h"

using namespace spinus;

TEST_CASE ("The audio status line says what the settings mean", "[engine][audio-setup]")
{
    engine::AudioSetup::Status none;
    CHECK (none.describe() == "No audio device open");

    engine::AudioSetup::Status s { "JACK", "Audio Coprocessor Headphones", 48000.0, 256, 512, 2 };
    CHECK (s.describe() == "JACK: Audio Coprocessor Headphones, 48.0 kHz, 256 samples (5.3 ms buffer, about 10.7 ms "
                           "output latency), 2 dropouts");
}

TEST_CASE ("The audio settings page opens without a device", "[ui][audio-setup]")
{
    EngineHost host ({ .openAudioDevice = false });
    ui::AudioSettings settings (host.getEngine().getDeviceManager().deviceManager);
    CHECK (settings.getStatusText() == "No audio device open");
}

TEST_CASE ("First-start defaults open a real device", "[.hardware]")
{
    // Needs a sound card: spinus_tests "[.hardware]". Prints what the first-start rules choose here.
    juce::AudioDeviceManager manager;
    const auto choice = engine::AudioSetup::applyDefaults (manager);
    std::cout << "First-start audio choice: " << choice << '\n';
    CHECK (manager.getCurrentAudioDevice() != nullptr);
    CHECK (manager.getCurrentAudioDevice()->getCurrentBufferSizeSamples() == engine::AudioSetup::defaultBufferSize);
    manager.closeAudioDevice();
}
