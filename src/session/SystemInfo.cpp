#include "SystemInfo.h"

#include "ProgramFolder.h"

#include <SpinusVersion.h>
#include <tracktion_engine/tracktion_engine.h>

#if JUCE_LINUX || JUCE_BSD
#include <sys/utsname.h>
#include <unistd.h>
#elif JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace spinus::session
{

namespace
{
#if JUCE_LINUX || JUCE_BSD
/** PRETTY_NAME from os-release: the computer's own system, also from inside the Flatpak (whose /etc is its
    runtime's). */
juce::String distributionName()
{
    for (const auto* path : { "/run/host/os-release", "/run/host/etc/os-release", "/etc/os-release" })
    {
        if (! isFlatpak() && juce::String (path).startsWith ("/run/host"))
            continue;
        for (const auto& line : juce::StringArray::fromLines (juce::File (path).loadFileAsString()))
            if (line.startsWith ("PRETTY_NAME="))
                return line.fromFirstOccurrenceOf ("=", false, false).unquoted().trim();
    }
    return "Linux";
}
#endif
} // namespace

juce::String getPackageKind()
{
#if JUCE_WINDOWS
    return getProgramFolder().getChildFile ("Uninstall Spinus.exe").existsAsFile() ? "Windows installer"
                                                                                   : "Windows (development build)";
#else
    if (isFlatpak())
        return "Flatpak";
    if (juce::SystemStats::getEnvironmentVariable ("APPIMAGE", {}).isNotEmpty())
        return "AppImage";
    if (const auto folder = getProgramFolder().getFullPathName(); folder.startsWith ("/usr/"))
        return juce::File ("/etc/arch-release").existsAsFile() ? juce::String ("Arch package")
                                                               : "installed in " + folder;
    return "development build";
#endif
}

juce::String getSystemName()
{
#if JUCE_LINUX || JUCE_BSD
    auto name = distributionName();
    if (utsname u {}; uname (&u) == 0)
        name << ", kernel " << u.release << " (" << u.machine << ")"; // not u.nodename: the computer's name
    return name;
#else
    return juce::SystemStats::getOperatingSystemName() +
           (juce::SystemStats::isOperatingSystem64Bit() ? ", 64-bit" : juce::String());
#endif
}

juce::StringArray describeRunForLog (juce::Time startTime)
{
    juce::StringArray lines;
    lines.add (juce::String (productName) + " " + versionString + ": log of the run started " +
               startTime.formatted ("%Y-%m-%d %H:%M:%S") + " (UTC" + startTime.getUTCOffsetString (true) + ")");
    lines.add ("Package: " + getPackageKind());
    lines.add ("System: " + getSystemName());
#if JUCE_LINUX || JUCE_BSD
    const auto desktop = juce::SystemStats::getEnvironmentVariable ("XDG_CURRENT_DESKTOP", "unknown");
    const auto session = juce::SystemStats::getEnvironmentVariable ("XDG_SESSION_TYPE", {});
    lines.add ("Desktop: " + desktop + (session.isNotEmpty() ? " (" + session + ")" : juce::String()) +
               (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty() ? ", no X11 display" : ""));
#endif
    lines.add ("Built with " + juce::SystemStats::getJUCEVersion() + " and " + tracktion::Engine::getVersion());
    lines.add ("CPU: " + juce::SystemStats::getCpuModel().trim() + ": " +
               juce::String (juce::SystemStats::getNumPhysicalCpus()) + " cores, " +
               juce::String (juce::SystemStats::getNumCpus()) + " threads; " +
               juce::String (juce::roundToInt (juce::SystemStats::getMemorySizeInMegabytes() / 1024.0)) + " GB memory");
#if JUCE_WINDOWS
    lines.add ("Process id: " + juce::String ((int)GetCurrentProcessId()));
#else
    lines.add ("Process id: " + juce::String ((int)getpid()));
#endif
    return lines;
}

} // namespace spinus::session
