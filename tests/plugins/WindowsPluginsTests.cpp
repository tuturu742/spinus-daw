#include <catch2/catch_test_macros.hpp>

#include "plugins/PluginScanner.h"
#include "plugins/windows/WindowsPlugins.h"

#include <iostream>

using namespace spinus::plugins;
using Format = WindowsPlugins::Format;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-windows-plugins") + "-" + juce::Uuid().toString());
};

/** A minimal PE file: 64-bit (or 32-bit) with an export table holding `exportName` (if any). */
juce::MemoryBlock makePe (uint16_t machine, const juce::String& exportName)
{
    juce::MemoryBlock pe (0x400, true);
    auto* b = static_cast<uint8_t*> (pe.getData());
    auto put16 = [b] (size_t at, unsigned v)
    { b[at] = (uint8_t)(v & 0xffu), b[at + 1] = (uint8_t)((v >> 8u) & 0xffu); };
    auto put32 = [&] (size_t at, unsigned v) { put16 (at, v & 0xffffu), put16 (at + 2, v >> 16u); };

    put16 (0, 0x5a4d);     // MZ
    put32 (0x3c, 0x40);    // PE header offset
    put32 (0x40, 0x4550);  // PE\0\0
    put16 (0x44, machine); // COFF: machine
    put16 (0x46, 1);       // one section
    put16 (0x54, 240);     // size of the optional header
    const size_t optional = 0x58;
    put16 (optional, 0x20b); // PE32+
    if (exportName.isNotEmpty())
        put32 (optional + 112, 0x1000); // export table RVA
    const size_t section = optional + 240;
    put32 (section + 8, 0x200);   // virtual size
    put32 (section + 12, 0x1000); // virtual address
    put32 (section + 16, 0x200);  // raw size
    put32 (section + 20, 0x200);  // raw offset

    // Export directory at RVA 0x1000 (file 0x200): one name.
    put32 (0x200 + 24, 1);           // number of names
    put32 (0x200 + 32, 0x1000 + 40); // address of names
    put32 (0x200 + 40, 0x1000 + 48); // the name's RVA
    for (int i = 0; i < exportName.length(); ++i)
        b[0x200 + 48 + (size_t)i] = (uint8_t)exportName[i];
    return pe;
}

void write (const juce::File& f, const juce::MemoryBlock& data)
{
    REQUIRE (f.getParentDirectory().createDirectory().wasOk());
    REQUIRE (f.replaceWithData (data.getData(), data.getSize()));
}

/** A prefix with plugins in the usual places (folder names in the case installers often use). */
void makePrefix (const WindowsPlugins& w)
{
    const auto c = w.getPrefix().getChildFile ("drive_c");
    REQUIRE (w.getPrefix().getChildFile ("system.reg").create().wasOk());
    write (c.getChildFile ("Program Files/Common Files/CLAP/Vendor/Synth.clap"), makePe (0x8664, {}));
    write (c.getChildFile ("Program Files/Common Files/CLAP/Old32.clap"), makePe (0x14c, {}));
    write (c.getChildFile ("Program Files/Common Files/VST3/Verb.vst3/Contents/x86_64-win/Verb.vst3"),
           makePe (0x8664, {}));
    write (c.getChildFile ("Program Files/Common Files/VST3/Legacy.vst3"), makePe (0x8664, {}));
    write (c.getChildFile ("Program Files/VSTPlugins/Delay.dll"), makePe (0x8664, "VSTPluginMain"));
    write (c.getChildFile ("Program Files/VSTPlugins/Helper.dll"), makePe (0x8664, "DoSomething")); // not a plugin
    write (c.getChildFile ("Program Files/Steinberg/VstPlugins/Other/Delay.dll"), makePe (0x8664, "main"));
}

WindowsPlugins::Runtime fakeRuntime (const juce::File& root)
{
    WindowsPlugins::Runtime r;
    r.kind = WindowsPlugins::Runtime::Kind::spinus;
    r.yabridge = root.getChildFile ("yabridge");
    REQUIRE (r.yabridge.createDirectory().wasOk());
    for (auto* name : { "libyabridge-chainloader-clap.so", "libyabridge-chainloader-vst2.so",
                        "libyabridge-chainloader-vst3.so", "yabridge-host.exe" })
        REQUIRE (r.yabridge.getChildFile (name).replaceWithText (name));
    r.host = r.yabridge.getChildFile ("yabridge-host.exe");
    r.wine = root.getChildFile ("wine/bin/wine");
    return r;
}
} // namespace

TEST_CASE ("Windows plugin files are told apart: 64-bit, and VST2 entry points", "[plugins][windows]")
{
    TempFolder t;
    const auto plugin = t.root.getChildFile ("a.dll"), helper = t.root.getChildFile ("b.dll"),
               old = t.root.getChildFile ("c.dll"), text = t.root.getChildFile ("d.dll");
    write (plugin, makePe (0x8664, "VSTPluginMain"));
    write (helper, makePe (0x8664, "Other"));
    write (old, makePe (0x14c, "main"));
    REQUIRE (text.replaceWithText ("not a library at all"));

    CHECK (WindowsPlugins::isWindows64 (plugin));
    CHECK (WindowsPlugins::exportsVst2Entry (plugin));
    CHECK (WindowsPlugins::isWindows64 (helper));
    CHECK (! WindowsPlugins::exportsVst2Entry (helper));
    CHECK (! WindowsPlugins::isWindows64 (old));
    CHECK (! WindowsPlugins::isWindows64 (text));
    CHECK (! WindowsPlugins::exportsVst2Entry (text));
}

TEST_CASE ("Plugins in Spinus's Windows folder are found and bridged with yabridge's layout", "[plugins][windows]")
{
    TempFolder t;
    const WindowsPlugins w (t.root.getChildFile ("windows"));
    makePrefix (w);
    CHECK (w.isPrefixReady());

    const auto found = w.findWindowsPlugins();
    juce::StringArray names;
    for (auto& f : found)
        names.add ((f.format == Format::vst2 ? "VST2 " : f.format == Format::vst3 ? "VST3 " : "CLAP ") + f.name);
    names.sort (false);
    CHECK (names == juce::StringArray { "CLAP Synth", "VST2 Delay", "VST2 Delay", "VST3 Legacy", "VST3 Verb" });

    const auto runtime = fakeRuntime (t.root);
    CHECK (w.syncBridges (runtime) == 5);

    const auto clap = w.getBridgedFolder (Format::clap);
    CHECK (clap.getChildFile ("Synth.clap").loadFileAsString() == "libyabridge-chainloader-clap.so");
    CHECK (clap.getChildFile ("Synth.clap-win").isSymbolicLink());
    CHECK (clap.getChildFile ("Synth.clap-win").getLinkedTarget().getFileName() == "Synth.clap");

    const auto vst3 = w.getBridgedFolder (Format::vst3);
    CHECK (vst3.getChildFile ("Verb.vst3/Contents/x86_64-linux/Verb.so").existsAsFile());
    CHECK (vst3.getChildFile ("Verb.vst3/Contents/x86_64-win/Verb.vst3")
               .getLinkedTarget()
               .getFullPathName()
               .endsWith ("VST3/Verb.vst3/Contents/x86_64-win/Verb.vst3"));
    CHECK (vst3.getChildFile ("Legacy.vst3/Contents/x86_64-win/Legacy.vst3").getLinkedTarget().getFileName() ==
           "Legacy.vst3");

    // Two VST2 plugins with the same file name in different folders.
    const auto vst2 = w.getBridgedFolder (Format::vst2);
    CHECK (vst2.getChildFile ("Delay.so").existsAsFile());
    CHECK (vst2.getChildFile ("Delay (2).so").existsAsFile());
    CHECK (vst2.getChildFile ("Delay (2).dll").isSymbolicLink());
    CHECK (! vst2.getChildFile ("Helper.so").exists());

    // An uninstalled plugin's bridge goes away; nothing else changes.
    REQUIRE (w.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/Vendor/Synth.clap").deleteFile());
    CHECK (w.syncBridges (runtime) == 4);
    CHECK (! clap.getChildFile ("Synth.clap").exists());
    CHECK (! clap.getChildFile ("Synth.clap-win").isSymbolicLink());
    CHECK (vst3.getChildFile ("Verb.vst3").isDirectory());
}

TEST_CASE ("Adding a Windows plugin file puts it where installers would", "[plugins][windows]")
{
    TempFolder t;
    const WindowsPlugins w (t.root.getChildFile ("windows"));
    CHECK (! w.addPluginFile (t.root.getChildFile ("x.clap")).wasOk()); // not set up yet
    makePrefix (w);

    const auto clap = t.root.getChildFile ("Downloads/New.clap");
    write (clap, makePe (0x8664, {}));
    REQUIRE (w.addPluginFile (clap).wasOk());
    CHECK (w.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/New.clap").existsAsFile());

    const auto bundle = t.root.getChildFile ("Downloads/Big.vst3");
    write (bundle.getChildFile ("Contents/x86_64-win/Big.vst3"), makePe (0x8664, {}));
    REQUIRE (w.addPluginFile (bundle).wasOk());
    CHECK (w.getPrefix()
               .getChildFile ("drive_c/Program Files/Common Files/VST3/Big.vst3/Contents/x86_64-win/Big.vst3")
               .existsAsFile());

    const auto dll = t.root.getChildFile ("Downloads/Fx.dll");
    write (dll, makePe (0x8664, "VSTPluginMain"));
    REQUIRE (w.addPluginFile (dll).wasOk());
    CHECK (w.getPrefix().getChildFile ("drive_c/Program Files/VSTPlugins/Fx.dll").existsAsFile());

    const auto old = t.root.getChildFile ("Downloads/Old.dll");
    write (old, makePe (0x14c, "main"));
    const auto refused = w.addPluginFile (old);
    CHECK (refused.getErrorMessage().contains ("not a 64-bit Windows plugin"));
    const auto helper = t.root.getChildFile ("Downloads/Helper.dll");
    write (helper, makePe (0x8664, "Other"));
    CHECK (w.addPluginFile (helper).getErrorMessage().contains ("not a VST plugin"));
    CHECK (! w.addPluginFile (t.root.getChildFile ("Downloads/readme.txt")).wasOk());
}

TEST_CASE ("The system's Wine and yabridge are found; Spinus's own runtime folder does not count", "[plugins][windows]")
{
    TempFolder t;
    const auto bin = t.root.getChildFile ("bin");
    REQUIRE (bin.createDirectory().wasOk());
    REQUIRE (bin.getChildFile ("wine").replaceWithText ("#!/bin/sh\necho wine-9.99 \\(Fake\\)\n", false, false, "\n"));
    REQUIRE (bin.getChildFile ("wine").setExecutePermission (true));
    const auto runtime = fakeRuntime (t.root);

    // yabridge from the user's own copy (XDG_DATA_HOME/yabridge), host next to it.
    const auto xdg = t.root.getChildFile ("share");
    REQUIRE (runtime.yabridge.copyDirectoryTo (xdg.getChildFile ("yabridge")));
    const auto previous = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    setenv ("XDG_DATA_HOME", xdg.getFullPathName().toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe)

    const auto system = WindowsPlugins::findSystemRuntime ({}, bin.getFullPathName() + ":/nonexistent");
    REQUIRE (system.has_value());
    const auto found = system.value_or (WindowsPlugins::Runtime {});
    CHECK (found.wine == bin.getChildFile ("wine"));
    CHECK (found.yabridge == xdg.getChildFile ("yabridge"));
    CHECK (WindowsPlugins::getWineVersion (found) == "wine-9.99 (Fake)");

    // Wine only inside the ignored folder: no system runtime.
    CHECK (! WindowsPlugins::findSystemRuntime (t.root, bin.getFullPathName()).has_value());

    if (previous.isEmpty())
        unsetenv ("XDG_DATA_HOME"); // NOLINT(concurrency-mt-unsafe)
    else
        setenv ("XDG_DATA_HOME", previous.toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe)
}

TEST_CASE ("Wine 9.22 and later with yabridge 5.1.1 are known to misplace clicks", "[plugins][windows]")
{
    using W = WindowsPlugins;
    CHECK (W::getKnownProblem ("wine-9.21 (Staging)", "5.1.1").isEmpty());
    CHECK (W::getKnownProblem ("wine-9.0", "").isEmpty());
    CHECK (W::getKnownProblem ("wine-9.22 (Staging)", "5.1.1").isNotEmpty());
    const auto problem = W::getKnownProblem ("wine-11.20 (Staging)", "5.1.1");
    CHECK (problem.contains ("wine-11.20"));
    CHECK (problem.contains ("yabridge 5.1.1"));
    CHECK (problem.contains ("clicks land in the wrong place"));
    CHECK (W::getKnownProblem ("wine-10.5", "").isNotEmpty());              // unknown yabridge: as the released ones
    CHECK (W::getKnownProblem ("wine-11.20 (Staging)", "5.1.2").isEmpty()); // a release with the fix
    CHECK (W::getKnownProblem ("", "5.1.1").isEmpty());                     // Wine does not start: said elsewhere
    W::Runtime own;
    own.kind = W::Runtime::Kind::spinus;
    CHECK (W::getYabridgeVersion (own) == "5.1.1");
}

TEST_CASE ("The runtime choice and the last test are remembered", "[plugins][windows]")
{
    TempFolder t;
    WindowsPlugins w (t.root.getChildFile ("windows"));
    CHECK (! w.getChosenRuntime().has_value());
    CHECK (! w.getRuntime().has_value());
    CHECK (w.needsRetest());

    w.setChosenRuntime (WindowsPlugins::Runtime::Kind::spinus);
    CHECK (WindowsPlugins (w.getFolder()).getChosenRuntime() == WindowsPlugins::Runtime::Kind::spinus);
    CHECK (! w.getRuntime().has_value()); // not downloaded

    // A test with a Wine that does not start fails, says why, and is saved.
    auto runtime = fakeRuntime (t.root);
    const auto result = w.test (runtime, juce::File());
    CHECK (! result.passed);
    CHECK (result.message.contains ("Wine does not start"));
    const auto last = WindowsPlugins (w.getFolder()).getLastTest();
    REQUIRE (last.has_value());
    const auto saved = last.value_or (WindowsPlugins::TestResult {});
    CHECK (! saved.passed);
    CHECK (saved.message == result.message);

    w.setChosenRuntime (std::nullopt);
    CHECK (! w.getChosenRuntime().has_value());
}

TEST_CASE ("The Flatpak keeps its own runtime choice and uses only Spinus's Wine", "[plugins][windows]")
{
    TempFolder t;
    const auto folder = t.root.getChildFile ("windows");
    WindowsPlugins native (folder);
    WindowsPlugins flatpak (folder, true);
    CHECK (flatpak.isSandboxed());
    CHECK (! flatpak.findUsableSystemRuntime().has_value()); // the computer's Wine is outside the sandbox

    // The native Spinus uses the computer's Wine: the Flatpak has nothing it can run yet.
    native.setChosenRuntime (WindowsPlugins::Runtime::Kind::system);
    CHECK (! flatpak.getChosenRuntime().has_value());
    CHECK (! flatpak.getRuntime().has_value());

    // Spinus's own runtime downloaded (by either): the Flatpak uses it, its wine64 (the 32-bit `wine` starter
    // cannot run in a Flatpak); the native choice stays.
    const auto& pack = WindowsPlugins::getRuntimePack();
    const auto dir = native.getDownloads().getPackFolder (pack);
    for (auto& f : pack.files)
        REQUIRE (dir.getChildFile (f).create().wasOk());
    REQUIRE (dir.getChildFile (".spinus-pack").replaceWithText (pack.version));
    CHECK (native.findSpinusRuntime().value_or (WindowsPlugins::Runtime {}).wine.getFileName() == "wine");
    REQUIRE (dir.getChildFile ("wine/bin/wine64").create().wasOk());
    CHECK (native.findSpinusRuntime().value_or (WindowsPlugins::Runtime {}).wine.getFileName() == "wine64");
    CHECK (flatpak.getChosenRuntime() == WindowsPlugins::Runtime::Kind::spinus);
    CHECK (native.getChosenRuntime() == WindowsPlugins::Runtime::Kind::system);

    // Choices and tests are saved apart; a native test of Spinus's Wine counts for the Flatpak until it has
    // its own.
    auto settings = juce::JSON::parse (folder.getChildFile ("settings.json"));
    auto* test = new juce::DynamicObject();
    test->setProperty ("passed", true);
    test->setProperty ("runtime", "spinus");
    test->setProperty ("wine", "wine-9.21 (Staging)");
    settings.getDynamicObject()->setProperty ("test", juce::var (test));
    REQUIRE (folder.getChildFile ("settings.json").replaceWithText (juce::JSON::toString (settings)));
    CHECK (flatpak.getLastTest().value_or (WindowsPlugins::TestResult {}).passed);

    flatpak.setChosenRuntime (WindowsPlugins::Runtime::Kind::spinus);
    CHECK (native.getChosenRuntime() == WindowsPlugins::Runtime::Kind::system);
    settings = juce::JSON::parse (folder.getChildFile ("settings.json"));
    CHECK (settings["flatpakRuntime"].toString() == "spinus");
    CHECK (settings["runtime"].toString() == "system");
}

TEST_CASE ("In the Flatpak, a 32-bit installer without Flatpak's 32-bit support is refused with what to do",
           "[plugins][windows]")
{
    TempFolder t;
    const auto setup32 = t.root.getChildFile ("AME_Installer.exe"), setup64 = t.root.getChildFile ("Setup64.exe"),
               msi = t.root.getChildFile ("Setup.msi"), text = t.root.getChildFile ("notes.exe");
    write (setup32, makePe (0x14c, {}));
    write (setup64, makePe (0x8664, {}));
    write (msi, makePe (0x14c, {})); // runs in Wine's 64-bit msiexec
    REQUIRE (text.replaceWithText ("not a program"));
    CHECK (WindowsPlugins::isWindows32 (setup32));
    CHECK (! WindowsPlugins::isWindows32 (setup64));
    CHECK (! WindowsPlugins::isWindows32 (text));

    const auto missing = t.root.getChildFile ("lib/ld-linux.so.2"), present = t.root.getChildFile ("ld-linux.so.2");
    REQUIRE (present.replaceWithText ("loader"));

    const auto problem = WindowsPlugins::getFlatpak32BitProblem (setup32, missing, "25.08");
    CHECK (problem.contains ("AME_Installer.exe is a 32-bit Windows program"));
    CHECK (problem.contains ("flatpak install flathub org.freedesktop.Platform.Compat.i386//25.08"));
    CHECK (WindowsPlugins::getFlatpak32BitCommand ("25.08") ==
           "flatpak install flathub org.freedesktop.Platform.Compat.i386//25.08");

    CHECK (WindowsPlugins::getFlatpak32BitProblem (setup32, present, "25.08").isEmpty()); // support installed
    CHECK (WindowsPlugins::getFlatpak32BitProblem (setup64, missing, "25.08").isEmpty());
    CHECK (WindowsPlugins::getFlatpak32BitProblem (msi, missing, "25.08").isEmpty());
    CHECK (WindowsPlugins::getFlatpak32BitProblem (text, missing, "25.08").isEmpty());

    // The native Spinus runs 32-bit installers with the computer's 32-bit libraries, as before.
    CHECK (WindowsPlugins (t.root.getChildFile ("windows")).getInstallerProblem (setup32).isEmpty());
}

TEST_CASE ("A Windows plugin is removed with its bridge, and nothing else", "[plugins][windows]")
{
    TempFolder t;
    const WindowsPlugins w (t.root.getChildFile ("windows"));
    makePrefix (w);
    const auto runtime = fakeRuntime (t.root);
    REQUIRE (w.syncBridges (runtime) == 5);

    auto find = [&w] (Format format, const juce::String& name)
    {
        for (auto& f : w.findWindowsPlugins())
            if (f.format == format && f.name == name)
                return f;
        return WindowsPlugins::Found {};
    };

    // VST3 bundle: the bundle in the prefix and the bridged bundle go.
    const auto verb = find (Format::vst3, "Verb");
    REQUIRE (verb.file.isDirectory());
    const auto verbBridges = w.findBridgedFiles (verb);
    REQUIRE (verbBridges.size() == 1);
    CHECK (verbBridges[0] == w.getBridgedFolder (Format::vst3).getChildFile ("Verb.vst3"));
    juce::Array<juce::File> removed;
    REQUIRE (w.removePlugin (verb, &removed).wasOk());
    CHECK (removed == verbBridges);
    CHECK (! verb.file.exists());
    CHECK (! w.getBridgedFolder (Format::vst3).getChildFile ("Verb.vst3").exists());
    CHECK (w.getBridgedFolder (Format::vst3).getChildFile ("Legacy.vst3").isDirectory());

    // CLAP: the .clap, its chainloader copy and link.
    const auto synth = find (Format::clap, "Synth");
    REQUIRE (w.removePlugin (synth).wasOk());
    CHECK (! synth.file.exists());
    CHECK (! w.getBridgedFolder (Format::clap).getChildFile ("Synth.clap").exists());
    CHECK (! w.getBridgedFolder (Format::clap).getChildFile ("Synth.clap-win").isSymbolicLink());

    // VST2: of two plugins named Delay, only the one asked for (and its own bridge, "Delay (2)").
    WindowsPlugins::Found other;
    for (auto& f : w.findWindowsPlugins())
        if (f.format == Format::vst2 && f.file.getFullPathName().contains ("/Other/"))
            other = f;
    REQUIRE (other.file.existsAsFile());
    const auto otherBridges = w.findBridgedFiles (other);
    REQUIRE (otherBridges.size() == 1);
    REQUIRE (w.removePlugin (other).wasOk());
    CHECK (! otherBridges[0].exists());
    CHECK (! otherBridges[0].withFileExtension ("dll").isSymbolicLink());
    CHECK (find (Format::vst2, "Delay").file.existsAsFile());
    const auto vst2 = w.getBridgedFolder (Format::vst2);
    CHECK (vst2.getChildFile ("Delay.so").existsAsFile() != vst2.getChildFile ("Delay (2).so").existsAsFile());

    // Nothing outside the prefix is ever deleted.
    const auto outside = t.root.getChildFile ("Elsewhere.clap");
    write (outside, makePe (0x8664, {}));
    CHECK (w.removePlugin ({ Format::clap, outside, "Elsewhere" }).failed());
    CHECK (outside.existsAsFile());
    CHECK (w.findWindowsPlugins().size() == 2);
}

TEST_CASE ("Real-time privileges are checked", "[plugins][windows]")
{
    const auto r = WindowsPlugins::checkRealtime();
    CHECK (r.memlockKiB != 0);
    CHECK (r.memlockOk == r.advice.isEmpty());
}

// Downloads Wine, yabridge and a Windows plugin (about 90 MB) and runs them: run by hand with
//   spinus_tests "[.windows-e2e]"
TEST_CASE ("Windows plugins end to end: Spinus's runtime, a prefix, a plugin, its bridge, a scan", "[.windows-e2e]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-windows-e2e");
    WindowsPlugins w (folder);
    const auto scanner = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                             .getParentDirectory()
                             .getParentDirectory()
                             .getChildFile ("spinus-plugin-scanner");
    REQUIRE (scanner.existsAsFile());

    if (! w.findSpinusRuntime())
        REQUIRE (w.getDownloads().install (WindowsPlugins::getRuntimePack()).wasOk());
    const auto own = w.findSpinusRuntime();
    REQUIRE (own.has_value());
    const auto runtime = own.value_or (WindowsPlugins::Runtime {});

    const auto start = juce::Time::getMillisecondCounter();
    const auto result = w.test (runtime, scanner, [] (const juce::String& s) { std::cout << "  " << s << "\n"; });
    INFO (result.message);
    REQUIRE (result.passed);
    std::cout << "Spinus runtime: " << result.wineVersion << ", test took "
              << (juce::Time::getMillisecondCounter() - start) / 1000.0 << " s\n";

    // The test plugin added as a user's plugin file, bridged and scanned like any plugin.
    REQUIRE (w.addPluginFile (w.getDownloads()
                                  .getPackFolder (WindowsPlugins::getTestPluginPack())
                                  .getChildFile ("DragonflyRoomReverb.clap"))
                 .wasOk());
    REQUIRE (w.syncBridges (runtime) == 1);
    WindowsPlugins::applyEnvironment (runtime);
    PluginScanner::Options options;
    options.scannerExecutable = scanner;
    juce::OwnedArray<juce::PluginDescription> found;
    REQUIRE (
        PluginScanner (options)
            .scanFile ("CLAP",
                       w.getBridgedFolder (Format::clap).getChildFile ("DragonflyRoomReverb.clap").getFullPathName(),
                       found)
            .wasOk());
    REQUIRE (found.size() == 1);
    CHECK (found[0]->name == "Dragonfly Room Reverb");

    // And the system's runtime, if there is one.
    if (const auto system = WindowsPlugins::findSystemRuntime (folder))
    {
        const auto again = w.test (*system, scanner);
        std::cout << "System runtime: " << again.wineVersion << " - " << (again.passed ? "works" : again.message)
                  << "\n";
    }
}

TEST_CASE ("An installer is waited for, not Wine's own programs or plugins open in Spinus", "[plugins][windows]")
{
    // Wine 9.21's `tasklist /fo csv /nh` in a prefix where a Windows plugin is open in Spinus.
    const juce::String idle = "\"services.exe\",\"48\",\"Console\",\"1\",\"23 624 K\"\r\n"
                              "\"explorer.exe\",\"72\",\"Console\",\"1\",\"65 272 K\"\r\n"
                              "\"winedevice.exe\",\"108\",\"Console\",\"1\",\"31 324 K\"\r\n"
                              "\"svchost.exe\",\"168\",\"Console\",\"1\",\"13 616 K\"\r\n"
                              "\"plugplay.exe\",\"188\",\"Console\",\"1\",\"25 816 K\"\r\n"
                              "\"rpcss.exe\",\"200\",\"Console\",\"1\",\"9 000 K\"\r\n"
                              "\"yabridge-host.exe\",\"230\",\"Console\",\"1\",\"90 000 K\"\r\n"
                              "\"Yabridge-Group-32.exe\",\"231\",\"Console\",\"1\",\"90 000 K\"\r\n"
                              "\"conhost.exe\",\"300\",\"Console\",\"1\",\"9 000 K\"\r\n"
                              "\"start.exe\",\"344\",\"Console\",\"1\",\"25 400 K\"\r\n"
                              "\"tasklist.exe\",\"352\",\"Console\",\"1\",\"20 364 K\"\r\n";
    CHECK (WindowsPlugins::findInstallerPrograms (idle).isEmpty());
    CHECK (WindowsPlugins::findInstallerPrograms ({}).isEmpty());

    // An installer's helpers still running.
    const auto busy = idle + "\"vcredist_x86.exe\",\"400\",\"Console\",\"1\",\"10 000 K\"\r\n"
                             "\"Setup Helper.exe\",\"404\",\"Console\",\"1\",\"10 000 K\"\r\n";
    CHECK (WindowsPlugins::findInstallerPrograms (busy) ==
           juce::StringArray { "vcredist_x86.exe", "Setup Helper.exe" });
}

TEST_CASE ("Spinus's default font reaches new and existing prefixes once", "[plugins][windows]")
{
    TempFolder t;
    WindowsPlugins w (t.root.getChildFile ("windows"));
    const auto r = fakeRuntime (t.root);

    // Without Wine's own Arial there is nothing to point at.
    CHECK (WindowsPlugins::getPrefixSettingsCommands (r).empty());

    // Wine's own Arial first in Windows's font list: the font for text in fonts the prefix doesn't have.
    const auto arial = t.root.getChildFile ("wine/share/wine/fonts/arial.ttf");
    REQUIRE (arial.getParentDirectory().createDirectory().wasOk());
    REQUIRE (arial.replaceWithText ("font"));
    const auto commands = WindowsPlugins::getPrefixSettingsCommands (r);
    REQUIRE (commands.size() == 1);
    CHECK (commands[0] == juce::StringArray { "reg", "add", "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                                              "/v", " Spinus default (TrueType)", "/t", "REG_SZ", "/d",
                                              "Z:" + arial.getFullPathName().replaceCharacter ('/', '\\'), "/f" });
    CHECK (commands[0][4] < "@"); // sorts before every font name, Wine's "@" vertical ones too

    // A stand-in Wine that logs what it is asked to do.
    const auto log = t.root.getChildFile ("wine.log");
    const auto script = juce::String ("#!/bin/sh\n"
                                      "case \"$1\" in --version) echo wine-9.21; exit 0;; esac\n"
                                      "echo \"$*\" >> '") +
                        log.getFullPathName() +
                        "'\n"
                        "if [ \"$1\" = wineboot ]; then mkdir -p \"$WINEPREFIX/drive_c\"; "
                        ": > \"$WINEPREFIX/system.reg\"; fi\n"
                        "if [ \"$1\" = reg ]; then cat \"$WINEPREFIX/reg-answer\" 2>/dev/null || "
                        "echo 'reg: The operation completed successfully'; fi\n";
    for (auto* name : { "wine", "wineserver" })
    {
        const auto f = r.wine.getSiblingFile (name);
        REQUIRE (f.getParentDirectory().createDirectory().wasOk());
        REQUIRE (f.replaceWithText (script, false, false, "\n"));
        REQUIRE (f.setExecutePermission (true));
    }
    auto calls = [&]
    {
        juce::StringArray lines;
        lines.addLines (log.loadFileAsString().trim());
        lines.removeEmptyStrings();
        [[maybe_unused]] const auto deleted = log.deleteFile();
        return lines;
    };

    // A new prefix: made, then the settings.
    const auto created = w.createPrefix (r);
    INFO (created.getErrorMessage());
    REQUIRE (created.wasOk());
    auto made = calls();
    REQUIRE (made.size() == 3);
    CHECK (made[0] == "wineboot -i");
    CHECK (made[1] == "-w");
    CHECK (made[2].startsWith ("reg add HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts /v  Spinus default "));

    // Nothing to do the next time.
    REQUIRE (w.createPrefix (r).wasOk());
    CHECK (calls().isEmpty());

    // A prefix made before the settings existed (same Wine): only the settings, without waiting for Wine's
    // server (plugins open in Spinus keep it running).
    REQUIRE (w.getPrefix().getChildFile (".spinus-settings").deleteFile());
    REQUIRE (w.createPrefix (r).wasOk());
    auto updated = calls();
    REQUIRE (updated.size() == 1);
    CHECK (updated[0].startsWith ("reg add "));

    // A prefix with older settings (an earlier Spinus) gets the new ones.
    REQUIRE (w.getPrefix().getChildFile (".spinus-settings").replaceWithText ("1"));
    REQUIRE (w.createPrefix (r).wasOk());
    CHECK (calls().size() == 1);

    // When Wine refuses, the prefix still works, and the settings are tried again the next time.
    REQUIRE (w.getPrefix().getChildFile (".spinus-settings").deleteFile());
    REQUIRE (w.getPrefix().getChildFile ("reg-answer").replaceWithText ("reg: Access denied"));
    CHECK (w.createPrefix (r).wasOk());
    CHECK (! w.getPrefix().getChildFile (".spinus-settings").exists());
    REQUIRE (w.getPrefix().getChildFile ("reg-answer").deleteFile());
    calls();
    REQUIRE (w.createPrefix (r).wasOk());
    CHECK (calls().size() == 1);
}
