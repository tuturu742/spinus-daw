#include <catch2/catch_test_macros.hpp>

#include "core/ChildProcess.h"
#include "core/DBus.h"
#include "core/FileDialog.h"
#include "core/FileDialogPortal.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

using spinus::core::FileDialog;
namespace dbus = spinus::core::dbus;
namespace portal = spinus::core::portal;

namespace
{
using Browser = juce::FileBrowserComponent;
constexpr int openFiles = (int)((unsigned)Browser::openMode | (unsigned)Browser::canSelectFiles);
constexpr int openMany = (int)((unsigned)openFiles | (unsigned)Browser::canSelectMultipleItems);
constexpr int openFolder = (int)((unsigned)Browser::openMode | (unsigned)Browser::canSelectDirectories);
constexpr int saveFile = (int)((unsigned)Browser::saveMode | (unsigned)Browser::canSelectFiles);

template <typename Condition> bool pumpUntil (Condition done, int timeoutMs = 10000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)timeoutMs;
    while (! done() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    return done();
}

juce::FileChooserDialogBox* findOwnBrowser()
{
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* box = dynamic_cast<juce::FileChooserDialogBox*> (juce::Desktop::getInstance().getComponent (i)))
            return box;
    return nullptr;
}

/** The options of a request as the portal receives them. */
juce::var sentOptions (const portal::Request& r, juce::Array<juce::var>* all = nullptr)
{
    const auto message = dbus::roundTrip ("OpenFile", portal::callArguments (r));
    if (! message.has_value())
    {
        FAIL ("libdbus could not build the call");
        return {};
    }
    const auto& m = *message;
    CHECK (m.signature == "ssa{sv}");
    if (all != nullptr)
        *all = m.arguments;
    return m.arguments[2];
}

/** A D-Bus daemon of the test's own, so no test talks to the user's session bus. */
struct PrivateBus
{
    PrivateBus()
    {
        folder.createDirectory();
        const auto config = folder.getChildFile ("bus.conf");
        config.replaceWithText ("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
                                "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                                "<busconfig><type>session</type><listen>unix:dir=" +
                                folder.getFullPathName() +
                                "</listen><auth>EXTERNAL</auth><policy context=\"default\">"
                                "<allow send_destination=\"*\" eavesdrop=\"true\"/><allow eavesdrop=\"true\"/>"
                                "<allow own=\"*\"/></policy></busconfig>\n");
        const juce::File system ("/usr/bin/dbus-daemon");
        const auto program = system.existsAsFile() ? system.getFullPathName() : juce::String ("dbus-daemon");
        if (! daemon.start (juce::StringArray { program, "--config-file=" + config.getFullPathName(), "--nofork",
                                                "--print-address=1" },
                            spinus::core::ChildProcess::wantStdOut))
            return;
        juce::String line;
        char c = 0;
        while (! line.endsWithChar ('\n') && daemon.readProcessOutput (&c, 1) == 1)
            line += juce::String::charToString ((juce::juce_wchar)(unsigned char)c);
        address = line.trim();
    }

    ~PrivateBus()
    {
        daemon.kill();
        daemon.waitForProcessToFinish (5000);
        std::error_code ignored; // with the bus's socket, which JUCE's deleteRecursively leaves
        std::filesystem::remove_all (folder.getFullPathName().toStdString(), ignored);
    }

    juce::File folder { juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-bus-" + juce::Uuid().toString()) };
    spinus::core::ChildProcess daemon;
    juce::String address;
};

/** Points FileDialog at a bus for one test, and back at none after it. */
struct UseBus
{
    explicit UseBus (const juce::String& address) { FileDialog::setBusAddress (address); }
    ~UseBus()
    {
        FileDialog::setBusAddress (juce::String());
        pumpUntil ([] { return FileDialog::getPortalRequestsRunning() == 0; });
    }
};

/** A stand-in for xdg-desktop-portal: answers the version and OpenFile/SaveFile, then sends Response. */
struct FakePortal
{
    explicit FakePortal (const juce::String& address)
    {
        juce::String error;
        bus = dbus::Connection::open (address, error);
        REQUIRE (bus != nullptr);
        REQUIRE (bus->requestName ("org.freedesktop.portal.Desktop"));
        thread = std::thread ([this] { run(); });
    }

    ~FakePortal()
    {
        stop = true;
        thread.join();
    }

    void run()
    {
        while (! stop)
        {
            const auto m = bus->next (20);
            if (! m || m->type != dbus::Message::methodCall)
                continue;
            if (m->member == "Get")
                bus->reply (*m, { dbus::Value::variant (dbus::Value::uint32 (4)) });
            else if ((m->member == "OpenFile" || m->member == "SaveFile") && failCalls)
                bus->replyError (*m, "org.freedesktop.portal.Error.Failed", "no backend");
            else if (m->member == "OpenFile" || m->member == "SaveFile")
            {
                const auto token = m->arguments[2].getProperty ("handle_token", {}).toString();
                const auto handle = portal::requestPath (m->sender, token);
                juce::StringArray answer;
                {
                    std::scoped_lock lock (mutex);
                    method = m->member;
                    arguments = m->arguments;
                    requestHandle = handle;
                    answer = uris;
                }
                bus->reply (*m, { dbus::Value::objectPath (handle) });
                if (respond)
                {
                    std::vector<dbus::Value> uriValues;
                    for (auto& u : answer)
                        uriValues.push_back (dbus::Value::string (u));
                    bus->emitSignal (
                        handle, "org.freedesktop.portal.Request", "Response",
                        { dbus::Value::uint32 (code),
                          dbus::Value::dictionary ({ { "uris", dbus::Value::array ("s", std::move (uriValues)) } }) });
                }
                ++calls;
            }
            else if (m->member == "Close" && m->interface == "org.freedesktop.portal.Request")
            {
                std::scoped_lock lock (mutex);
                closed = m->path;
            }
        }
    }

    juce::String getClosed()
    {
        std::scoped_lock lock (mutex);
        return closed;
    }

    // Set before the dialog opens:
    std::atomic<bool> respond { true };
    std::atomic<bool> failCalls { false };
    std::atomic<juce::uint32> code { 0 };
    juce::StringArray uris; // with `mutex`

    void setUris (const juce::StringArray& u)
    {
        std::scoped_lock lock (mutex);
        uris = u;
    }

    std::atomic<int> calls { 0 };
    std::mutex mutex;
    juce::String method, requestHandle, closed;
    juce::Array<juce::var> arguments;

    std::unique_ptr<dbus::Connection> bus;
    std::atomic<bool> stop { false };
    std::thread thread;
};
} // namespace

TEST_CASE ("A file dialog's portal request: title, filters, where it starts, what it chooses", "[core][filedialog]")
{
    if (! dbus::isAvailable())
        SKIP ("libdbus-1.so.3 is not installed");

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-dialog-" + juce::Uuid().toString());
    folder.createDirectory();

    // Opening several audio files from a folder.
    auto r = portal::makeRequest ("Add samples", folder, "*.wav;*.FLAC", openMany);
    r.handleToken = "spinus1";
    r.parentWindow = "x11:4a00007";
    juce::Array<juce::var> all;
    auto options = sentOptions (r, &all);
    CHECK (all[0].toString() == "x11:4a00007");
    CHECK (all[1].toString() == "Add samples");
    CHECK (options["handle_token"].toString() == "spinus1");
    CHECK ((bool)options["modal"]);
    CHECK ((bool)options["multiple"]);
    CHECK (! options.hasProperty ("directory"));
    CHECK (! options.hasProperty ("current_name"));
    CHECK (options["current_folder"].toString() == folder.getFullPathName());
    REQUIRE (options["filters"].size() == 1);
    const auto filter = options["filters"][0];
    CHECK (filter[0].toString() == "*.wav, *.FLAC");
    REQUIRE (filter[1].size() == 2);
    CHECK ((int)filter[1][0][0] == 0); // a glob
    CHECK (filter[1][0][1].toString() == "*.[wW][aA][vV]");
    CHECK (filter[1][1][1].toString() == "*.[fF][lL][aA][cC]");
    CHECK (options["current_filter"][0].toString() == "*.wav, *.FLAC");

    // One file: not multiple.
    CHECK (! (bool)sentOptions (portal::makeRequest ("Load a loop", {}, "*.wav", openFiles))["multiple"]);

    // A folder: no filters.
    options = sentOptions (portal::makeRequest ("Add a folder of samples", folder, "*.wav", openFolder));
    CHECK ((bool)options["directory"]);
    CHECK (! options.hasProperty ("filters"));

    // Saving: the suggested name and its folder, not "multiple"; a folder that is not there is not sent.
    r = portal::makeRequest ("Export audio as", folder.getChildFile ("Song.wav"), "*.wav", saveFile);
    CHECK (r.save);
    options = sentOptions (r);
    CHECK (options["current_name"].toString() == "Song.wav");
    CHECK (options["current_folder"].toString() == folder.getFullPathName());
    CHECK (! options.hasProperty ("multiple"));
    options =
        sentOptions (portal::makeRequest ("Save project as", folder.getChildFile ("missing/Untitled"), {}, saveFile));
    CHECK (options["current_name"].toString() == "Untitled");
    CHECK (! options.hasProperty ("current_folder"));
    CHECK (! options.hasProperty ("filters"));

    // "All files" patterns need no filter; a folder with non-ASCII letters arrives intact.
    CHECK (! sentOptions (portal::makeRequest ("Any", {}, "*", openFiles)).hasProperty ("filters"));
    const auto named = folder.getChildFile (juce::CharPointer_UTF8 ("Mus\xc3\xad"
                                                                    "ca y m\xc3\xa1s"));
    named.createDirectory();
    CHECK (sentOptions (portal::makeRequest ("Open", named, {}, openFiles))["current_folder"].toString() ==
           named.getFullPathName());

    folder.deleteRecursively();
}

TEST_CASE ("Portal details: patterns in any case, request paths, which dialogs it can show", "[core][filedialog]")
{
    CHECK (portal::caseInsensitiveGlob ("*.spn") == "*.[sS][pP][nN]");
    CHECK (portal::caseInsensitiveGlob ("project.json") == "[pP][rR][oO][jJ][eE][cC][tT].[jJ][sS][oO][nN]");
    CHECK (portal::caseInsensitiveGlob ("*.[ab]x") == "*.[ab][xX]");
    CHECK (portal::caseInsensitiveGlob ("*.7z") == "*.7[zZ]");

    CHECK (portal::requestPath (":1.42", "spinus7") == "/org/freedesktop/portal/desktop/request/1_42/spinus7");

    CHECK (portal::canShow (openFiles));
    CHECK (portal::canShow (openMany));
    CHECK (portal::canShow (openFolder));
    CHECK (portal::canShow (saveFile));
    CHECK (! portal::canShow ((int)((unsigned)openFiles | (unsigned)Browser::canSelectDirectories)));
}

TEST_CASE ("Portal responses: chosen files, cancelled, failed", "[core][filedialog]")
{
    auto results = [] (const juce::StringArray& uris)
    {
        auto* o = new juce::DynamicObject();
        juce::Array<juce::var> list;
        for (auto& u : uris)
            list.add (u);
        o->setProperty ("uris", list);
        return juce::var (o);
    };

    auto r = portal::parseResponse (
        { (juce::int64)0, results ({ "file:///home/me/My%20Song%2B1.wav", "https://example.com/a.wav",
                                     "file:///home/me/M%C3%BAsica/b.flac" }) });
    CHECK (r.code == portal::Response::chosen);
    REQUIRE (r.files.size() == 2); // the web address is not a file
    CHECK (r.files[0].getFullPathName() == "/home/me/My Song+1.wav");
    CHECK (r.files[1].getFullPathName() == juce::String (juce::CharPointer_UTF8 ("/home/me/M\xc3\xba"
                                                                                 "sica/b.flac")));

    CHECK (portal::parseResponse ({ (juce::int64)1, results ({}) }).code == portal::Response::cancelled);
    CHECK (portal::parseResponse ({ (juce::int64)2, results ({}) }).code == portal::Response::ended);
    CHECK (portal::parseResponse ({ (juce::int64)3, results ({}) }).code == portal::Response::ended);
    CHECK (portal::parseResponse ({ (juce::int64)1, results ({ "file:///a" }) }).files.isEmpty());
    CHECK (portal::parseResponse ({}).code == portal::Response::ended);

    CHECK (portal::fileFromUri ("file://localhost/tmp/x") == juce::File ("/tmp/x"));
    CHECK (portal::fileFromUri ("file://otherhost/tmp/x") == juce::File());
    CHECK (portal::fileFromUri ("file:///tmp/100%") == juce::File ("/tmp/100%"));

    const juce::File docs ("/run/user/1000/doc");
    CHECK (portal::documentId (juce::File ("/run/user/1000/doc/a1b2c3/Song.spn"), docs) == "a1b2c3");
    CHECK (portal::documentId (juce::File ("/home/me/Song.spn"), docs).isEmpty());
}

#if JUCE_LINUX
TEST_CASE ("File dialogs use Spinus's own browser without a portal", "[core][filedialog][ui]")
{
    // No bus at all (what the tests use everywhere): Spinus's own browser at once.
    {
        UseBus none ("");
        FileDialog dialog ("Load a loop", {}, "*.wav");
        bool called = false;
        dialog.launchAsync (openFiles, [&] (const FileDialog& d) { called = d.getResults().isEmpty(); });
        CHECK (dialog.getKind() == FileDialog::Kind::ownBrowser);
        auto* box = findOwnBrowser();
        REQUIRE (box != nullptr);
        box->exitModalState (0);
        CHECK (pumpUntil ([&] { return called; }));
    }

    // A dialog that takes files or folders: the portal cannot show it.
    {
        UseBus bus ("unix:path=/nonexistent/bus");
        FileDialog dialog ("Choose a plugin file or folder");
        dialog.launchAsync ((int)((unsigned)openFiles | (unsigned)Browser::canSelectDirectories),
                            [] (const FileDialog&) {});
        CHECK (dialog.getKind() == FileDialog::Kind::ownBrowser);
        REQUIRE (findOwnBrowser() != nullptr);
        findOwnBrowser()->exitModalState (0);
        pumpUntil ([] { return findOwnBrowser() == nullptr; });
    }

    if (! dbus::isAvailable())
        SKIP ("libdbus-1.so.3 is not installed");

    // A bus without a portal on it (a bare window manager): the portal is asked, then Spinus's own browser.
    PrivateBus bus;
    if (bus.address.isEmpty())
        SKIP ("dbus-daemon is not installed");
    UseBus use (bus.address);
    FileDialog dialog ("Add samples", {}, "*.wav");
    bool called = false;
    dialog.launchAsync (openMany, [&] (const FileDialog&) { called = true; });
    CHECK (dialog.getKind() == FileDialog::Kind::portal);
    REQUIRE (pumpUntil ([] { return findOwnBrowser() != nullptr; }));
    CHECK (dialog.getKind() == FileDialog::Kind::ownBrowser);
    INFO (dialog.getFallbackReason());
    CHECK (dialog.getFallbackReason().contains ("no desktop file dialog"));
    CHECK (! called);
    findOwnBrowser()->exitModalState (0);
    CHECK (pumpUntil ([&] { return called; }));
}

TEST_CASE ("File dialogs through a portal: chosen, cancelled, closed, failing", "[core][filedialog][ui]")
{
    if (! dbus::isAvailable())
        SKIP ("libdbus-1.so.3 is not installed");
    PrivateBus bus;
    if (bus.address.isEmpty())
        SKIP ("dbus-daemon is not installed");
    UseBus use (bus.address);
    FakePortal fake (bus.address);

    // Chosen: the files, and what was asked for.
    {
        fake.setUris ({ "file:///tmp/one%20a.wav", "file:///tmp/two.wav" });
        FileDialog dialog ("Add samples", {}, "*.wav");
        juce::Array<juce::File> chosen;
        bool called = false;
        dialog.launchAsync (openMany,
                            [&] (const FileDialog& d)
                            {
                                chosen = d.getResults();
                                called = true;
                            });
        REQUIRE (pumpUntil ([&] { return called; }));
        CHECK (dialog.getKind() == FileDialog::Kind::portal);
        REQUIRE (chosen.size() == 2);
        CHECK (chosen[0] == juce::File ("/tmp/one a.wav"));
        CHECK (dialog.getResult() == juce::File ("/tmp/one a.wav"));
        std::scoped_lock lock (fake.mutex);
        CHECK (fake.method == "OpenFile");
        CHECK (fake.arguments[1].toString() == "Add samples");
        CHECK ((bool)fake.arguments[2]["multiple"]);
        CHECK (findOwnBrowser() == nullptr);
    }

    // Saving: SaveFile.
    {
        fake.setUris ({ "file:///tmp/Song.wav" });
        FileDialog dialog ("Export audio as", juce::File ("/tmp/Song.wav"), "*.wav");
        bool called = false;
        dialog.launchAsync (saveFile, [&] (const FileDialog&) { called = true; });
        REQUIRE (pumpUntil ([&] { return called; }));
        CHECK (dialog.getResult() == juce::File ("/tmp/Song.wav"));
        std::scoped_lock lock (fake.mutex);
        CHECK (fake.method == "SaveFile");
        CHECK (fake.arguments[2]["current_name"].toString() == "Song.wav");
    }

    // Cancelled (response 1): no file, and no other dialog.
    {
        fake.code = 1;
        FileDialog dialog ("Load a loop", {}, "*.wav");
        bool called = false, empty = false;
        dialog.launchAsync (openFiles,
                            [&] (const FileDialog& d)
                            {
                                called = true;
                                empty = d.getResults().isEmpty();
                            });
        REQUIRE (pumpUntil ([&] { return called; }));
        CHECK (empty);
        CHECK (dialog.getKind() == FileDialog::Kind::portal);
        CHECK (findOwnBrowser() == nullptr);
    }

    // Ended another way (response 2: GTK's dialog closed with Escape, or the portal failed): also no file,
    // and no second dialog.
    {
        fake.code = 2;
        FileDialog dialog ("Load a loop", {}, "*.wav");
        bool called = false, empty = false;
        dialog.launchAsync (openFiles,
                            [&] (const FileDialog& d)
                            {
                                called = true;
                                empty = d.getResults().isEmpty();
                            });
        REQUIRE (pumpUntil ([&] { return called; }));
        CHECK (empty);
        CHECK (dialog.getKind() == FileDialog::Kind::portal);
        CHECK (findOwnBrowser() == nullptr);
    }

    // The portal cannot open the dialog (an error for the call): Spinus's own browser instead.
    {
        fake.failCalls = true;
        FileDialog dialog ("Load a loop", {}, "*.wav");
        bool called = false;
        dialog.launchAsync (openFiles, [&] (const FileDialog&) { called = true; });
        REQUIRE (pumpUntil ([] { return findOwnBrowser() != nullptr; }));
        CHECK (dialog.getKind() == FileDialog::Kind::ownBrowser);
        CHECK (dialog.getFallbackReason().contains ("did not open"));
        CHECK (! called);
        findOwnBrowser()->exitModalState (0);
        CHECK (pumpUntil ([&] { return called; }));
        fake.failCalls = false;
    }

    // The window that asked closes while the dialog is open: the dialog is closed, nothing is called.
    {
        fake.respond = false;
        const int before = fake.calls;
        bool called = false;
        auto dialog = std::make_unique<FileDialog> ("Load a loop", juce::File(), "*.wav");
        dialog->launchAsync (openFiles, [&] (const FileDialog&) { called = true; });
        REQUIRE (pumpUntil ([&] { return fake.calls > before; }));
        dialog.reset();
        REQUIRE (pumpUntil ([&] { return fake.getClosed().isNotEmpty(); }));
        std::scoped_lock lock (fake.mutex);
        CHECK (fake.closed == fake.requestHandle);
        CHECK (! called);
    }
    CHECK (pumpUntil ([] { return FileDialog::getPortalRequestsRunning() == 0; }));
}
#endif

TEST_CASE ("Spinus code shows file dialogs through core::FileDialog, not JUCE's FileChooser", "[core][filedialog]")
{
    // JUCE's Linux file chooser runs zenity or kdialog through JUCE's ChildProcess.
    const auto root = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory();
    const juce::String banned = juce::String ("juce::") + "FileChooser";
    juce::StringArray uses;
    for (auto* folder : { "src", "app", "tests" })
        for (const auto& entry : juce::RangedDirectoryIterator (root.getChildFile (folder), true, "*.cpp;*.h"))
        {
            const auto file = entry.getFile();
            if (file.getParentDirectory().getFileName() == "core" && file.getFileNameWithoutExtension() == "FileDialog")
                continue;
            const auto text = file.loadFileAsString();
            for (int i = text.indexOf (banned); i >= 0; i = text.indexOf (i + 1, banned))
                if (! juce::CharacterFunctions::isLetterOrDigit (text[i + banned.length()]))
                    uses.add (file.getRelativePathFrom (root));
        }
    INFO (uses.joinIntoString (", "));
    CHECK (uses.isEmpty());
}
