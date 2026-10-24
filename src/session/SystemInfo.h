#pragma once

#include <juce_core/juce_core.h>

namespace spinus::session
{

/** How this Spinus was installed: "Flatpak", "AppImage", "Arch package", "Windows installer" or "development
    build" (and on Windows a build that was not installed: "Windows (development build)"). */
juce::String getPackageKind();

/** The system in a line: "Arch Linux, kernel 7.2.9 (x86_64)"; "Windows 11". Never the computer's name. */
juce::String getSystemName();

/** The session log's header (core::Logs::Options::header): version, package, system, desktop, the JUCE and
    Tracktion versions, CPU and memory, the process id. No user or computer names. */
juce::StringArray describeRunForLog (juce::Time startTime);

} // namespace spinus::session
