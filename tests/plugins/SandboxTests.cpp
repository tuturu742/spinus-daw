#include <catch2/catch_test_macros.hpp>

#include "plugins/clap/ClapPluginFormat.h"
#include "plugins/sandbox/SandboxPluginFormat.h"

#include <thread>

using namespace spinus::plugins;
using sandbox::SandboxPluginFormat;
using Failure = sandbox::SandboxedInstance::Failure;

namespace
{
juce::PluginDescription crashTest()
{
    ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    clap.findAllTypesForFile (
        found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("crash-test.clap").getFullPathName());
    REQUIRE (found.size() == 1);
    return *found[0];
}

struct Fixture
{
    Fixture()
        : real (crashTest()), format (juce::File (SPINUS_PLUGIN_HOST),
                                      [this] (const juce::String& f, const juce::String& file,
                                              int) -> std::optional<juce::PluginDescription>
                                      {
                                          if (f == real.pluginFormatName && file == real.fileOrIdentifier)
                                              return real;
                                          return std::nullopt;
                                      })
    {
    }

    std::unique_ptr<juce::AudioPluginInstance> load()
    {
        juce::String error;
        auto instance =
            format.createInstanceFromDescription (SandboxPluginFormat::describe (real), 48000.0, block, error);
        INFO (error);
        REQUIRE (instance != nullptr);
        return instance;
    }

    /** Plays blocks of a 1 kHz sine (amplitude 0.5) and keeps the left output. Paced: like an audio
        device, a block every block's time, with the message loop running between. */
    std::vector<float> play (juce::AudioPluginInstance& instance, int blocks, bool paced)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i,
                                      0.5f * (float)std::sin (2.0 * M_PI * 1000.0 * (double)(phase + i) / 48000.0));
            phase += block;
            instance.processBlock (buffer, midi);
            out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + block);
            if (paced)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (1000 * block / 48000);
        }
        return out;
    }

    static float peak (const std::vector<float>& x, size_t from, size_t to)
    {
        float p = 0.0f;
        for (auto i = from; i < std::min (to, x.size()); ++i)
            p = std::max (p, std::abs (x[i]));
        return p;
    }

    static constexpr int block = 256;
    juce::PluginDescription real;
    SandboxPluginFormat format;
    int64_t phase = 0;
};

bool waitFor (const std::function<bool()>& done, int ms = 5000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
    while (! done() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    return done();
}
} // namespace

TEST_CASE ("A plugin in its own process: audio, parameters, text and state", "[plugins][sandbox]")
{
    Fixture f;
    auto instance = f.load();
    CHECK (instance->getName() == "Crash Test");
    CHECK (instance->getTotalNumInputChannels() == 2);
    CHECK (instance->getTotalNumOutputChannels() == 2);
    const auto& parameters = instance->getParameters();
    REQUIRE (parameters.size() == 3);
    CHECK (parameters[0]->getName (100) == "Gain");
    CHECK (juce::approximatelyEqual (parameters[0]->getValue(), 1.0f));
    CHECK (parameters[0]->getText (0.5f, 100) == "50 %");
    CHECK (juce::approximatelyEqual (parameters[0]->getValueForText ("25 %"), 0.25f));
    juce::PluginDescription described;
    instance->fillInPluginDescription (described);
    CHECK (described.pluginFormatName == "Spinus Sandbox");

    // Offline: each block waits for its answer, one block late (the reported latency).
    instance->setNonRealtime (true);
    instance->prepareToPlay (48000.0, Fixture::block);
    CHECK (instance->getLatencySamples() == Fixture::block);
    auto out = f.play (*instance, 8, false);
    CHECK (Fixture::peak (out, 0, Fixture::block) < 1.0e-6f);
    CHECK (std::abs (Fixture::peak (out, (size_t)Fixture::block * 2, (size_t)Fixture::block * 8) - 0.5f) < 0.01f);

    // A parameter set here reaches the plugin.
    parameters[0]->setValue (0.5f);
    out = f.play (*instance, 4, false);
    CHECK (std::abs (Fixture::peak (out, (size_t)Fixture::block * 2, (size_t)Fixture::block * 4) - 0.25f) < 0.01f);

    // In real time, paced like an audio device.
    instance->releaseResources();
    instance->setNonRealtime (false);
    instance->prepareToPlay (48000.0, Fixture::block);
    out = f.play (*instance, 60, true);
    CHECK (std::abs (Fixture::peak (out, (size_t)Fixture::block * 20, (size_t)Fixture::block * 60) - 0.25f) < 0.01f);

    // State: another instance gets the gain, and shows it.
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    auto second = f.load();
    second->setStateInformation (state.getData(), (int)state.getSize());
    CHECK (juce::approximatelyEqual (second->getParameters()[0]->getValue(), 0.5f));
}

TEST_CASE ("A plugin that crashes or hangs in its own process leaves Spinus running", "[plugins][sandbox]")
{
    Fixture f;
    auto sandboxed = [] (juce::AudioPluginInstance& i) { return dynamic_cast<sandbox::SandboxedInstance*> (&i); };

    // Crash: the process goes, the instance says so and lets its input through.
    {
        auto instance = f.load();
        REQUIRE (sandboxed (*instance) != nullptr);
        instance->prepareToPlay (48000.0, Fixture::block);
        f.play (*instance, 20, true);
        CHECK (sandboxed (*instance)->getFailure() == Failure::none);
        instance->getParameters()[1]->setValue (1.0f); // Crash
        f.play (*instance, 20, true);
        CHECK (waitFor ([&] { return sandboxed (*instance)->getFailure() == Failure::crashed; }));
        const auto out = f.play (*instance, 4, true);
        CHECK (std::abs (Fixture::peak (out, 0, out.size()) - 0.5f) < 0.01f); // bypassed
        juce::MemoryBlock state;
        instance->getStateInformation (state); // the last state it had, for reloading
    }

    // Hang: after two seconds without answers it is called hung, and its process is ended.
    {
        auto instance = f.load();
        instance->prepareToPlay (48000.0, Fixture::block);
        f.play (*instance, 10, true);
        instance->getParameters()[2]->setValue (1.0f); // Hang
        f.play (*instance, 48000 * 5 / 2 / Fixture::block, true);
        CHECK (sandboxed (*instance)->getFailure() == Failure::hung);
    }
}

TEST_CASE ("A separated plugin's window shows inside a Spinus window", "[plugins][sandbox][clap-gui]")
{
#if defined(SPINUS_PLUGINS_DIR)
    const auto surge = juce::File (SPINUS_PLUGINS_DIR).getChildFile ("Surge XT.clap");
#else
    const juce::File surge;
#endif
    if (! surge.existsAsFile())
        SKIP ("bundled plugins not built");

    ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    clap.findAllTypesForFile (found, surge.getFullPathName());
    const juce::PluginDescription* real = nullptr;
    for (auto* d : found)
        if (d->name == "Surge XT")
            real = d;
    REQUIRE (real != nullptr);
    SandboxPluginFormat format (juce::File (SPINUS_PLUGIN_HOST),
                                [real] (const juce::String&, const juce::String&,
                                        int) -> std::optional<juce::PluginDescription> { return *real; });
    juce::String error;
    auto instance = format.createInstanceFromDescription (SandboxPluginFormat::describe (*real), 48000.0, 512, error);
    INFO (error);
    REQUIRE (instance != nullptr);
    REQUIRE (instance->hasEditor());

    std::unique_ptr<juce::AudioProcessorEditor> editor (instance->createEditorAndMakeActive());
    REQUIRE (editor != nullptr);
    {
        juce::DocumentWindow window ("Surge XT (separate process)", juce::Colours::black, 0);
        window.setUsingNativeTitleBar (true);
        window.setContentNonOwned (editor.get(), true);
        window.setVisible (true);
        // The plugin's own window opens in the other process and takes its size here.
        CHECK (waitFor ([&] { return editor->getWidth() > 500 && editor->getHeight() > 300; }, 15000));
        INFO (editor->getWidth() << " x " << editor->getHeight());
        if (juce::SystemStats::getEnvironmentVariable ("SPINUS_KEEP_WINDOW", {}).isNotEmpty())
            juce::MessageManager::getInstance()->runDispatchLoopUntil (8000); // to look at it
        window.clearContentComponent();
    }
    editor = nullptr;
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
}
