#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>

namespace spinus::plugins
{

/** Windows plugins' own windows (a "needs registration" box, a licence check, an activation prompt) brought
    to the front while Spinus waits for the plugin.

    Such a plugin shows its window from inside a call Spinus makes into it (creating it, opening its window,
    loading its state) and does not return until the user answers. Spinus's message thread waits meanwhile,
    so Spinus cannot draw, and the desktop may keep the plugin's window behind Spinus's (KDE Plasma's
    focus-stealing prevention does): Spinus looks frozen. While Spinus waits for longer than a second, a
    background thread of this class finds the windows Wine shows on its own (top-level, not embedded in
    Spinus's) and asks the desktop to bring them to the front, and adds a note to the title of Spinus's
    windows (the desktop draws titles, so the note shows while Spinus cannot draw).

    "Waiting" is either Spinus's message thread not responding (watchMessageThread(): a heartbeat timer) or
    a ScopedWait in progress on any thread (e.g. the plugin scanner waiting for a Windows plugin).

    X11 only (Spinus's windows on Linux are X11, also under XWayland and in the Flatpak); does nothing on
    Windows and without an X display. Never runs on the audio thread. */
class PluginDialogs
{
public:
    struct Options
    {
        int firstAfterMs = 1000; // waiting this long: bring the plugin's windows to the front
        int everyMs = 1000;      // then look again this often while still waiting
        /** What one look does (tests replace it); empty: WineWindows::bringToFront(). */
        std::function<void()> bringToFront;
        /** Called when the wait ends (tests replace it); empty: WineWindows::finish(). */
        std::function<void()> finished;
    };

    PluginDialogs();
    explicit PluginDialogs (Options);
    ~PluginDialogs();

    /** Watches Spinus's message thread from now on: when it does not respond for `firstAfterMs`, it is
        taken to be waiting for a plugin. Call on the message thread, after the GUI has started. */
    void watchMessageThread();

    /** Marks a wait for a Windows plugin, on any thread (not the audio thread). Nests. */
    class ScopedWait
    {
    public:
        ScopedWait();
        ~ScopedWait();
        ScopedWait (const ScopedWait&) = delete;
        ScopedWait& operator= (const ScopedWait&) = delete;

    private:
        juce::uint64 id = 0;
    };

    /** True if Spinus knows this plugin file as a Windows plugin through yabridge (its bridged folder, a
        yabridge setup of the user's own, or a Wine folder). */
    static bool isWindowsPlugin (const juce::String& fileOrIdentifier);

    /** How many times a wait was noticed so far (tests). */
    int getNumberOfWaitsNoticed() const noexcept { return waitsNoticed.load(); }

private:
    class Watcher;
    class Heartbeat;
    void beat() noexcept;

    Options options;
    std::atomic<juce::uint32> lastBeat { 0 };
    std::atomic<bool> watchingMessageThread { false };
    std::atomic<int> waitsNoticed { 0 };
    std::unique_ptr<Heartbeat> heartbeat;
    std::unique_ptr<Watcher> watcher;
};

/** The X11 side of PluginDialogs: one wait, from its first look to its end. Uses its own connection to the
    X display, so it never needs Spinus's message thread. */
class WineWindows
{
public:
    /** `ownProcess`: the process whose windows are Spinus's (normally this one). */
    explicit WineWindows (int ownProcess = -1);
    ~WineWindows();

    /** One look: Wine's visible top-level windows (WM_CLASS "*.exe" or "Wine", not Wine's own explorer.exe,
        not Spinus's) are activated as a pager would (_NET_ACTIVE_WINDOW with source 2, which focus-stealing
        prevention lets through) and raised. A window is activated when first seen, and again later only
        while one of Spinus's windows (or none) is the active one, so a browser the plugin opened, for
        example, keeps the front. Adds the waiting note to Spinus's window titles while any was found.
        Returns how many windows it activated. No display: returns 0. */
    int bringToFront();

    /** Ends the wait: Spinus's window titles back as they were (unless changed since), connection closed. */
    void finish();

    /** The note added to Spinus's window titles. */
    static juce::String getTitleNote();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace spinus::plugins
