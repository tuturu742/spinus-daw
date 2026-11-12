#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace spinus::ui
{

/** Repaints `component` and puts the result on the screen now, for the rare moments the message thread is busy
    (opening a project) and the usual repaint would wait for the end.

    ComponentPeer::performAnyPendingRepaintsNow() alone is not enough on Linux: JUCE sends each frame to the X
    server through shared memory and then paints nothing more into that window until the server's "done" event
    for the frame has been read, which only the message loop does. The frame itself also waits in Xlib's output
    buffer until something flushes it. So during a busy message thread it paints at most one frame, which the
    server may not even get. This waits for the server to finish the frames already sent, takes their "done"
    events, paints, and flushes. Nothing else is read from the X server, so no click or key runs in between.

    Does nothing when the component has no window. */
void paintNow (juce::Component& component);

/** True once the component's window is on the screen: on Linux, mapped by the window manager (a window only just
    made visible is not yet, and what is painted into it before that is lost). */
bool isOnScreen (const juce::Component& component);

/** Calls `then` on the message thread once `component`'s window has been on the screen for a moment, so that a
    long job started from `then` can show progress with paintNow; or after `timeoutMs`, whatever happens. Not
    called if the component is deleted first. */
void callWhenOnScreen (juce::Component& component, std::function<void()> then, int timeoutMs = 3000);

} // namespace spinus::ui
