#include <catch2/catch_test_macros.hpp>

#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <thread>

using namespace spinus;

TEST_CASE ("The engine offers CLAP alongside the JUCE plugin formats", "[plugins]")
{
    EngineHost host ({ .openAudioDevice = false });

    juce::StringArray names;
    for (auto* format : host.getEngine().getPluginManager().pluginFormatManager.getFormats())
        names.add (format->getName());

    CHECK (names.contains (plugins::ClapPluginFormat::formatName));
    CHECK (names.contains ("VST3"));
}

TEST_CASE ("The CLAP format ignores files that are not CLAP plugins", "[plugins]")
{
    plugins::ClapPluginFormat format;
    juce::TemporaryFile notAPlugin (".clap");
    REQUIRE (notAPlugin.getFile().replaceWithText ("not an ELF file"));

    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, notAPlugin.getFile().getFullPathName());
    CHECK (found.isEmpty());

    CHECK_FALSE (format.fileMightContainThisPluginType ("/tmp/plugin.vst3"));
}

TEST_CASE ("A knob dragged in a CLAP plugin's window is heard on the message thread: its start, values and end",
           "[plugins]")
{
    // The test effect in param-ids.clap turns a setting as its window would, in its next block (its audio thread):
    // the start of the drag, the value, and later the end, as CLAP output events.
    const auto file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("param-ids.clap");
    juce::DynamicLibrary library (file.getFullPathName());
    auto* hand = reinterpret_cast<void (*) (int, double, int)> (library.getFunction ("spinus_test_param_ids_hand"));
    REQUIRE (hand != nullptr);

    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file.getFullPathName());
    REQUIRE (found.size() == 1);
    juce::String error;
    auto instance = format.createInstanceFromDescription (*found[0], 48000.0, 256, error);
    INFO (error);
    REQUIRE (instance != nullptr);
    instance->prepareToPlay (48000.0, 256);

    struct Spy final : juce::AudioProcessorListener
    {
        void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override
        {
            heard.add ("value " + juce::String (index) + " " + juce::String (value, 2) + where());
        }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* p, int index) override
        {
            heard.add ("begin " + juce::String (index) + " from " +
                       juce::String (p->getParameters()[index]->getValue(), 2) + where());
        }
        void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* p, int index) override
        {
            heard.add ("end " + juce::String (index) + " at " +
                       juce::String (p->getParameters()[index]->getValue(), 2) + where());
        }
        static juce::String where()
        {
            return juce::MessageManager::existsAndIsCurrentThread() ? juce::String() : " (not the message thread)";
        }
        juce::StringArray heard;
    } spy;
    instance->addListener (&spy);

    auto processOnAnotherThread = [&]
    {
        std::thread audio (
            [&]
            {
                juce::AudioBuffer<float> buffer (2, 256);
                buffer.clear();
                juce::MidiBuffer midi;
                instance->processBlock (buffer, midi);
            });
        audio.join();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    };
    hand (1, 0.6, 1); // Tone, from 0.2
    processOnAnotherThread();
    hand (1, 0.7, 2);
    processOnAnotherThread();
    hand (1, 0.7, 3);
    processOnAnotherThread();

    CHECK (spy.heard.joinIntoString (", ") == "begin 1 from 0.20, value 1 0.60, value 1 0.70, end 1 at 0.70");
    // An end without a start, as some plugins send, is not passed on.
    hand (1, 0.7, 3);
    processOnAnotherThread();
    CHECK (spy.heard.size() == 4);

    instance->removeListener (&spy);
    instance->releaseResources();
}
