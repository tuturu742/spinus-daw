#pragma once

#include "DBus.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <vector>

/*  The desktop's own file dialog, through the XDG Desktop Portal (org.freedesktop.portal.FileChooser):
    the dialog Flatpak apps use, shown by xdg-desktop-portal-kde, -gnome or -gtk in a program
    of their own. Used by core::FileDialog on Linux; nothing here touches the message thread.
*/
namespace spinus::core::portal
{

struct Filter
{
    juce::String name;
    juce::StringArray globs;
};

/** What an OpenFile or SaveFile call asks for. */
struct Request
{
    bool save = false;         // SaveFile, else OpenFile
    juce::String parentWindow; // "x11:<window number in hex>", or empty
    juce::String title;
    juce::String acceptLabel; // the button's text; empty: the desktop's own ("Open", "Save")
    bool multiple = false;
    bool directory = false; // choose a folder
    std::vector<Filter> filters;
    juce::String currentName; // SaveFile: the suggested name
    juce::File currentFolder; // where the dialog starts; only sent if it is a folder
    juce::String handleToken; // names the request's object; run() makes one if empty
};

/** True if the portal can show this kind of dialog: anything but one that takes either files or folders
    (the portal's dialog chooses one or the other). `flags` are juce::FileBrowserComponent's. */
bool canShow (int flags);

/** A request from FileChooser-style arguments: title, where to start (a folder, or a file whose
    folder and, when saving, name are suggested), patterns ("*.wav;*.aif") and FileBrowserComponent flags. */
Request makeRequest (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags);

/** "*.wav" -> "*.[wW][aA][vV]": the desktop's dialogs match patterns with case, Spinus's files are
    found in any case. */
juce::String caseInsensitiveGlob (const juce::String& glob);

/** The OpenFile/SaveFile arguments: parent window, title, options (a{sv}). */
std::vector<dbus::Value> callArguments (const Request&);

/** The object path the portal gives the request: /org/freedesktop/portal/desktop/request/<sender>/<token>,
    the sender being the unique name without ':' and with '.' as '_'. */
juce::String requestPath (const juce::String& uniqueName, const juce::String& token);

/** The Response signal of a request. */
struct Response
{
    enum Code
    {
        chosen = 0,
        cancelled = 1,
        ended = 2, // ended another way: closed with Escape or the window's button (GTK), or the portal
                   // failed; anything else is also counted as this
    };
    int code = ended;
    juce::Array<juce::File> files; // from "uris"; only file: URIs
};

/** Reads the Response signal's arguments (u response, a{sv} results). */
Response parseResponse (const juce::Array<juce::var>& signalArguments);

/** A file: URI's file ("file:///home/a%20b.wav" -> /home/a b.wav); an empty File for another URI. */
juce::File fileFromUri (const juce::String& uri);

/** For a path in the document portal's folder (`<documentsRoot>/<id>/...`), the document's id; else empty.
    A sandboxed app without access to a file gets such a path, which names only that file. */
juce::String documentId (const juce::File& file, const juce::File& documentsRoot);

/** The document portal's folder: $XDG_RUNTIME_DIR/doc. */
juce::File documentsRoot();

/** What run() gives. */
struct Outcome
{
    enum Kind
    {
        chosen,
        cancelled,
        unavailable // no portal, or it could not show the dialog: use Spinus's own browser; `reason` says why
    };
    Kind kind = unavailable;
    juce::Array<juce::File> files;
    juce::String reason;
    int version = 0; // the portal's FileChooser version
};

/** Shows the dialog and waits for it, on the calling thread (never the message thread). Returns
    `cancelled` soon after `stop` becomes true, closing the dialog, and for any response but a choice
    (once the dialog was up, Spinus does not show another). A path in the document portal is
    replaced by the file's real path when the app can reach it there (with save: write in its folder). */
Outcome run (const juce::String& busAddress, Request request, const std::atomic<bool>& stop);

} // namespace spinus::core::portal
