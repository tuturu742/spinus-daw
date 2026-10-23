#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace spinus::core
{

/** Every file dialog of Spinus goes through this (a test fails if juce::FileChooser is named
    elsewhere). The interface of juce::FileChooser's launchAsync.

    - Linux: the desktop's own dialog through the XDG Desktop Portal (KDE's, GNOME's...; in the Flatpak
      the one Flatpak apps use), modal to Spinus's window. Waiting for it never blocks the message thread.
      Without a portal (a bare window manager, an old system, a test display), or for a dialog that takes
      either files or folders, Spinus's own file browser (JUCE's, not the zenity/kdialog dialog JUCE would
      start, whose output JUCE reads with its ChildProcess).
    - Windows: the system's dialog, through JUCE.

    The callback runs on the message thread once the dialog is done; with no file when it was cancelled.
    Deleting the FileDialog closes a dialog still open, and the callback is not called. */
class FileDialog
{
public:
    /** As juce::FileChooser: patterns like "*.wav;*.aif"; start is a folder, or a file whose folder (and,
        when saving, name) is suggested. The dialog is modal to parent's window, else Spinus's active one. */
    explicit FileDialog (const juce::String& title, const juce::File& start = {}, const juce::String& patterns = {},
                         juce::Component* parent = nullptr);
    ~FileDialog();

    /** flags: juce::FileBrowserComponent's (openMode/saveMode, canSelectFiles/Directories/MultipleItems,
        warnAboutOverwriting). */
    void launchAsync (int flags, std::function<void (const FileDialog&)> callback);

    juce::File getResult() const { return results.isEmpty() ? juce::File() : results.getFirst(); }
    const juce::Array<juce::File>& getResults() const { return results; }

    enum class Kind
    {
        none,       // not launched yet
        portal,     // the desktop's dialog through the portal (Linux)
        ownBrowser, // Spinus's own file browser (Linux)
        system      // the system's dialog through JUCE (Windows)
    };
    /** Which dialog is (or was) shown. Asking the portal counts as `portal` until it says it cannot. */
    Kind getKind() const { return kind; }
    /** Why Spinus's own browser is shown instead of the desktop's dialog. */
    const juce::String& getFallbackReason() const { return fallbackReason; }

    /** The bus where the portal is asked for. Tests set a private bus, or "" for none (Spinus's own
        browser): never the user's session bus. std::nullopt: the session bus (the default). */
    static void setBusAddress (std::optional<juce::String> address);
    /** How many portal dialogs are still being waited for on their threads (tests). */
    static int getPortalRequestsRunning();

private:
    struct Pending;
    void launchOwnBrowser (const juce::String& reason);
    void finish (juce::Array<juce::File> files);

    juce::String title;
    juce::File start;
    juce::String patterns;
    juce::Component::SafePointer<juce::Component> parent;
    int flags = 0;
    std::function<void (const FileDialog&)> callback;
    juce::Array<juce::File> results;
    Kind kind = Kind::none;
    juce::String fallbackReason;
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<Pending> pending;

    JUCE_DECLARE_NON_COPYABLE (FileDialog)
};

} // namespace spinus::core
