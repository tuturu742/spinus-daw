// The X11 declarations JUCE keeps private by default; only this file (and ui/WindowStacking.cpp) needs them.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "PluginDialogs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#if JUCE_LINUX || JUCE_BSD
#include <unistd.h>
#endif

namespace spinus::plugins
{

namespace
{
/** The ScopedWaits in progress: their start times (Time::getMillisecondCounter()). */
struct Waits
{
    std::mutex lock;
    juce::uint64 nextId = 1;
    std::map<juce::uint64, juce::uint32> started;

    static Waits& get()
    {
        static Waits waits;
        return waits;
    }

    /** The oldest one's start, if any. */
    std::optional<juce::uint32> oldest()
    {
        const std::scoped_lock l (lock);
        if (started.empty())
            return std::nullopt;
        // Ids grow with time, so the first is the oldest.
        return started.begin()->second;
    }
};

/** a - b on the wrapping millisecond counter, as a signed age. */
juce::int64 elapsed (juce::uint32 now, juce::uint32 since) { return (juce::int32)(now - since); }
} // namespace

//==============================================================================
PluginDialogs::ScopedWait::ScopedWait()
{
    auto& waits = Waits::get();
    const std::scoped_lock l (waits.lock);
    id = waits.nextId++;
    waits.started[id] = juce::Time::getMillisecondCounter();
}

PluginDialogs::ScopedWait::~ScopedWait()
{
    auto& waits = Waits::get();
    const std::scoped_lock l (waits.lock);
    waits.started.erase (id);
}

bool PluginDialogs::isWindowsPlugin (const juce::String& f)
{
    return f.contains ("/windows/bridged/") || f.containsIgnoreCase ("/yabridge/") || f.containsIgnoreCase ("/.wine");
}

//==============================================================================
/** Spinus's message thread says it is alive four times a second. */
class PluginDialogs::Heartbeat final : private juce::Timer
{
public:
    explicit Heartbeat (PluginDialogs& o) : owner (o) { startTimer (250); }
    ~Heartbeat() override { stopTimer(); }

private:
    void timerCallback() override { owner.beat(); }
    PluginDialogs& owner;
};

/** The background thread: notices a wait, looks for the plugin's windows once a second while it lasts. */
class PluginDialogs::Watcher final : public juce::Thread
{
public:
    explicit Watcher (PluginDialogs& o) : juce::Thread ("Spinus plugin dialogs"), owner (o) {}
    ~Watcher() override { stopThread (4000); }

    void run() override
    {
        const auto& options = owner.options;
        bool waiting = false;
        juce::uint32 lastLook = 0;
        std::unique_ptr<WineWindows> windows;

        const auto finish = [&]
        {
            waiting = false;
            if (options.finished)
                options.finished();
            else if (windows != nullptr)
                windows->finish();
            windows.reset();
        };

        while (! threadShouldExit())
        {
            wait (250);
            const auto now = juce::Time::getMillisecondCounter();

            // Waiting since: the oldest ScopedWait, or the message thread's last heartbeat.
            auto since = Waits::get().oldest();
            if (owner.watchingMessageThread.load())
            {
                const auto beat = owner.lastBeat.load();
                if (! since || elapsed (*since, beat) > 0)
                    since = beat;
            }

            if (since && elapsed (now, *since) >= options.firstAfterMs)
            {
                if (! waiting)
                {
                    waiting = true;
                    ++owner.waitsNoticed;
                    lastLook = now - (juce::uint32)options.everyMs;
                }
                if (elapsed (now, lastLook) >= options.everyMs)
                {
                    lastLook = now;
                    if (options.bringToFront)
                    {
                        options.bringToFront();
                    }
                    else
                    {
                        if (windows == nullptr)
                            windows = std::make_unique<WineWindows>();
                        windows->bringToFront();
                    }
                }
            }
            else if (waiting)
            {
                finish();
            }
        }

        if (waiting)
            finish();
    }

private:
    PluginDialogs& owner;
};

//==============================================================================
PluginDialogs::PluginDialogs() : PluginDialogs (Options {}) {}

PluginDialogs::PluginDialogs (Options o) : options (std::move (o))
{
    lastBeat = juce::Time::getMillisecondCounter();
    watcher = std::make_unique<Watcher> (*this);
    watcher->startThread (juce::Thread::Priority::low);
}

PluginDialogs::~PluginDialogs()
{
    watchingMessageThread = false;
    heartbeat.reset();
    watcher.reset(); // stops the thread, which ends a wait in progress
}

void PluginDialogs::watchMessageThread()
{
    JUCE_ASSERT_MESSAGE_THREAD
#if JUCE_LINUX || JUCE_BSD
    // JUCE's X error handler ignores errors (a window closing while it is looked at), where Xlib's default
    // would end Spinus. It is installed with JUCE's X connection.
    juce::ignoreUnused (juce::XWindowSystem::getInstance());
#endif
    beat();
    heartbeat = std::make_unique<Heartbeat> (*this);
    watchingMessageThread = true;
}

void PluginDialogs::beat() noexcept { lastBeat = juce::Time::getMillisecondCounter(); }

//==============================================================================
juce::String WineWindows::getTitleNote() { return " - waiting for a plugin's own window: answer it to go on"; }

#if JUCE_LINUX || JUCE_BSD

struct WineWindows::Impl
{
    explicit Impl (int own) : ownProcess (own > 0 ? own : (int)getpid()) {}

    ~Impl() { close(); }

    bool open()
    {
        if (display != nullptr)
            return true;
        if (triedToOpen)
            return false;
        triedToOpen = true;

        // Without JUCE's X error handler, Xlib's default would end Spinus when a window closes while it is
        // looked at (BadWindow).
        if (juce::XWindowSystem::getInstanceWithoutCreating() == nullptr)
            return false;

        x11 = juce::X11Symbols::getInstance();
        if (x11 == nullptr)
            return false;
        display = x11->xOpenDisplay (nullptr);
        if (display == nullptr)
            return false;

        root = x11->xDefaultRootWindow (display);
        const auto atom = [this] (const char* name) { return x11->xInternAtom (display, name, False); };
        clientList = atom ("_NET_CLIENT_LIST");
        activeWindow = atom ("_NET_ACTIVE_WINDOW");
        wmPid = atom ("_NET_WM_PID");
        wmName = atom ("_NET_WM_NAME");
        wmState = atom ("WM_STATE");
        return true;
    }

    void close()
    {
        if (display != nullptr)
            x11->xCloseDisplay (display);
        display = nullptr;
    }

    //==============================================================================
    struct Property
    {
        explicit Property (juce::X11Symbols* s) : x11 (s) {}
        ~Property()
        {
            if (data != nullptr)
                x11->xFree (data);
        }
        Property (const Property&) = delete;
        Property& operator= (const Property&) = delete;

        juce::X11Symbols* x11;
        Atom type = None;
        int format = 0;
        unsigned long count = 0;
        unsigned char* data = nullptr;

        const unsigned long* longs() const { return reinterpret_cast<const unsigned long*> (data); }
    };

    bool read (::Window w, Atom name, Property& p, long maxLongs = 4096) const
    {
        unsigned long after = 0;
        return x11->xGetWindowProperty (display, w, name, 0, maxLongs, False, AnyPropertyType, &p.type, &p.format,
                                        &p.count, &after, &p.data) == Success &&
               p.data != nullptr;
    }

    std::optional<unsigned long> readLong (::Window w, Atom name) const
    {
        Property p (x11);
        if (read (w, name, p, 1) && p.format == 32 && p.count >= 1)
            return p.longs()[0];
        return std::nullopt;
    }

    int pidOf (::Window w) const
    {
        const auto pid = readLong (w, wmPid);
        return pid ? (int)*pid : -1;
    }

    /** True if `pid` is a descendant of Spinus's process (yabridge starts each plugin's Wine side from the
        Spinus or scanner process that loads it). The user's own Wine programs are not. */
    bool isOurs (int pid) const
    {
        for (int depth = 0; pid > 1 && depth < 64; ++depth)
        {
            if (pid == ownProcess)
                return true;
            // /proc/<pid>/stat: "pid (name) state ppid ..."; the name may hold spaces and brackets.
            const auto stat = juce::File ("/proc/" + juce::String (pid) + "/stat").loadFileAsString();
            const auto afterName = stat.fromLastOccurrenceOf (")", false, false).trim();
            const auto fields = juce::StringArray::fromTokens (afterName, " ", "");
            if (fields.size() < 2)
                return false;
            pid = fields[1].getIntValue();
        }
        return false;
    }

    /** Wine's window, by its class: "yabridge-host.exe", "Wine". Wine's own explorer.exe (the desktop, the
        system tray when the desktop has none) is left alone. */
    bool isWineWindow (::Window w) const
    {
        Property p (x11);
        if (! read (w, XA_WM_CLASS, p) || p.format != 8)
            return false;
        // "name\0class\0"
        const std::string both (reinterpret_cast<const char*> (p.data), p.count);
        const auto end = std::min (both.find ('\0'), both.size());
        const auto name = juce::String::fromUTF8 (both.c_str(), (int)end);
        const auto className = end < both.size() ? juce::String::fromUTF8 (both.c_str() + end + 1) : juce::String();
        if (name.equalsIgnoreCase ("explorer.exe"))
            return false;
        return name.endsWithIgnoreCase (".exe") || className.equalsIgnoreCase ("Wine");
    }

    /** The desktop's top-level windows: those the window manager manages (not override-redirect menus, not
        windows embedded in others, such as plugin windows inside Spinus's), or without a window manager the
        root window's visible children. */
    std::vector<::Window> topLevelWindows() const
    {
        std::vector<::Window> result;
        if (Property p (x11); read (root, clientList, p) && p.format == 32)
        {
            for (unsigned long i = 0; i < p.count; ++i)
            {
                const auto w = (::Window)p.longs()[i];
                // Normal (1) or minimised (3); a hidden window is withdrawn (0) or has no state.
                if (const auto state = readLong (w, wmState); state && (*state == 1 || *state == 3))
                    result.push_back (w);
            }
            return result;
        }

        ::Window rootReturn = 0, parentReturn = 0;
        ::Window* children = nullptr;
        unsigned int count = 0;
        if (x11->xQueryTree (display, root, &rootReturn, &parentReturn, &children, &count) == 0)
            return result;
        for (unsigned int i = 0; i < count; ++i)
        {
            XWindowAttributes attributes {};
            if (x11->xGetWindowAttributes (display, children[i], &attributes) != 0 &&
                attributes.map_state == IsViewable && attributes.override_redirect == False)
                result.push_back (children[i]);
        }
        if (children != nullptr)
            x11->xFree (children);
        return result;
    }

    /** As a pager or taskbar would: KDE Plasma's focus-stealing prevention lets source 2 through. */
    void activate (::Window w) const
    {
        XEvent event {};
        event.xclient.type = ClientMessage;
        event.xclient.send_event = True;
        event.xclient.display = display;
        event.xclient.window = w;
        event.xclient.message_type = activeWindow;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 2;           // source: a pager
        event.xclient.data.l[1] = CurrentTime; // when
        event.xclient.data.l[2] = 0;           // the requester's active window: none
        x11->xSendEvent (display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
        // Window managers without _NET_ACTIVE_WINDOW, or none: raise it (and show it if minimised).
        x11->xMapRaised (display, w);
    }

    /** A window's title: _NET_WM_NAME, or WM_NAME (where JUCE puts it, as UTF-8). */
    struct Title
    {
        Atom property = None, type = None;
        juce::String text;
    };

    Title readTitle (::Window w) const
    {
        for (auto property : { wmName, (Atom)XA_WM_NAME })
        {
            Property p (x11);
            if (read (w, property, p) && p.format == 8 && p.count > 0)
                return { property, p.type,
                         juce::String::fromUTF8 (reinterpret_cast<const char*> (p.data), (int)p.count) };
        }
        return {};
    }

    /** Spinus's titles are UTF-8; the note is plain ASCII, so a Latin-1 (STRING) title stays valid too. */
    void setTitle (::Window w, const Title& title) const
    {
        const auto* bytes = title.text.toRawUTF8();
        x11->xChangeProperty (display, w, title.property, title.type, 8, PropModeReplace,
                              reinterpret_cast<const unsigned char*> (bytes), (int)strlen (bytes));
    }

    //==============================================================================
    int bringToFront()
    {
        if (! open())
            return 0;

        std::vector<::Window> wine, own;
        for (auto w : topLevelWindows())
        {
            const auto pid = pidOf (w);
            if (pid == ownProcess)
                own.push_back (w);
            else if (pid > 0 && isWineWindow (w) && isOurs (pid))
                wine.push_back (w);
        }

        // Again only while Spinus (frozen) has the front, so what the plugin opened (e.g. a browser for its
        // registration page) or another program the user went to keeps it.
        const auto active = readLong (root, activeWindow);
        const bool spinusInFront =
            ! active || *active == 0 || std::find (own.begin(), own.end(), (::Window)*active) != own.end();

        int activated = 0;
        for (auto w : wine)
        {
            const bool firstTime = seen.insert (w).second;
            if (firstTime || (spinusInFront && (! active || (::Window)*active != w)))
            {
                activate (w);
                ++activated;
            }
        }

        if (! wine.empty())
            for (auto w : own)
            {
                if (retitled.count (w) != 0)
                    continue;
                const auto title = readTitle (w);
                if (title.text.isEmpty() || title.text.endsWith (getTitleNote()))
                    continue;
                auto noted = title;
                noted.text += getTitleNote();
                setTitle (w, noted);
                retitled[w] = { title, noted.text };
            }

        x11->xSync (display, False); // applied before this look returns
        return activated;
    }

    void finish()
    {
        if (display != nullptr)
        {
            for (auto& [w, titles] : retitled)
                if (readTitle (w).text == titles.second) // unless Spinus changed it since
                    setTitle (w, titles.first);
            x11->xSync (display, False);
        }
        retitled.clear();
        seen.clear();
        close();
        triedToOpen = false;
    }

    const int ownProcess;
    juce::X11Symbols* x11 = nullptr;
    ::Display* display = nullptr;
    bool triedToOpen = false;
    ::Window root = 0;
    Atom clientList = None, activeWindow = None, wmPid = None, wmName = None, wmState = None;
    std::set<::Window> seen;
    std::map<::Window, std::pair<Title, juce::String>> retitled; // as it was, as noted
};

#else

struct WineWindows::Impl
{
    explicit Impl (int) {}
    int bringToFront() { return 0; }
    void finish() {}
};

#endif

WineWindows::WineWindows (int ownProcess) : impl (std::make_unique<Impl> (ownProcess)) {}
WineWindows::~WineWindows() { impl->finish(); }
int WineWindows::bringToFront() { return impl->bringToFront(); }
void WineWindows::finish() { impl->finish(); }

} // namespace spinus::plugins
