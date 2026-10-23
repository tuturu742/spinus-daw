// spinus-plugin-host: runs one plugin in its own process for Spinus, so that a plugin that crashes or
// hangs cannot take Spinus down. Started by spinus::plugins::sandbox::SandboxPluginFormat.

#include <juce_events/juce_events.h>

#include "core/ChildProcess.h"
#include "plugins/sandbox/SandboxHost.h"

int main (int argc, char* argv[])
{
    spinus::core::endAtOnceOnCrash();           // Spinus notices at once, and carries on
    const juce::ScopedJuceInitialiser_GUI juce; // plugins need a message manager, and windows
    juce::StringArray args (argv, argc);
    args.remove (0);
    return spinus::plugins::sandbox::runSandboxHost (args.joinIntoString (" "));
}
