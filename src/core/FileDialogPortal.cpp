#include "FileDialogPortal.h"

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_LINUX || JUCE_BSD
#include <unistd.h>
#endif

namespace spinus::core::portal
{

namespace
{
using Browser = juce::FileBrowserComponent;

bool has (int flags, int flag) { return ((unsigned)flags & (unsigned)flag) != 0; }

constexpr const char* desktopName = "org.freedesktop.portal.Desktop";
constexpr const char* desktopPath = "/org/freedesktop/portal/desktop";
constexpr const char* chooserInterface = "org.freedesktop.portal.FileChooser";
constexpr const char* requestInterface = "org.freedesktop.portal.Request";

juce::String filterName (const juce::StringArray& globs)
{
    const auto joined = globs.joinIntoString (", ");
    return joined.length() <= 60 ? joined : juce::String ("Supported files");
}

dbus::Value filterValue (const Filter& f)
{
    std::vector<dbus::Value> globs;
    globs.reserve ((size_t)f.globs.size());
    for (auto& g : f.globs)
        globs.push_back (dbus::Value::structure ({ dbus::Value::uint32 (0), dbus::Value::string (g) })); // 0: glob
    return dbus::Value::structure ({ dbus::Value::string (f.name), dbus::Value::array ("(us)", std::move (globs)) });
}

int hexDigit (juce::juce_wchar c)
{
    if (c >= '0' && c <= '9')
        return (int)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F')
        return (int)(c - 'A') + 10;
    return -1;
}

/** A path from the portal, or its real path when it is in the document portal and the app reaches that. */
juce::File realPath (dbus::Connection& bus, const juce::File& file, bool save)
{
    const auto root = documentsRoot();
    const auto id = documentId (file, root);
    if (id.isEmpty())
        return file;
    const auto reply = bus.call ("org.freedesktop.portal.Documents", "/org/freedesktop/portal/documents",
                                 "org.freedesktop.portal.Documents", "GetHostPaths",
                                 { dbus::Value::array ("s", { dbus::Value::string (id) }) }, 5000);
    if (reply.type != dbus::Message::methodReturn || reply.arguments.isEmpty())
        return file;
    const auto host = reply.arguments[0].getProperty (juce::Identifier (id), {}).toString();
    if (! juce::File::isAbsolutePath (host))
        return file;
    // <root>/<id>/<name>[/more] is <host>[/more].
    const auto inDocument = file.getRelativePathFrom (root.getChildFile (id));
    const auto rest = inDocument.fromFirstOccurrenceOf ("/", false, false);
    const auto real = rest.isEmpty() ? juce::File (host) : juce::File (host).getChildFile (rest);
    const bool reachable =
        save ? real.getParentDirectory().isDirectory() && real.getParentDirectory().hasWriteAccess() : real.exists();
    return reachable ? real : file;
}
} // namespace

bool canShow (int flags)
{
    return has (flags, Browser::saveMode) ||
           ! (has (flags, Browser::canSelectFiles) && has (flags, Browser::canSelectDirectories));
}

juce::String caseInsensitiveGlob (const juce::String& glob)
{
    juce::String result;
    bool inBrackets = false;
    for (auto c : glob)
    {
        if (c == '[')
            inBrackets = true;
        else if (c == ']')
            inBrackets = false;
        const auto lower = juce::CharacterFunctions::toLowerCase (c);
        const auto upper = juce::CharacterFunctions::toUpperCase (c);
        if (! inBrackets && lower != upper)
            result << '[' << juce::String::charToString (lower) << juce::String::charToString (upper) << ']';
        else
            result << juce::String::charToString (c);
    }
    return result;
}

Request makeRequest (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags)
{
    Request r;
    r.title = title;
    r.save = has (flags, Browser::saveMode);
    r.directory = ! r.save && has (flags, Browser::canSelectDirectories) && ! has (flags, Browser::canSelectFiles);
    r.multiple = ! r.save && has (flags, Browser::canSelectMultipleItems);

    if (! r.directory)
    {
        juce::StringArray globs;
        globs.addTokens (patterns, ";,", "\"'");
        globs.trim();
        globs.removeEmptyStrings();
        globs.removeDuplicates (true);
        if (! globs.isEmpty() && ! globs.contains ("*") && ! globs.contains ("*.*"))
        {
            Filter f { filterName (globs), {} };
            for (auto& g : globs)
                f.globs.add (caseInsensitiveGlob (g));
            r.filters.push_back (f);
        }
    }

    if (start != juce::File())
    {
        if (start.isDirectory())
            r.currentFolder = start;
        else
        {
            if (r.save)
                r.currentName = start.getFileName();
            if (start.getParentDirectory().isDirectory())
                r.currentFolder = start.getParentDirectory();
        }
    }
    return r;
}

std::vector<dbus::Value> callArguments (const Request& r)
{
    std::vector<std::pair<juce::String, dbus::Value>> options;
    options.emplace_back ("handle_token", dbus::Value::string (r.handleToken));
    options.emplace_back ("modal", dbus::Value::boolean (true));
    if (r.acceptLabel.isNotEmpty())
        options.emplace_back ("accept_label", dbus::Value::string (r.acceptLabel));
    if (! r.save)
    {
        options.emplace_back ("multiple", dbus::Value::boolean (r.multiple));
        if (r.directory)
            options.emplace_back ("directory", dbus::Value::boolean (true));
    }
    if (! r.filters.empty())
    {
        std::vector<dbus::Value> filters;
        filters.reserve (r.filters.size());
        for (auto& f : r.filters)
            filters.push_back (filterValue (f));
        options.emplace_back ("filters", dbus::Value::array ("(sa(us))", std::move (filters)));
        options.emplace_back ("current_filter", filterValue (r.filters.front()));
    }
    if (r.save && r.currentName.isNotEmpty())
        options.emplace_back ("current_name", dbus::Value::string (r.currentName));
    if (r.currentFolder != juce::File())
        options.emplace_back ("current_folder", dbus::Value::bytes (r.currentFolder.getFullPathName()));

    return { dbus::Value::string (r.parentWindow), dbus::Value::string (r.title),
             dbus::Value::dictionary (std::move (options)) };
}

juce::String requestPath (const juce::String& uniqueName, const juce::String& token)
{
    return juce::String (desktopPath) + "/request/" +
           uniqueName.trimCharactersAtStart (":").replaceCharacter ('.', '_') + "/" + token;
}

juce::File fileFromUri (const juce::String& uri)
{
    if (! uri.startsWithIgnoreCase ("file://"))
        return {};
    auto rest = uri.substring (7);
    if (! rest.startsWith ("/")) // file://host/path: only this computer
    {
        if (! rest.startsWithIgnoreCase ("localhost/"))
            return {};
        rest = rest.substring (9);
    }
    // Percent-decoding of the UTF-8 bytes ('+' is a plus in a file: URI).
    juce::MemoryOutputStream bytes;
    const auto utf8 = rest.toStdString();
    for (size_t i = 0; i < utf8.size(); ++i)
    {
        const auto hi = i + 2 < utf8.size() ? hexDigit ((juce::juce_wchar)(unsigned char)utf8[i + 1]) : -1;
        const auto lo = i + 2 < utf8.size() ? hexDigit ((juce::juce_wchar)(unsigned char)utf8[i + 2]) : -1;
        if (utf8[i] == '%' && hi >= 0 && lo >= 0)
        {
            bytes.writeByte ((char)(hi * 16 + lo));
            i += 2;
        }
        else
            bytes.writeByte (utf8[i]);
    }
    const auto path = bytes.toUTF8();
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

Response parseResponse (const juce::Array<juce::var>& args)
{
    Response r;
    if (args.isEmpty())
        return r;
    const auto code = (juce::int64)args[0];
    r.code = code == 0 ? Response::chosen : code == 1 ? Response::cancelled : Response::ended;
    if (r.code != Response::chosen)
        return r;
    if (auto* uris = args[1].getProperty ("uris", {}).getArray())
        for (auto& u : *uris)
            if (const auto f = fileFromUri (u.toString()); f != juce::File())
                r.files.add (f);
    return r;
}

juce::File documentsRoot()
{
    auto runtime = juce::SystemStats::getEnvironmentVariable ("XDG_RUNTIME_DIR", {});
#if JUCE_LINUX || JUCE_BSD
    if (runtime.isEmpty())
        runtime = "/run/user/" + juce::String ((int)getuid());
#endif
    return runtime.isEmpty() ? juce::File() : juce::File (runtime).getChildFile ("doc");
}

juce::String documentId (const juce::File& file, const juce::File& root)
{
    if (root == juce::File() || ! file.isAChildOf (root))
        return {};
    return file.getRelativePathFrom (root).upToFirstOccurrenceOf ("/", false, false);
}

Outcome run (const juce::String& busAddress, Request request, const std::atomic<bool>& stop)
{
    Outcome outcome;
    juce::String error;
    auto bus = dbus::Connection::open (busAddress, error);
    if (bus == nullptr)
    {
        outcome.reason = std::move (error);
        return outcome;
    }

    // Starts the portal if it is not running; fails at once if there is none, or no file dialog in it.
    const auto version = bus->call (desktopName, desktopPath, "org.freedesktop.DBus.Properties", "Get",
                                    { dbus::Value::string (chooserInterface), dbus::Value::string ("version") }, 25000);
    if (version.type != dbus::Message::methodReturn || version.arguments.isEmpty())
    {
        outcome.reason = "no desktop file dialog (" + version.errorText() + ")";
        return outcome;
    }
    outcome.version = (int)(juce::int64)version.arguments[0];
    if (request.directory && outcome.version < 3)
    {
        outcome.reason = "the desktop's file dialog cannot choose folders (FileChooser version " +
                         juce::String (outcome.version) + ")";
        return outcome;
    }

    // Listens before calling: the answer can come before the call returns.
    if (request.handleToken.isEmpty())
        request.handleToken = "spinus" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());
    auto handle = requestPath (bus->getUniqueName(), request.handleToken);
    auto match = [&] (const juce::String& path)
    {
        return bus->addMatch ("type='signal',interface='" + juce::String (requestInterface) +
                              "',member='Response',path='" + path + "'");
    };
    match (handle);
    bus->addMatch ("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='"
                   "NameOwnerChanged',arg0='" +
                   juce::String (desktopName) + "'");

    const auto reply = bus->call (desktopName, desktopPath, chooserInterface, request.save ? "SaveFile" : "OpenFile",
                                  callArguments (request), 25000);
    if (reply.type != dbus::Message::methodReturn || reply.arguments.isEmpty())
    {
        outcome.reason = "the desktop's file dialog did not open (" + reply.errorText() + ")";
        return outcome;
    }
    if (const auto given = reply.arguments[0].toString(); given != handle) // portals before version 0.9
    {
        handle = given;
        match (handle);
    }

    while (! stop.load())
    {
        const auto message = bus->next (100);
        if (! message || message->type != dbus::Message::signal)
            continue;
        if (message->member == "NameOwnerChanged" && message->arguments.size() >= 3 &&
            message->arguments[2].toString().isEmpty())
        {
            outcome.reason = "the desktop's file dialog stopped";
            return outcome;
        }
        if (message->member != "Response" || message->path != handle)
            continue;

        // Response 2 ("ended another way") is also what xdg-desktop-portal-gtk sends when the dialog is
        // closed with Escape or its window's close button: a cancel, not a reason for another dialog.
        const auto response = parseResponse (message->arguments);
        if (response.code != Response::chosen || response.files.isEmpty())
        {
            outcome.kind = Outcome::cancelled;
            return outcome;
        }
        outcome.kind = Outcome::chosen;
        for (auto& f : response.files)
            outcome.files.add (realPath (*bus, f, request.save));
        return outcome;
    }

    bus->callNoReply (desktopName, handle, requestInterface, "Close", {});
    outcome.kind = Outcome::cancelled;
    return outcome;
}

} // namespace spinus::core::portal
