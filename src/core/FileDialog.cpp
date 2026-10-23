#include "FileDialog.h"

#if (JUCE_LINUX || JUCE_BSD) && ! defined(SPINUS_FILE_DIALOG_OWN_BROWSER_ONLY)
#define SPINUS_FILE_DIALOG_PORTAL 1
#include "DBus.h"
#include "FileDialogPortal.h"

#include <thread>
#else
#define SPINUS_FILE_DIALOG_PORTAL 0
#endif

namespace spinus::core
{

namespace
{
std::optional<juce::String> busOverride; // message thread
std::atomic<int> portalRequestsRunning { 0 };

#if SPINUS_FILE_DIALOG_PORTAL
/** The portal's name for the window the dialog belongs to: "x11:<number in hex>" (JUCE's windows are X11
    windows, also under Wayland through XWayland). */
juce::String parentWindowHandle (juce::Component* parent)
{
    juce::Component* window = parent != nullptr ? parent->getTopLevelComponent() : nullptr;
    if (window == nullptr || window->getPeer() == nullptr)
        window = juce::TopLevelWindow::getActiveTopLevelWindow();
    if (window == nullptr || window->getPeer() == nullptr)
        for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
            if (auto* c = juce::Desktop::getInstance().getComponent (i); c->isShowing() && c->getPeer() != nullptr)
            {
                window = c;
                break;
            }
    if (window == nullptr || window->getPeer() == nullptr)
        return {};
    return "x11:" + juce::String::toHexString ((juce::pointer_sized_int)window->getPeer()->getNativeHandle());
}
#endif
} // namespace

/** A portal dialog being waited for on its own thread; the thread keeps it alive after the FileDialog
    is gone. `owner` is only used on the message thread. */
struct FileDialog::Pending
{
    std::atomic<bool> stop { false };
    FileDialog* owner = nullptr;
};

FileDialog::FileDialog (const juce::String& t, const juce::File& s, const juce::String& p, juce::Component* c)
    : title (t), start (s), patterns (p), parent (c)
{
}

FileDialog::~FileDialog()
{
    if (pending != nullptr)
    {
        pending->owner = nullptr;
        pending->stop = true; // the thread closes the dialog and ends; it is not waited for here
    }
}

void FileDialog::setBusAddress (std::optional<juce::String> address) { busOverride = std::move (address); }

int FileDialog::getPortalRequestsRunning() { return portalRequestsRunning.load(); }

void FileDialog::launchAsync (int f, std::function<void (const FileDialog&)> done)
{
    flags = f;
    callback = std::move (done);
    results.clear();

#if SPINUS_FILE_DIALOG_PORTAL
    if (! portal::canShow (flags))
    {
        launchOwnBrowser ("the desktop's file dialog takes either files or folders");
        return;
    }
    const auto address = busOverride.value_or (dbus::sessionBusAddress());
    if (address.isEmpty())
    {
        launchOwnBrowser ("no session bus");
        return;
    }
    if (! dbus::isAvailable())
    {
        launchOwnBrowser ("libdbus-1.so.3 is not installed");
        return;
    }

    auto request = portal::makeRequest (title, start, patterns, flags);
    request.parentWindow = parentWindowHandle (parent.getComponent());

    if (pending != nullptr)
    {
        pending->owner = nullptr;
        pending->stop = true;
    }
    pending = std::make_shared<Pending>();
    pending->owner = this;
    kind = Kind::portal;
    ++portalRequestsRunning;
    std::thread (
        [p = pending, address, request]
        {
            auto outcome = portal::run (address, request, p->stop);
            juce::MessageManager::callAsync (
                [p, outcome]
                {
                    auto* owner = p->owner;
                    if (owner == nullptr || owner->pending != p)
                        return;
                    owner->pending.reset();
                    if (outcome.kind == portal::Outcome::unavailable)
                        owner->launchOwnBrowser (outcome.reason);
                    else
                        owner->finish (outcome.files); // may delete the FileDialog
                });
            --portalRequestsRunning;
        })
        .detach();
#elif JUCE_LINUX || JUCE_BSD
    launchOwnBrowser ("a plugin build");
#else
    kind = Kind::system;
    chooser = std::make_unique<juce::FileChooser> (title, start, patterns, true);
    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc) { finish (fc.getResults()); });
#endif
}

void FileDialog::launchOwnBrowser (const juce::String& reason)
{
    kind = Kind::ownBrowser;
    fallbackReason = reason;
    // Not JUCE's "native" Linux dialog: that runs zenity or kdialog through JUCE's ChildProcess.
    chooser = std::make_unique<juce::FileChooser> (title, start, patterns, false);
    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc) { finish (fc.getResults()); });
}

void FileDialog::finish (juce::Array<juce::File> files)
{
    results = std::move (files);
    if (auto done = callback) // a copy: the callback may delete this FileDialog
        done (*this);
}

} // namespace spinus::core
