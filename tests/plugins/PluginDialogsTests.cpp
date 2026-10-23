// The X11 declarations JUCE keeps private by default: the test makes windows of its own.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include <catch2/catch_test_macros.hpp>

#include "plugins/windows/PluginDialogs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <thread>

#if JUCE_LINUX || JUCE_BSD
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace spinus::plugins;

namespace
{
/** Waits up to `ms` for `condition`, letting the message thread run. */
bool waitFor (const std::function<bool()>& condition, int ms = 3000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
    while (juce::Time::getMillisecondCounter() < end)
    {
        if (condition())
            return true;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }
    return condition();
}
} // namespace

TEST_CASE ("A long wait for a Windows plugin is noticed, a short one is not", "[plugins][windows][plugin-dialogs]")
{
    std::atomic<int> looks { 0 }, ends { 0 };
    PluginDialogs::Options options;
    options.firstAfterMs = 300;
    options.everyMs = 200;
    options.bringToFront = [&] { ++looks; };
    options.finished = [&] { ++ends; };
    PluginDialogs dialogs (options);

    {
        const PluginDialogs::ScopedWait wait;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    CHECK (looks == 0);
    CHECK (ends == 0);

    // On another thread, as the plugin scanner waits.
    std::thread (
        []
        {
            const PluginDialogs::ScopedWait wait;
            std::this_thread::sleep_for (std::chrono::milliseconds (1100));
        })
        .join();
    CHECK (looks >= 2); // the first after 300 ms, then every 200 ms
    CHECK (looks <= 5);
    CHECK (waitFor ([&] { return ends == 1; }, 1000));
    CHECK (dialogs.getNumberOfWaitsNoticed() == 1);
}

TEST_CASE ("A message thread that stops responding counts as waiting", "[plugins][windows][plugin-dialogs]")
{
    std::atomic<int> looks { 0 }, ends { 0 };
    PluginDialogs::Options options;
    options.firstAfterMs = 600;
    options.everyMs = 200;
    options.bringToFront = [&] { ++looks; };
    options.finished = [&] { ++ends; };
    PluginDialogs dialogs (options);
    dialogs.watchMessageThread();

    // Responding: no wait.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (1200);
    CHECK (looks == 0);

    // Stuck (as inside a plugin's call that shows a window and waits for an answer).
    std::this_thread::sleep_for (std::chrono::milliseconds (1300));
    CHECK (looks >= 1);
    CHECK (ends == 0);

    // Responding again: the wait ends.
    CHECK (waitFor ([&] { return ends == 1; }, 2000));
}

#if JUCE_LINUX || JUCE_BSD
namespace
{
/** Windows of the test's own on the X display, as Spinus's, a plugin's and other programs' would be. */
struct TestDisplay
{
    TestDisplay()
    {
        x11 = juce::X11Symbols::getInstance();
        display = x11->xOpenDisplay (nullptr);
        if (display == nullptr)
            return;
        root = x11->xDefaultRootWindow (display);
        active = atom ("_NET_ACTIVE_WINDOW");
        clientList = atom ("_NET_CLIENT_LIST");
        stacking = atom ("_NET_CLIENT_LIST_STACKING");
    }

    ~TestDisplay()
    {
        if (display == nullptr)
            return;
        for (auto w : made)
            x11->xDestroyWindow (display, w);
        x11->xCloseDisplay (display);
    }

    Atom atom (const char* name) const { return x11->xInternAtom (display, name, False); }

    /** True if a window manager that knows _NET_ACTIVE_WINDOW runs (openbox on the test display :5). */
    bool hasWindowManager() const { return ! readList (root, atom ("_NET_SUPPORTING_WM_CHECK")).empty(); }

    ::Window make (const juce::String& title, const char* className, size_t classLength, long pid)
    {
        XSetWindowAttributes attributes {};
        const auto w = x11->xCreateWindow (display, root, 100, 100, 300, 200, 0, CopyFromParent, InputOutput, nullptr,
                                           0, &attributes);
        made.push_back (w);
        x11->xChangeProperty (display, w, XA_WM_CLASS, XA_STRING, 8, PropModeReplace,
                              reinterpret_cast<const unsigned char*> (className), (int)classLength);
        x11->xChangeProperty (display, w, atom ("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace,
                              reinterpret_cast<const unsigned char*> (&pid), 1);
        setTitle (w, title);
        x11->xMapWindow (display, w);
        x11->xSync (display, False); // done before another connection looks
        return w;
    }

    void setTitle (::Window w, const juce::String& title) const
    {
        // As JUCE sets it: WM_NAME in UTF-8, no _NET_WM_NAME.
        x11->xChangeProperty (display, w, XA_WM_NAME, atom ("UTF8_STRING"), 8, PropModeReplace,
                              reinterpret_cast<const unsigned char*> (title.toRawUTF8()),
                              (int)title.getNumBytesAsUTF8());
        x11->xSync (display, False); // done before another connection looks
    }

    juce::String getTitle (::Window w) const
    {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        juce::String title;
        if (x11->xGetWindowProperty (display, w, XA_WM_NAME, 0, 1024, False, AnyPropertyType, &type, &format, &count,
                                     &after, &data) == Success &&
            data != nullptr)
        {
            title = juce::String::fromUTF8 (reinterpret_cast<const char*> (data), (int)count);
            x11->xFree (data);
        }
        return title;
    }

    std::vector<::Window> readList (::Window w, Atom name) const
    {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        std::vector<::Window> list;
        if (x11->xGetWindowProperty (display, w, name, 0, 1024, False, AnyPropertyType, &type, &format, &count, &after,
                                     &data) == Success &&
            data != nullptr)
        {
            for (unsigned long i = 0; i < count; ++i)
                list.push_back ((::Window) reinterpret_cast<const unsigned long*> (data)[i]);
            x11->xFree (data);
        }
        return list;
    }

    ::Window getActive() const
    {
        auto list = readList (root, active);
        return list.empty() ? 0 : list.front();
    }

    bool isManaged (::Window w) const
    {
        auto list = readList (root, clientList);
        return std::find (list.begin(), list.end(), w) != list.end();
    }

    /** True if `upper` is stacked above `lower`. */
    bool isAbove (::Window upper, ::Window lower) const
    {
        auto list = readList (root, stacking); // bottom to top
        const auto u = std::find (list.begin(), list.end(), upper), l = std::find (list.begin(), list.end(), lower);
        return u != list.end() && l != list.end() && u > l;
    }

    /** As a user clicking a window would (a pager's request, which the window manager follows). */
    void activate (::Window w) const
    {
        XEvent event {};
        event.xclient.type = ClientMessage;
        event.xclient.send_event = True;
        event.xclient.display = display;
        event.xclient.window = w;
        event.xclient.message_type = active;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 2;
        x11->xSendEvent (display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
        x11->xSync (display, False); // done before another connection looks
    }

    juce::X11Symbols* x11 = nullptr;
    ::Display* display = nullptr;
    ::Window root = 0;
    Atom active = None, clientList = None, stacking = None;
    std::vector<::Window> made;
};

/** A process started by this one, standing in for Wine running a plugin's Windows side. */
struct ChildProcessId
{
    ChildProcessId()
    {
        const char* args[] = { "sleep", "60", nullptr };
        if (posix_spawnp (&pid, "sleep", nullptr, nullptr, const_cast<char* const*> (args), environ) != 0)
            pid = -1;
    }
    ~ChildProcessId()
    {
        if (pid > 0)
        {
            kill (pid, SIGTERM);
            waitpid (pid, nullptr, 0);
        }
    }
    pid_t pid = -1;
};
} // namespace

TEST_CASE ("A Windows plugin's own window is brought to the front", "[plugins][windows][plugin-dialogs]")
{
    // JUCE's X connection and error handler, as in Spinus.
    if (juce::XWindowSystem::getInstance()->getDisplay() == nullptr)
        SKIP ("no X display");
    TestDisplay x;
    if (x.display == nullptr)
        SKIP ("no X display");
    if (! x.hasWindowManager())
        SKIP ("no window manager on this display (the test display :5 runs openbox)");

    const ChildProcessId wine;
    REQUIRE (wine.pid > 0);

    static const char pluginClass[] = "yabridge-host.exe\0yabridge-host.exe";
    static const char strangerClass[] = "game.exe\0Wine";
    static const char otherClass[] = "other\0Other";

    // The plugin's question appears first, then Spinus's window covers it (as on a desktop that keeps a
    // window opened by a background program behind the one in use).
    const auto dialog = x.make ("Needs registration", pluginClass, sizeof (pluginClass), wine.pid);
    // A Windows program of the user's own, not started by Spinus: left alone.
    const auto stranger = x.make ("A game", strangerClass, sizeof (strangerClass), 1);
    REQUIRE (waitFor ([&] { return x.isManaged (dialog) && x.isManaged (stranger); }));
    const auto spinus = x.make ("Spinus DAW", otherClass, sizeof (otherClass), (long)getpid());
    const auto other = x.make ("Another program", otherClass, sizeof (otherClass), 1);
    REQUIRE (waitFor ([&] { return x.isManaged (spinus) && x.isManaged (other); }));
    x.activate (spinus);
    REQUIRE (waitFor ([&] { return x.getActive() == spinus && x.isAbove (spinus, dialog); }));

    WineWindows windows ((int)getpid());

    // Seen for the first time: activated and raised above Spinus's window.
    CHECK (windows.bringToFront() == 1);
    CHECK (waitFor ([&] { return x.getActive() == dialog; }));
    CHECK (x.isAbove (dialog, spinus));
    CHECK (x.getTitle (spinus) == "Spinus DAW" + WineWindows::getTitleNote());
    CHECK (x.getTitle (other) == "Another program");

    // In front already: nothing to do.
    CHECK (windows.bringToFront() == 0);

    // The user went to another program (e.g. a browser the plugin opened): it keeps the front.
    x.activate (other);
    REQUIRE (waitFor ([&] { return x.getActive() == other; }));
    CHECK (windows.bringToFront() == 0);
    CHECK (x.getActive() == other);

    // The user clicked Spinus's (frozen) window: the question comes back to the front.
    x.activate (spinus);
    REQUIRE (waitFor ([&] { return x.getActive() == spinus; }));
    CHECK (windows.bringToFront() == 1);
    CHECK (waitFor ([&] { return x.getActive() == dialog; }));
    CHECK (x.getActive() != stranger);

    // The wait is over: Spinus's title as it was.
    windows.finish();
    CHECK (x.getTitle (spinus) == "Spinus DAW");

    // A title Spinus changed meanwhile stays.
    x.activate (spinus);
    REQUIRE (waitFor ([&] { return x.getActive() == spinus; }));
    CHECK (windows.bringToFront() == 1);
    x.setTitle (spinus, "Spinus DAW - Song");
    windows.finish();
    CHECK (x.getTitle (spinus) == "Spinus DAW - Song");
}
#endif

TEST_CASE ("Windows plugins are told by their files", "[plugins][windows][plugin-dialogs]")
{
    CHECK (PluginDialogs::isWindowsPlugin ("/home/u/.local/share/Spinus DAW/windows/bridged/vst3/Tube.vst3"));
    CHECK (PluginDialogs::isWindowsPlugin ("/home/u/.vst3/yabridge/Plugin.vst3"));
    CHECK (PluginDialogs::isWindowsPlugin ("/home/u/.wine/drive_c/Plugin.so"));
    CHECK_FALSE (PluginDialogs::isWindowsPlugin ("/usr/lib/vst3/Surge XT.vst3"));
}
