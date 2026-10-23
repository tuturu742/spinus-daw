#pragma once

#include <juce_core/juce_core.h>

namespace spinus::plugins::sandbox
{

/** The command-line ID Spinus and spinus-plugin-host agree on (juce::ChildProcessCoordinator). */
inline constexpr const char* processId = "spinus-plugin-host";

/** spinus-plugin-host's side (app/PluginHostMain.cpp): hosts one plugin for Spinus, which talks to
    it over a pipe (load, state, parameters' text, the plugin's window) and shared memory (audio, MIDI,
    parameter changes; SandboxProtocol.h). Runs until Spinus closes the connection. */
int runSandboxHost (const juce::String& commandLine);

} // namespace spinus::plugins::sandbox
