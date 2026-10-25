// spinus-plugin-scanner: loads one plugin file in its own process so that a plugin that crashes or
// hangs while loading cannot take Spinus down. Started by spinus::plugins::PluginScanner.
//
// Usage: spinus-plugin-scanner --format <VST3|CLAP|LV2|VST|...> --file <path> --output <result.xml>

#include <juce_events/juce_events.h>

#include "core/ChildProcess.h"
#include "core/Log.h"
#include "plugins/HostLibraries.h"
#include "plugins/PluginScanner.h"

int main (int argc, char* argv[])
{
    spinus::plugins::addPluginFoldersToLv2Path(); // the computer's LV2 plugins, in the Flatpak
    spinus::core::endAtOnceOnCrash();             // Spinus notices at once, and carries on
    const juce::ScopedJuceInitialiser_GUI juce;   // many plugins need a message manager while loading
    // Notable errors go into the log of the Spinus that started this (labelled "plugin scanner").
    spinus::core::Logs::joinParentLog ("plugin scanner");
    spinus::core::Logs::installTerminateHandler();
    const juce::StringArray args (argv, argc);

    auto value = [&] (const char* flag)
    { return args.indexOf (flag) >= 0 ? args[args.indexOf (flag) + 1] : juce::String(); };
    const auto format = value ("--format"), file = value ("--file"), output = value ("--output");

    if (format.isEmpty() || file.isEmpty() || output.isEmpty())
    {
        std::cerr << "Usage: spinus-plugin-scanner --format <format> --file <plugin> --output <result.xml>\n";
        return 1;
    }

    return spinus::plugins::runScannerProcess (format, file,
                                               juce::File::getCurrentWorkingDirectory().getChildFile (output));
}
