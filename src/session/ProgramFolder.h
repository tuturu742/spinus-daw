#pragma once

#include <juce_core/juce_core.h>
#include <cstring>

namespace spinus
{

/** The folder Spinus's programs are in, with its helper programs (spinus-plugin-scanner,
    spinus-plugin-host) and the AI service (ai/). Spinus is often started through a link found on the PATH
    (/usr/bin/spinus and the Flatpak's /app/bin/spinus point to lib/spinus/spinus), so the real program is
    asked for (on Linux /proc/self/exe; elsewhere the links are followed). */
inline juce::File getProgramFolder()
{
#if JUCE_LINUX || JUCE_BSD
    if (const juce::File self ("/proc/self/exe"); self.isSymbolicLink())
        if (const auto program = self.getLinkedTarget(); program.existsAsFile())
            return program.getParentDirectory();
#endif
    auto program = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    for (int i = 0; i < 8 && program.isSymbolicLink(); ++i)
        program = program.getLinkedTarget();
    return program.getParentDirectory();
}

/** One of Spinus's helper programs next to it ("spinus-plugin-scanner"; on Windows with ".exe"). */
inline juce::File getHelperProgram (const juce::File& folder, const juce::String& name)
{
#if JUCE_WINDOWS
    return folder.getChildFile (name + ".exe");
#else
    return folder.getChildFile (name);
#endif
}

inline juce::File getHelperProgram (const juce::String& name) { return getHelperProgram (getProgramFolder(), name); }

/** Where large per-user files go (downloads, models): $XDG_DATA_HOME, normally ~/.local/share, on Linux;
    %LOCALAPPDATA% on Windows (not the roaming folder: these files are big and belong to this PC). */
inline juce::File getUserDataRoot()
{
#if JUCE_WINDOWS
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
#else
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    return juce::File::isAbsolutePath (xdg)
               ? juce::File (xdg)
               : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
#endif
}

//==============================================================================
/* Where Spinus keeps its files: the one place that decides.

                      native (and AppImage)               Flatpak
   settings           ~/.config/Spinus DAW                the same folder (the home folder is shared)
   plugin list        ~/.config/Spinus DAW                ~/.var/app/<id>/config/Spinus DAW
   session logs       ~/.config/Spinus DAW/logs           ~/.var/app/<id>/config/Spinus DAW/logs
   data (downloads)   ~/.local/share/Spinus DAW           ~/.var/app/<id>/data/Spinus DAW
   Windows plugins    ~/.local/share/Spinus DAW/windows   the same folder as native

   The Flatpak keeps its own plugin list: what it can load differs (the system's plugins are under
   /run/host there, and some of them cannot run in the sandbox). Windows plugins are shared: their Windows
   folder (a Wine prefix with the installed plugins) and Spinus's own Wine are in the home folder. */

/** True inside a Flatpak sandbox. */
inline bool isFlatpak()
{
#if JUCE_LINUX || JUCE_BSD
    return juce::SystemStats::getEnvironmentVariable ("FLATPAK_ID", {}).isNotEmpty() ||
           juce::File ("/.flatpak-info").existsAsFile();
#else
    return false;
#endif
}

/** Settings: ~/.config/Spinus DAW (JUCE's folder, which does not follow XDG_CONFIG_HOME, so a Flatpak and a
    native Spinus share it); %APPDATA%\Spinus DAW on Windows. */
inline juce::File getSettingsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Spinus DAW");
}

/** The plugin list and what goes with it (plugin-list.xml, problem-plugins.xml, plugin-formats.json,
    plugins-run-separately.json): the settings folder, except in the Flatpak, which keeps its own in its
    $XDG_CONFIG_HOME (~/.var/app/<id>/config/Spinus DAW). */
inline juce::File getPluginListFolder()
{
    if (isFlatpak())
        if (const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_CONFIG_HOME", {});
            juce::File::isAbsolutePath (xdg))
            return juce::File (xdg).getChildFile ("Spinus DAW");
    return getSettingsFolder();
}

/** The session logs (core::Logs): <settings>/logs, so ~/.config/Spinus DAW/logs (Windows: %APPDATA%\Spinus DAW\logs);
    in the Flatpak its own config folder's (~/.var/app/<id>/config/Spinus DAW/logs), so a Flatpak and a native
    Spinus keep their runs apart. */
inline juce::File getLogFolder() { return getPluginListFolder().getChildFile ("logs"); }

/** Large files that are not settings (downloaded plugins): $XDG_DATA_HOME/Spinus DAW, normally
    ~/.local/share/Spinus DAW (Windows: %LOCALAPPDATA%\Spinus DAW). */
inline juce::File getDataFolder() { return getUserDataRoot().getChildFile ("Spinus DAW"); }

/** The computer's own /usr seen from the Flatpak (--filesystem=host-os:ro puts it at /run/host/usr), where
    the system's package manager installs plugins; none outside the Flatpak or without that permission. */
inline juce::File getHostUsrFolder()
{
    if (const juce::File host ("/run/host/usr"); isFlatpak() && host.getChildFile ("lib").isDirectory())
        return host;
    return {};
}

/** The system's plugin folders seen from the Flatpak for a plugin format ("VST3", "CLAP", "LV2", "VST"):
    /run/host/usr/lib/vst3 and /run/host/usr/local/lib/vst3 and so on. Empty outside the Flatpak. */
inline juce::Array<juce::File> getHostPluginFolders (const juce::String& formatName)
{
    juce::Array<juce::File> folders;
    const auto usr = getHostUsrFolder();
    const auto name = formatName == "VST3"   ? juce::String ("vst3")
                      : formatName == "CLAP" ? juce::String ("clap")
                      : formatName == "LV2"  ? juce::String ("lv2")
                      : formatName == "VST"  ? juce::String ("vst")
                                             : juce::String();
    if (usr == juce::File() || name.isEmpty())
        return folders;
    for (const auto& lib : { "lib", "local/lib" })
        if (const auto folder = usr.getChildFile (lib).getChildFile (name); folder.isDirectory())
            folders.add (folder);
    return folders;
}

/** A plugin's path as projects keep it: the computer's path, the same in the Flatpak and the native build
    (/run/host/usr/lib/vst3/X.vst3 is kept as /usr/lib/vst3/X.vst3). Paths compare through this. */
inline juce::String toPortablePluginPath (const juce::String& path)
{
    return path.startsWith ("/run/host/usr/") ? path.substring ((int)std::strlen ("/run/host")) : path;
}

/** Windows plugins through Wine: <data folder>/windows; in the Flatpak the native Spinus's
    (~/.local/share/Spinus DAW/windows), so both use one Windows folder with the same plugins. */
inline juce::File getWindowsPluginsFolder()
{
    if (isFlatpak())
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile (".local/share/Spinus DAW/windows");
    return getDataFolder().getChildFile ("windows");
}

} // namespace spinus
