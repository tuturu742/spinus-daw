// Test helper for PluginCrashGuard: crashes on purpose, in a plugin or not.
//
// Usage: spinus_crash_guard_helper <record file> <plugin.clap>    loads the plugin in this process, crashes it
//        spinus_crash_guard_helper <record file> --elsewhere      crashes outside any plugin

#include "engine/PluginCrashGuard.h"
#include "plugins/SandboxList.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <csignal>

int main (int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juce;
    if (argc < 3)
        return 12;
    spinus::engine::PluginCrashGuard::install (juce::File (argv[1]));
    const juce::String target (argv[2]);
    if (target == "--elsewhere")
    {
        raise (SIGSEGV);
        return 13;
    }

    spinus::plugins::ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    clap.findAllTypesForFile (found, target);
    if (found.isEmpty())
        return 14;
    const spinus::engine::PluginCrashGuard::ModuleWatch watch;
    juce::String error;
    auto instance = clap.createInstanceFromDescription (*found[0], 48000.0, 256, error);
    if (instance == nullptr)
        return 15;
    watch.addNewModules (spinus::plugins::SandboxList::keyOf ("CLAP", target), found[0]->name, target);

    instance->prepareToPlay (48000.0, 256);
    instance->getParameters()[1]->setValue (1.0f); // Crash
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    instance->processBlock (buffer, midi);
    return 16; // not reached
}
