#include <catch2/catch_test_macros.hpp>

#include "plugins/HostLibraries.h"
#include "plugins/PluginScanner.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "plugins/sandbox/SandboxPluginFormat.h"

#if JUCE_LINUX || JUCE_BSD
#include <gnu/libc-version.h>
#endif

using namespace spinus::plugins;

namespace
{
/** A computer's root as the Flatpak sees it under /run/host: a C library that says its version, and a loader. */
struct FakeRoot
{
    explicit FakeRoot (const char* libFolder, const char* cVersion)
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-host-" + juce::Uuid().toString()))
    {
        const auto lib = root.getChildFile (libFolder);
        lib.createDirectory();
        lib.getChildFile ("libc.so.6")
            .replaceWithText (juce::String ("\x7f"
                                            "ELF...GNU C Library (GNU libc) stable release version ") +
                              cVersion + ".\nCopyright");
        lib.getChildFile ("ld-linux-x86-64.so.2").replaceWithText ("loader");
    }
    ~FakeRoot() { root.deleteRecursively(); }
    juce::File root;
};
} // namespace

TEST_CASE ("The computer's libraries are found under the Flatpak's /run/host", "[plugins][host-libraries]")
{
    SECTION ("Arch: /usr/lib")
    {
        FakeRoot arch ("usr/lib", "2.44");
        arch.root.getChildFile ("usr/lib/dri").createDirectory();
        arch.root.getChildFile ("usr/share/glvnd/egl_vendor.d").createDirectory();
        const auto found = findHostLibraries (arch.root, "2.42");
        REQUIRE (found.has_value());
        const auto libraries = found.value_or (HostLibraries {});
        CHECK (libraries.loader == arch.root.getChildFile ("usr/lib/ld-linux-x86-64.so.2"));
        CHECK (libraries.folders == juce::StringArray (arch.root.getChildFile ("usr/lib").getFullPathName()));
        CHECK (libraries.environment.contains ("LIBGL_DRIVERS_PATH=" +
                                               arch.root.getChildFile ("usr/lib/dri").getFullPathName()));
        CHECK (libraries.environment.contains (
            "__EGL_VENDOR_LIBRARY_DIRS=" + arch.root.getChildFile ("usr/share/glvnd/egl_vendor.d").getFullPathName()));

        const juce::File scanner ("/app/lib/spinus/spinus-plugin-scanner");
        const auto command = libraries.command (scanner, { "--format", "CLAP" });
        CHECK (command[0] == "env");
        const auto loaderAt = command.indexOf (libraries.loader.getFullPathName());
        REQUIRE (loaderAt > 0);
        CHECK (command[loaderAt + 1] == "--library-path");
        CHECK (command[loaderAt + 2] == arch.root.getChildFile ("usr/lib").getFullPathName());
        CHECK (command[loaderAt + 3] == scanner.getFullPathName());
        CHECK (command[loaderAt + 4] == "--format");
    }

    SECTION ("Debian and Ubuntu: the multiarch folder")
    {
        FakeRoot debian ("usr/lib/x86_64-linux-gnu", "2.41");
        const auto found = findHostLibraries (debian.root, "2.41");
        REQUIRE (found.has_value());
        const auto libraries = found.value_or (HostLibraries {});
        CHECK (libraries.loader == debian.root.getChildFile ("usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"));
        CHECK (libraries.environment.isEmpty());
    }

    SECTION ("Not when the computer's C library is older than the Flatpak's: its plugins load there anyway")
    {
        FakeRoot old ("usr/lib64", "2.35");
        CHECK_FALSE (findHostLibraries (old.root, "2.42").has_value());
        CHECK (findHostLibraries (old.root, "2.9").has_value()); // compared as numbers, not text
    }

    SECTION ("Not without a loader")
    {
        FakeRoot noLoader ("usr/lib", "2.44");
        noLoader.root.getChildFile ("usr/lib/ld-linux-x86-64.so.2").deleteFile();
        CHECK_FALSE (findHostLibraries (noLoader.root, "2.42").has_value());
    }

    SECTION ("None outside the Flatpak")
    {
        CHECK_FALSE (getHostLibraries().has_value());
        CHECK_FALSE (isHostPlugin ("CLAP", "/run/host/usr/lib/clap/Surge XT.clap"));
    }
}

TEST_CASE ("The plugins that need the computer's libraries are kept", "[plugins][host-libraries]")
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-host-plugins-" + juce::Uuid().toString() + ".json");
    {
        HostLibraryPlugins list (file);
        list.set ("/run/host/usr/lib/clap/A.clap", true);
        list.set ("urn:b", true);
        list.set ("urn:b", false);
        list.save();
    }
    HostLibraryPlugins again (file);
    again.load();
    CHECK (again.contains ("/run/host/usr/lib/clap/A.clap"));
    CHECK_FALSE (again.contains ("urn:b"));
    file.deleteFile();
}

#if JUCE_LINUX || JUCE_BSD
TEST_CASE ("A plugin is scanned and run with another loader and its libraries", "[plugins][host-libraries]")
{
    // This computer's own loader stands for the one under /run/host: the scanner and the plugin host start
    // through it, as they do in the Flatpak.
    const auto libraries = findHostLibraries (juce::File ("/"), gnu_get_libc_version());
    if (! libraries)
        SKIP ("this system's loader is not in a usual place");
    const auto plugin = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap");

    PluginScanner::Options options;
    options.scannerExecutable = juce::File (SPINUS_PLUGIN_SCANNER);
    options.hostLibraries = libraries;
    juce::OwnedArray<juce::PluginDescription> found;
    const auto result = PluginScanner (options).scanFile ("CLAP", plugin.getFullPathName(), found, true);
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    REQUIRE (found.size() >= 1);

    // spinus-plugin-host-system, outside the Flatpak, starts the plugin host as it is.
    const auto real = *found[0];
    sandbox::SandboxPluginFormat format (juce::File (SPINUS_PLUGIN_HOST),
                                         [&] (const juce::String&, const juce::String&, int)
                                         { return std::optional<juce::PluginDescription> (real); });
    bool asked = false;
    format.setHostLibrariesLauncher (juce::File (SPINUS_PLUGIN_HOST_SYSTEM),
                                     [&] (const juce::PluginDescription&) { return asked = true; });
    juce::String error;
    auto instance =
        format.createInstanceFromDescription (sandbox::SandboxPluginFormat::describe (real), 48000.0, 256, error);
    INFO (error);
    CHECK (asked);
    REQUIRE (instance != nullptr);
    CHECK (instance->getName() == real.name);
}
#endif
