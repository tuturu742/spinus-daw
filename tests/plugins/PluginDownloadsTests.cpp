#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/StockDevices.h"
#include "plugins/PluginDownloads.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/Session.h"
#include "ui/Browser.h"
#include "ui/PluginDownloadDialog.h"
#include "ui/Theme.h"

#include <array>

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace spinus;
using plugins::DownloadPack;
using plugins::PluginDownloads;

namespace
{
struct ArchiveEntry
{
    juce::String path;
    juce::String content;
    int permissions = 0644;
};

/** Writes a .tar.gz with the given files, as a project's release archive would be. */
juce::File writeArchive (const juce::File& file, const std::vector<ArchiveEntry>& entries)
{
    file.getParentDirectory().createDirectory();
    std::unique_ptr<struct archive, decltype (&archive_write_free)> writer (archive_write_new(), archive_write_free);
    archive_write_add_filter_gzip (writer.get());
    archive_write_set_format_pax_restricted (writer.get());
    REQUIRE (archive_write_open_filename (writer.get(), file.getFullPathName().toRawUTF8()) == ARCHIVE_OK);

    for (auto& e : entries)
    {
        std::unique_ptr<archive_entry, decltype (&archive_entry_free)> entry (archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname (entry.get(), e.path.toRawUTF8());
        archive_entry_set_size (entry.get(), (la_int64_t)e.content.getNumBytesAsUTF8());
        archive_entry_set_filetype (entry.get(), AE_IFREG);
        archive_entry_set_perm (entry.get(), (mode_t)e.permissions);
        REQUIRE (archive_write_header (writer.get(), entry.get()) == ARCHIVE_OK);
        archive_write_data (writer.get(), e.content.toRawUTF8(), e.content.getNumBytesAsUTF8());
    }
    archive_write_close (writer.get());
    return file;
}

struct Fixture
{
    Fixture()
    {
        // A plugin archive and a sounds archive, like Surge XT's.
        const auto plugin = writeArchive (temp.getChildFile ("server/plugin.tar.gz"),
                                          { { "./Fake Synth.clap", "binary", 0755 },
                                            { "./Fake Synth.vst3/Contents/x86_64-linux/Fake.so", "unused" },
                                            { "./readme.txt", "not wanted" } });
        const auto sounds = writeArchive (temp.getChildFile ("server/sounds.tar.gz"),
                                          { { "Team/Data/patches/Bass.fxp", "bass" },
                                            { "Team/Data/patches/sub/Lead.fxp", "lead" },
                                            { "Team/Other/skip.txt", "skip" },
                                            { "Team/Data/../../../escape.txt", "outside" } });

        pack.id = "fake-synth";
        pack.name = "Fake Synth";
        pack.version = "1.0";
        pack.description = "A synth for tests";
        pack.license = "GPL-3.0";
        pack.homepage = "https://example.org";
        pack.archives = { { juce::URL (plugin).toString (false),
                            PluginDownloads::sha256Of (plugin),
                            plugin.getSize(),
                            { { "Fake Synth.clap", "" } } },
                          { juce::URL (sounds).toString (false),
                            PluginDownloads::sha256Of (sounds),
                            sounds.getSize(),
                            { { "Team/Data/", "Data" } } } };
        pack.files = { "Fake Synth.clap" };
    }

    ~Fixture() { temp.deleteRecursively(); }

    juce::File temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-downloads") + "-" + juce::Uuid().toString());
    PluginDownloads downloads { temp.getChildFile ("plugins") };
    DownloadPack pack;
};
} // namespace

TEST_CASE ("A pack is downloaded, checked and unpacked into its own folder", "[plugins][downloads]")
{
    Fixture f;
    CHECK (! f.downloads.isInstalled (f.pack));

    std::vector<PluginDownloads::Progress> progress;
    const auto result = f.downloads.install (f.pack, [&] (auto& p) { progress.push_back (p); });
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    CHECK (f.downloads.isInstalled (f.pack));

    const auto folder = f.downloads.getPackFolder (f.pack);
    CHECK (folder.getChildFile ("Fake Synth.clap").loadFileAsString() == "binary");
    CHECK (access (folder.getChildFile ("Fake Synth.clap").getFullPathName().toRawUTF8(), X_OK) == 0);
    CHECK (folder.getChildFile ("Data/patches/sub/Lead.fxp").loadFileAsString() == "lead");
    // Only what the pack uses, and nothing outside its folder.
    CHECK (! folder.getChildFile ("readme.txt").exists());
    CHECK (! folder.getChildFile ("Fake Synth.vst3").exists());
    CHECK (! folder.getChildFile ("Other").exists());
    CHECK (! f.downloads.getFolder().getChildFile ("escape.txt").exists());
    CHECK (! f.temp.getChildFile ("escape.txt").exists());
    // No downloads or staging folders are left behind.
    CHECK (f.downloads.getFolder().findChildFiles (juce::File::findFilesAndDirectories, false, "*").size() == 1);

    REQUIRE (! progress.empty());
    CHECK (progress.back().total == f.pack.getDownloadSize());
    CHECK (std::any_of (progress.begin(), progress.end(), [] (auto& p) { return p.stage == "Unpacking"; }));

    // Found by the bundled devices' search, before other folders.
    const auto other = f.temp.getChildFile ("other");
    other.getChildFile ("Fake Synth.clap").create();
    CHECK (PluginDownloads::findFile ({ folder, other }, "Fake Synth.clap") == folder.getChildFile ("Fake Synth.clap"));
    CHECK (PluginDownloads::findFile ({ other, folder }, "Fake Synth.clap") == other.getChildFile ("Fake Synth.clap"));
    CHECK (PluginDownloads::findFile ({ other }, "Missing.clap") == juce::File());

    // A new version replaces it; removing deletes it.
    f.pack.version = "1.1";
    CHECK (! f.downloads.isInstalled (f.pack));
    REQUIRE (f.downloads.install (f.pack).wasOk());
    CHECK (f.downloads.isInstalled (f.pack));
    REQUIRE (f.downloads.remove (f.pack).wasOk());
    CHECK (! folder.exists());
}

TEST_CASE ("A download that is not the expected file is not installed", "[plugins][downloads]")
{
    Fixture f;
    REQUIRE (f.downloads.install (f.pack).wasOk());

    // Same version, wrong checksum: refused, and the installed copy stays.
    f.pack.archives[1].sha256 = juce::String::repeatedString ("0", 64);
    auto result = f.downloads.install (f.pack);
    CHECK (result.failed());
    CHECK (result.getErrorMessage().contains ("checksum"));
    CHECK (f.downloads.isInstalled (f.pack));

    // A file the pack needs is not in the archive.
    f.pack.archives[1].sha256 = PluginDownloads::sha256Of (f.temp.getChildFile ("server/sounds.tar.gz"));
    f.pack.files.add ("Not There.clap");
    result = f.downloads.install (f.pack);
    CHECK (result.getErrorMessage().contains ("Not There.clap"));

    // Unreachable.
    f.pack.archives[0].url = juce::URL (f.temp.getChildFile ("server/gone.tar.gz")).toString (false);
    result = f.downloads.install (f.pack);
    CHECK (result.getErrorMessage().startsWith ("Could not download"));

    // Cancelled.
    std::atomic<bool> cancel { true };
    f.pack.files = { "Fake Synth.clap" };
    f.pack.archives[0].url = juce::URL (f.temp.getChildFile ("server/plugin.tar.gz")).toString (false);
    result = f.downloads.install (f.pack, {}, &cancel);
    CHECK (result.getErrorMessage() == "Cancelled.");

    // Nothing half-done is left behind.
    CHECK (f.downloads.getFolder().findChildFiles (juce::File::findFilesAndDirectories, false, "*").size() == 1);
}

namespace
{
constexpr std::array allSystems { PluginDownloads::System::linuxX64, PluginDownloads::System::windowsX64 };

const DownloadPack* findPack (PluginDownloads::System system, const juce::String& id)
{
    for (auto& p : PluginDownloads::getPacksFor (system))
        if (p.id == id)
            return &p;
    return nullptr;
}

juce::StringArray packIds (PluginDownloads::System system)
{
    juce::StringArray ids;
    for (auto& p : PluginDownloads::getPacksFor (system))
        ids.add (p.id);
    return ids;
}

/** Some extract rule of the pack's archives puts `file` at the top of the pack's folder. */
bool isExtracted (const DownloadPack& pack, const juce::String& file)
{
    for (auto& archive : pack.archives)
        for (auto& e : archive.extract)
        {
            if (e.from.endsWith ("/"))
            {
                if (e.to.isEmpty())
                    return true; // a folder's contents, unpacked at the top
            }
            else if ((e.to.isNotEmpty() ? e.to : e.from.fromLastOccurrenceOf ("/", false, false)) == file)
            {
                return true;
            }
        }
    return false;
}
} // namespace

TEST_CASE ("Every free plugin Spinus offers comes from a download pack", "[plugins][downloads]")
{
    for (auto& device : model::getBundledDevices())
    {
        INFO (device.stockType);
        // Published for at least one system, and wherever its pack is, the pack has its file.
        bool somewhere = false;
        for (auto system : allSystems)
            if (const auto* pack = findPack (system, device.pack))
            {
                somewhere = true;
                CHECK (pack->files.contains (device.file));
            }
        CHECK (somewhere);
        CHECK (PluginDownloads::findStandardPack (device.pack) ==
               findPack (PluginDownloads::getThisSystem(), device.pack));
    }

    for (auto system : allSystems)
        for (auto& pack : PluginDownloads::getPacksFor (system))
        {
            INFO (pack.id);
            CHECK (pack.name.isNotEmpty());
            CHECK (pack.version.isNotEmpty());
            CHECK (pack.description.isNotEmpty());
            CHECK (pack.license.isNotEmpty());
            CHECK (pack.homepage.startsWith ("https://"));
            REQUIRE (! pack.files.isEmpty());
            for (auto& file : pack.files)
            {
                INFO (file);
                // Spinus loads the free plugins with its CLAP host.
                CHECK (file.endsWith (".clap"));
                CHECK (isExtracted (pack, file));
            }
            for (auto& archive : pack.archives)
            {
                INFO (archive.url);
                CHECK (archive.url.startsWith ("https://github.com/"));
                CHECK (archive.sha256.length() == 64);
                CHECK (archive.sha256.containsOnly ("0123456789abcdef"));
                CHECK (archive.size > 0);
                CHECK (! archive.extract.empty());
                // Plain archives only: Spinus never runs a project's installer.
                for (auto installer : { ".exe", ".msi", ".deb", ".rpm", ".dmg", ".pkg" })
                    CHECK (! archive.url.endsWithIgnoreCase (installer));
            }
        }
}

TEST_CASE ("Each system is offered only the free plugins published for it", "[plugins][downloads]")
{
    using System = PluginDownloads::System;
    CHECK (packIds (System::linuxX64) ==
           juce::StringArray { "surge-xt", "lsp-plugins", "dragonfly-reverb", "odin2", "ob-xf", "peakeater" });
    // LSP Plugins has no Windows build, and Odin 2 only a Windows installer.
    CHECK (packIds (System::windowsX64) == juce::StringArray { "surge-xt", "dragonfly-reverb", "ob-xf", "peakeater" });

#if JUCE_WINDOWS
    CHECK (PluginDownloads::getThisSystem() == System::windowsX64);
#else
    CHECK (PluginDownloads::getThisSystem() == System::linuxX64);
#endif
    CHECK (&PluginDownloads::getStandardPacks() == &PluginDownloads::getPacksFor (PluginDownloads::getThisSystem()));

    // Windows unpacks with JUCE alone (ArchiveReader): .zip and .tar.gz.
    for (auto& pack : PluginDownloads::getPacksFor (System::windowsX64))
        for (auto& archive : pack.archives)
        {
            INFO (archive.url);
            CHECK ((archive.url.endsWith (".zip") || archive.url.endsWith (".tar.gz")));
            CHECK (! archive.url.containsIgnoreCase ("linux"));
        }
    for (auto& pack : PluginDownloads::getPacksFor (System::linuxX64))
        for (auto& archive : pack.archives)
        {
            INFO (archive.url);
            CHECK (! archive.url.containsIgnoreCase ("win"));
        }

    // A project offered on both systems is the same release on both, and no download is used twice.
    juce::StringArray urls;
    for (auto system : allSystems)
    {
        juce::StringArray ids;
        for (auto& pack : PluginDownloads::getPacksFor (system))
        {
            INFO (pack.id);
            CHECK (! ids.contains (pack.id));
            ids.add (pack.id);
            for (auto& archive : pack.archives)
                // Surge XT's sounds and OB-Xf's patches are the same everywhere.
                if (! archive.url.contains ("portable-content") && ! archive.url.contains ("ob-xf-assets"))
                {
                    CHECK (! urls.contains (archive.url));
                    urls.add (archive.url);
                }
            for (auto other : allSystems)
                if (const auto* same = findPack (other, pack.id))
                {
                    CHECK (same->name == pack.name);
                    CHECK (same->version == pack.version);
                    CHECK (same->license == pack.license);
                    CHECK (same->homepage == pack.homepage);
                    CHECK (same->files == pack.files);
                }
        }
    }

    // The Browser lists a free plugin only where its project publishes it (or something stands in).
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    const auto& entries = session.getDeviceList().getEntries();
    for (auto& device : model::getBundledDevices())
    {
        INFO (device.stockType);
        const bool listed = std::any_of (entries.begin(), entries.end(),
                                         [&device] (auto& e) { return e.device.stockType == device.stockType; });
        CHECK (listed == (PluginDownloads::findStandardPack (device.pack) != nullptr ||
                          model::findStandIn (device.stockType) != nullptr));
    }
}

TEST_CASE ("Odin 2, OB-Xf and PeakEater are offered as pinned open-source releases", "[plugins][downloads]")
{
    using System = PluginDownloads::System;
    struct Expected
    {
        const char* stockType;
        const char* pack;
        const char* version;
        bool instrument;
        std::vector<System> systems;
    };
    const std::vector<Expected> expected {
        { "bundled.odin2", "odin2", "2.4.1", true, { System::linuxX64 } },
        { "bundled.ob-xf", "ob-xf", "1.0.3", true, { System::linuxX64, System::windowsX64 } },
        { "bundled.peakeater", "peakeater", "0.8.2", false, { System::linuxX64, System::windowsX64 } },
    };

    for (auto& e : expected)
    {
        INFO (e.stockType);
        const auto* device = model::findBundledDevice (e.stockType);
        REQUIRE (device != nullptr);
        CHECK (juce::String (device->pack) == e.pack);
        CHECK (device->isInstrument == e.instrument);
        CHECK (juce::String (device->license) == "GPL-3.0");

        for (auto system : allSystems)
        {
            const auto* pack = findPack (system, e.pack);
            const bool offered = std::find (e.systems.begin(), e.systems.end(), system) != e.systems.end();
            CHECK ((pack != nullptr) == offered);
            if (pack == nullptr)
                continue;
            CHECK (pack->version == e.version);
            CHECK (pack->license == device->license);
            CHECK (pack->files == juce::StringArray { device->file });
            // OB-Xf: its factory patches too, the same for every system (for closest preset).
            const bool patches = juce::String (e.pack) == "ob-xf";
            REQUIRE (pack->archives.size() == (patches ? 2u : 1u));
            // The project's own release of that version, for that system.
            CHECK (pack->archives[0].url.contains (e.version));
            CHECK (pack->archives[0].url.contains (system == System::windowsX64 ? "Windows" : "Linux"));
            if (patches)
            {
                CHECK (pack->archives[1].url.endsWith ("/v1.0.3/ob-xf-assets-v1.0.3.zip"));
                CHECK (pack->archives[1].sha256 == "6d324e520694167f22dc97912088bafea52bb6e348916056482a5f79434c6ec0");
                REQUIRE (pack->archives[1].extract.size() == 1);
                CHECK (pack->archives[1].extract[0].to == "Patches");
            }
        }
    }

    // Exactly the files checked when they were pinned.
    CHECK (findPack (System::linuxX64, "odin2")->archives[0].sha256 ==
           "462c4fffda99ff91a6c362d8cf41616b50807778ae7a96471753046a799aed63");
    CHECK (findPack (System::linuxX64, "ob-xf")->archives[0].sha256 ==
           "72b60c83cf6426337031df744c34a047104a9d95f1feaf6cd048ecfa39f74c96");
    CHECK (findPack (System::windowsX64, "ob-xf")->archives[0].sha256 ==
           "149fda0649daf8a3aa0851801f92515f0dbf6203acf33c0edc7d3c445e80b1e7");
    CHECK (findPack (System::linuxX64, "peakeater")->archives[0].sha256 ==
           "0ddf7d839fa436e62c2e9d5b1f188d695424d5aba2f242ad0a89bb8634f299b9");
    CHECK (findPack (System::windowsX64, "peakeater")->archives[0].sha256 ==
           "6bae14feb7ea19beed6faf4a6984168521d2deb1351db9da5c208d274254e075");
}

TEST_CASE ("The Browser offers free plugins that are not downloaded yet", "[plugins][downloads][ui]")
{
    Fixture f;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    auto& list = session.getDeviceList();

    auto find = [&] (const juce::String& name) -> const plugins::DeviceList::Entry*
    {
        for (auto& e : list.getEntries())
            if (e.name == name)
                return &e;
        return nullptr;
    };

    // Nothing downloaded: listed, but not available.
    const auto empty = f.temp.getChildFile ("empty");
    list.setBundledFolders ({ empty });
    REQUIRE (find ("Surge XT") != nullptr);
    CHECK (! find ("Surge XT")->available);
    CHECK (find ("Surge XT")->pack == "surge-xt");
    CHECK (find ("Hall Reverb")->detail.contains ("Not downloaded"));

    ui::Browser browser (session);
    browser.setSize (300, 600);
    juce::String asked;
    bool used = false;
    browser.onDownload = [&] (const juce::String& pack) { asked = pack; };
    browser.onUse = [&] (auto&) { used = true; };
    browser.setSearchText ("Surge XT");
    int row = -1;
    for (size_t i = 0; i < browser.getRows().size(); ++i)
        if (browser.getRows()[i].kind == ui::Browser::RowKind::device && browser.getRows()[i].text == "Surge XT")
            row = (int)i;
    REQUIRE (row >= 0);
    CHECK (! browser.getRows()[(size_t)row].available);
    CHECK (browser.getRows()[(size_t)row].note.contains ("get it"));
    browser.useRow (row);
    CHECK (asked == "surge-xt");
    CHECK (! used);

    // Downloaded (here: a file where Surge XT's would be): available, and an installed copy of the same
    // file is not listed twice.
    const auto folder = f.temp.getChildFile ("surge");
    folder.getChildFile ("Surge XT.clap").create();
    list.setBundledFolders ({ folder });
    CHECK (find ("Surge XT")->available);
    browser.setSearchText ("Surge XT");
    browser.useRow (row);
    CHECK (used);
}

TEST_CASE ("Get free plugins: tick, download, and see the result", "[plugins][downloads][ui]")
{
    Fixture f;
    auto other = f.pack;
    other.id = "on-system";
    other.name = "On System";
    other.files = { "System.clap" };
    const auto system = f.temp.getChildFile ("system");
    system.getChildFile ("System.clap").create();

    ui::PluginDownloadDialog dialog (f.downloads, f.downloads.getSearchFolders ({ system }), {}, { f.pack, other });
    // Missing: ticked. Installed on the system: not ticked, and said so.
    CHECK (dialog.isSelected ("fake-synth"));
    CHECK (dialog.getPackStatus ("fake-synth") == "Not downloaded");
    CHECK (! dialog.isSelected ("on-system"));
    CHECK (dialog.getPackStatus ("on-system") == "Installed on this computer");
    dialog.setSelected ("on-system", true); // cannot be ticked
    CHECK (! dialog.isSelected ("on-system"));

    int installed = 0;
    dialog.onInstalled = [&] { ++installed; };
    dialog.startDownloads();
    CHECK (dialog.isDownloading());
    dialog.finishForTests();
    CHECK (! dialog.isDownloading());
    CHECK (installed == 1);
    CHECK (dialog.getPackStatus ("fake-synth") == "Downloaded");
    CHECK (! dialog.isSelected ("fake-synth"));
    CHECK (dialog.getStatusText().contains ("Downloaded 1 plugin set"));
    CHECK (f.downloads.isInstalled (f.pack));

    // A failed download says why.
    f.downloads.remove (f.pack);
    auto broken = f.pack;
    broken.archives[0].sha256 = juce::String::repeatedString ("0", 64);
    ui::PluginDownloadDialog second (f.downloads, {}, "fake-synth", { broken });
    second.startDownloads();
    second.finishForTests();
    CHECK (second.getPackStatus ("fake-synth") == "Failed");
    CHECK (second.getStatusText().contains ("checksum"));

    // Painting works.
    CHECK (second.createComponentSnapshot (second.getLocalBounds()).isValid());
}

TEST_CASE ("Removing a pack deletes its folder, plugin files first", "[plugins][downloads]")
{
    Fixture f;
    REQUIRE (f.downloads.install (f.pack).wasOk());
    const auto folder = f.downloads.getPackFolder (f.pack);
    // Restore the permissions whatever happens, so the fixture can clean up.
    const juce::ScopeGuard writable { [&folder]
                                      {
                                          chmod (folder.getFullPathName().toRawUTF8(), 0755);
                                          chmod (folder.getChildFile ("Data/patches").getFullPathName().toRawUTF8(),
                                                 0755);
                                      } };

    // The plugin file cannot be deleted (here: the folder is read-only; on Windows: it is loaded): nothing is,
    // and the pack is still installed.
    REQUIRE (chmod (folder.getFullPathName().toRawUTF8(), 0555) == 0);
    auto result = f.downloads.remove (f.pack);
    CHECK (result.failed());
    CHECK (result.getErrorMessage().contains ("Fake Synth.clap"));
    CHECK (f.downloads.isInstalled (f.pack));
    CHECK (folder.getChildFile ("Data/patches/Bass.fxp").existsAsFile());
    REQUIRE (chmod (folder.getFullPathName().toRawUTF8(), 0755) == 0);

    // The plugin goes, but some sounds stay: removed, and the leftovers are named.
    REQUIRE (chmod (folder.getChildFile ("Data/patches").getFullPathName().toRawUTF8(), 0555) == 0);
    result = f.downloads.remove (f.pack);
    chmod (folder.getChildFile ("Data/patches").getFullPathName().toRawUTF8(), 0755);
    CHECK (result.failed());
    CHECK (result.getErrorMessage().startsWith ("Removed Fake Synth, but"));
    CHECK (result.getErrorMessage().contains ("Bass.fxp"));
    CHECK (! f.downloads.isInstalled (f.pack));
    CHECK (! folder.getChildFile ("Fake Synth.clap").exists());

    // The rest; and removing what is not there is fine.
    CHECK (f.downloads.remove (f.pack).wasOk());
    CHECK (! folder.exists());
    CHECK (f.downloads.remove (f.pack).wasOk());
}

TEST_CASE ("Get free plugins: remove a downloaded pack, after asking", "[plugins][downloads][ui]")
{
    Fixture f;
    REQUIRE (f.downloads.install (f.pack).wasOk());
    auto missing = f.pack;
    missing.id = "missing";
    missing.name = "Missing";
    missing.files = { "Missing.clap" };
    auto other = f.pack;
    other.id = "on-system";
    other.name = "On System";
    other.files = { "System.clap" };
    const auto system = f.temp.getChildFile ("system");
    system.getChildFile ("System.clap").create();

    ui::PluginDownloadDialog dialog (f.downloads, f.downloads.getSearchFolders ({ system }), {},
                                     { f.pack, missing, other });
    dialog.setSelected ("missing", false);
    int removed = 0, checked = 0;
    dialog.onRemoved = [&] { ++removed; };
    dialog.checkRemove = [&] (const DownloadPack&)
    {
        ++checked;
        return juce::String();
    };

    // Only what Spinus downloaded has Remove: not a pack that is missing or installed by the system.
    CHECK (dialog.getPackStatus ("fake-synth") == "Downloaded");
    CHECK (dialog.canRemove ("fake-synth"));
    CHECK (! dialog.canRemove ("missing"));
    CHECK (! dialog.canRemove ("on-system"));
    dialog.askToRemove ("on-system");
    CHECK (! dialog.isAskingToRemove ("on-system"));

    // Remove asks first; Keep keeps it.
    dialog.askToRemove ("fake-synth");
    CHECK (dialog.isAskingToRemove ("fake-synth"));
    CHECK (dialog.getRowMessage ("fake-synth").startsWith ("Remove Fake Synth?"));
    CHECK (dialog.getRowMessage ("fake-synth").contains ("missing"));
    CHECK (dialog.createComponentSnapshot (dialog.getLocalBounds()).isValid());
    dialog.keepPack ("fake-synth");
    CHECK (! dialog.isAskingToRemove ("fake-synth"));
    CHECK (dialog.getRowMessage ("fake-synth").isEmpty());
    CHECK (f.downloads.isInstalled (f.pack));
    CHECK (removed == 0);

    // Remove, then Remove again: deleted, shown as not downloaded with its tick box, and the lists refresh.
    dialog.askToRemove ("fake-synth");
    dialog.confirmRemove ("fake-synth");
    CHECK (checked == 3); // asked twice, and again just before deleting
    CHECK (! f.downloads.getPackFolder (f.pack).exists());
    CHECK (removed == 1);
    CHECK (dialog.getPackStatus ("fake-synth") == "Not downloaded");
    CHECK (! dialog.canRemove ("fake-synth"));
    CHECK (dialog.getStatusText() == "Removed Fake Synth.");
    dialog.setSelected ("fake-synth", true);
    CHECK (dialog.isSelected ("fake-synth"));

    // Not while downloading.
    dialog.startDownloads();
    CHECK (! dialog.canRemove ("fake-synth"));
    dialog.finishForTests();
    CHECK (f.downloads.isInstalled (f.pack));
    CHECK (dialog.canRemove ("fake-synth"));
}

TEST_CASE ("Get free plugins: a pack the open project uses is not removed", "[plugins][downloads][ui]")
{
    Fixture f;
    REQUIRE (f.downloads.install (f.pack).wasOk());
    const auto folder = f.downloads.getPackFolder (f.pack);

    // What uses a pack: its bundled devices, or a plugin loaded from a file in its folder.
    model::Project project;
    const auto surge = PluginDownloads::findStandardPack ("surge-xt");
    REQUIRE (surge != nullptr);
    CHECK (ui::PluginDownloadDialog::describeUseInProject (project, *surge, folder).isEmpty());
    for (auto name : { "Lead", "Pad", "Drums" })
    {
        model::Channel channel;
        channel.name = name;
        if (juce::String (name) != "Drums")
        {
            channel.instrument = model::Device {};
            channel.instrument->stockType = "bundled.surge-xt";
        }
        else
        {
            channel.instrument = model::Device {};
            channel.instrument->stockType = "spinus.sampler";
        }
        project.channels.push_back (channel);
    }
    model::Insert bus;
    bus.name = "Bus";
    bus.effects.push_back ({ model::Device {}, 1.0f });
    bus.effects.back().device.stockType = "bundled.surge-xt-effects";
    project.mixer.inserts.push_back (bus);
    const auto surgeUse = ui::PluginDownloadDialog::describeUseInProject (project, *surge, folder);
    CHECK (
        surgeUse.startsWith ("Surge XT is used by 2 channels (Lead, Pad) and 1 mixer insert (Bus) in this project."));
    CHECK (ui::PluginDownloadDialog::describeUseInProject (project, *PluginDownloads::findStandardPack ("peakeater"),
                                                           folder)
               .isEmpty());

    model::Device plugin;
    plugin.kind = model::Device::Kind::plugin;
    plugin.plugin.format = "CLAP";
    plugin.plugin.fileOrIdentifier = folder.getChildFile ("Fake Synth.clap").getFullPathName();
    project.mixer.master.effects.push_back ({ plugin, 1.0f });
    CHECK (ui::PluginDownloadDialog::describeUseInProject (project, f.pack, folder)
               .startsWith ("Fake Synth is used by 1 mixer insert (Master)"));

    // In use: Remove says why instead of asking, and nothing is deleted.
    ui::PluginDownloadDialog dialog (f.downloads, f.downloads.getSearchFolders(), {}, { f.pack });
    int removed = 0;
    dialog.onRemoved = [&] { ++removed; };
    dialog.checkRemove = [&] (const DownloadPack& pack)
    { return ui::PluginDownloadDialog::describeUseInProject (project, pack, f.downloads.getPackFolder (pack)); };
    dialog.askToRemove ("fake-synth");
    CHECK (! dialog.isAskingToRemove ("fake-synth"));
    CHECK (dialog.getRowMessage ("fake-synth").contains ("used by 1 mixer insert (Master) in this project"));
    dialog.confirmRemove ("fake-synth");
    CHECK (f.downloads.isInstalled (f.pack));
    dialog.keepPack ("fake-synth"); // OK
    CHECK (dialog.getRowMessage ("fake-synth").isEmpty());

    // Taken out of the project: it asks. Put back before the answer: checked again, and kept.
    project.mixer.master.effects.clear();
    dialog.askToRemove ("fake-synth");
    CHECK (dialog.isAskingToRemove ("fake-synth"));
    project.mixer.master.effects.push_back ({ plugin, 1.0f });
    dialog.confirmRemove ("fake-synth");
    CHECK (! dialog.isAskingToRemove ("fake-synth"));
    CHECK (dialog.getRowMessage ("fake-synth").contains ("in this project"));
    CHECK (f.downloads.isInstalled (f.pack));
    CHECK (removed == 0);
}

#if defined(SPINUS_PLUGINS_DIR)
TEST_CASE ("Get free plugins: a pack whose plugin is loaded is not removed", "[plugins][downloads][ui]")
{
    const auto built = juce::File (SPINUS_PLUGINS_DIR).getChildFile ("DragonflyRoomReverb.clap");
    if (! built.existsAsFile())
        SKIP ("bundled plugins not built");

    // A pack with a real plugin in it, loaded as a hidden copy (the Analyzer's, say) would be.
    Fixture f;
    f.pack.files = { "DragonflyRoomReverb.clap" };
    const auto folder = f.downloads.getPackFolder (f.pack);
    REQUIRE (folder.createDirectory());
    REQUIRE (built.copyFileTo (folder.getChildFile ("DragonflyRoomReverb.clap")));
    REQUIRE (folder.getChildFile (".spinus-pack").replaceWithText (f.pack.version));
    REQUIRE (f.downloads.isInstalled (f.pack));

    plugins::ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    clap.findAllTypesForFile (found, folder.getChildFile ("DragonflyRoomReverb.clap").getFullPathName());
    REQUIRE (found.size() == 1);
    CHECK (! ui::PluginDownloadDialog::isLoadedFrom (folder)); // describing it does not keep it loaded
    juce::String error;
    auto instance = clap.createInstanceFromDescription (*found[0], 48000.0, 512, error);
    REQUIRE (instance != nullptr);
    CHECK (ui::PluginDownloadDialog::isLoadedFrom (folder));

    ui::PluginDownloadDialog dialog (f.downloads, f.downloads.getSearchFolders(), {}, { f.pack });
    dialog.askToRemove ("fake-synth");
    CHECK (! dialog.isAskingToRemove ("fake-synth"));
    CHECK (dialog.getRowMessage ("fake-synth").contains ("still running"));
    dialog.keepPack ("fake-synth");

    // Let go: it can be removed.
    instance = nullptr;
    CHECK (! ui::PluginDownloadDialog::isLoadedFrom (folder));
    dialog.askToRemove ("fake-synth");
    REQUIRE (dialog.isAskingToRemove ("fake-synth"));
    dialog.confirmRemove ("fake-synth");
    CHECK (! folder.exists());
}
#endif

// Downloads this system's real packs (about 490 MB on Linux) and creates every bundled device from them. Hidden: run it
// by hand with  spinus_tests "[.network]"  after changing a pinned version.
TEST_CASE ("The real packs download and every bundled device loads from them", "[.network]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile (juce::String ("spinus-real-downloads") + "-" + juce::Uuid().toString());
    const juce::ScopeGuard cleanUp { [&folder] { folder.deleteRecursively(); } };
    PluginDownloads downloads (folder);
    for (auto& pack : PluginDownloads::getStandardPacks())
    {
        INFO (pack.id);
        const auto result = downloads.install (pack);
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
    }

    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), downloads.getSearchFolders());
    auto* factory = devices.get();
    Session session (host.getEngine(), std::move (devices));
    for (auto& device : model::getBundledDevices())
    {
        INFO (device.stockType);
        model::Device d;
        d.kind = model::Device::Kind::stock;
        d.stockType = device.stockType;
        CHECK (factory->create (session.getEdit(), d) != nullptr);
    }
}

// Downloads every system's packs (about 900 MB) and unpacks them, without loading any plugin: this checks
// the other system's pins too. Hidden: run it by hand with  spinus_tests "[unpack]"  after changing a
// pinned version (add  -c "<system> <pack id>"  for one pack, e.g.  -c "windows ob-xf").
TEST_CASE ("Every system's real packs download and unpack", "[.network][unpack]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile (juce::String ("spinus-real-unpack") + "-" + juce::Uuid().toString());
    const juce::ScopeGuard cleanUp { [&folder] { folder.deleteRecursively(); } };
    for (auto system : allSystems)
    {
        const juce::String name = system == PluginDownloads::System::windowsX64 ? "windows" : "linux";
        const PluginDownloads downloads (folder.getChildFile (name));
        for (auto& pack : PluginDownloads::getPacksFor (system))
            DYNAMIC_SECTION (name << " " << pack.id)
            {
                const auto result = downloads.install (pack);
                INFO (result.getErrorMessage());
                REQUIRE (result.wasOk());
                CHECK (downloads.isInstalled (pack));
                CHECK (downloads.remove (pack).wasOk());
            }
    }
}

TEST_CASE ("Get free plugins window screenshot for the manual", "[.screenshot][downloads]")
{
    const auto folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile (juce::String ("spinus-screenshot-downloads") + "-" + juce::Uuid().toString());
    const juce::ScopeGuard cleanUp { [&folder] { folder.deleteRecursively(); } };
    // Dragonfly Reverb already downloaded (its marker and files), the others not.
    const PluginDownloads downloads (folder);
    const auto* dragonfly = PluginDownloads::findStandardPack ("dragonfly-reverb");
    REQUIRE (dragonfly != nullptr);
    for (auto& f : dragonfly->files)
        REQUIRE (downloads.getPackFolder (*dragonfly).getChildFile (f).create().wasOk());
    REQUIRE (downloads.getPackFolder (*dragonfly).getChildFile (".spinus-pack").replaceWithText (dragonfly->version));

    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    ui::PluginDownloadDialog dialog (downloads, downloads.getSearchFolders());
    dialog.setLookAndFeel (&lookAndFeel);
    const auto save = [&dialog] (const juce::String& name)
    {
        const auto image = dialog.createComponentSnapshot (dialog.getLocalBounds(), true, 1.0f);
        const auto file =
            juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/" + name);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    };
    save ("free-plugins.png");
    // Remove pressed for Dragonfly Reverb: the row asks first.
    dialog.askToRemove ("dragonfly-reverb");
    REQUIRE (dialog.isAskingToRemove ("dragonfly-reverb"));
    save ("free-plugins-remove.png");
    dialog.setLookAndFeel (nullptr);
}

TEST_CASE ("Get free plugins, opened for a project, ticks just the packs it needs", "[plugins][downloads][ui]")
{
    Fixture f;
    auto second = f.pack, third = f.pack;
    second.id = "second";
    second.files = { "Second.clap" };
    third.id = "third";
    third.files = { "Third.clap" };

    ui::PluginDownloadDialog one (f.downloads, f.downloads.getSearchFolders(), "second", { f.pack, second, third });
    CHECK (! one.isSelected ("fake-synth"));
    CHECK (one.isSelected ("second"));
    CHECK (! one.isSelected ("third"));

    ui::PluginDownloadDialog two (f.downloads, f.downloads.getSearchFolders(), "fake-synth,third",
                                  { f.pack, second, third });
    CHECK (two.isSelected ("fake-synth"));
    CHECK (! two.isSelected ("second"));
    CHECK (two.isSelected ("third"));
}
