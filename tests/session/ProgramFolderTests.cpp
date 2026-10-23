#include <catch2/catch_test_macros.hpp>

#include "plugins/DeviceList.h"
#include "session/ProgramFolder.h"

using namespace spinus;

TEST_CASE ("Plugin paths are kept as the computer's, the same in the Flatpak and natively", "[session][folders]")
{
    CHECK (toPortablePluginPath ("/run/host/usr/lib/vst3/Geonkick.vst3") == "/usr/lib/vst3/Geonkick.vst3");
    CHECK (toPortablePluginPath ("/run/host/usr/local/lib/clap/X.clap") == "/usr/local/lib/clap/X.clap");
    CHECK (toPortablePluginPath ("/usr/lib/vst3/Geonkick.vst3") == "/usr/lib/vst3/Geonkick.vst3");
    CHECK (toPortablePluginPath ("/home/me/.vst3/A.vst3") == "/home/me/.vst3/A.vst3");
    CHECK (toPortablePluginPath ("/run/host/etc/x") == "/run/host/etc/x"); // only the system's /usr
    CHECK (toPortablePluginPath ("urn:example:lv2") == "urn:example:lv2");

    // A device made from the Flatpak's description keeps the computer's path in the project.
    juce::PluginDescription d;
    d.pluginFormatName = "VST3";
    d.name = "Geonkick";
    d.fileOrIdentifier = "/run/host/usr/lib/vst3/Geonkick.vst3";
    CHECK (plugins::DeviceList::makeDevice (d).plugin.fileOrIdentifier == "/usr/lib/vst3/Geonkick.vst3");
}

TEST_CASE ("Outside a Flatpak, Spinus's folders are the usual ones", "[session][folders]")
{
    if (isFlatpak())
        SKIP ("running inside a Flatpak");
    CHECK (getPluginListFolder() == getSettingsFolder());
    CHECK (getWindowsPluginsFolder() == getDataFolder().getChildFile ("windows"));
    CHECK (getHostUsrFolder() == juce::File());
    CHECK (getHostPluginFolders ("VST3").isEmpty());
}
