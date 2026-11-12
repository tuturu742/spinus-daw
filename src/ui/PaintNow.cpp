// The X11 declarations JUCE keeps private by default; only this file (and WindowStacking.cpp) needs them.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "PaintNow.h"

namespace spinus::ui
{

namespace
{
#if JUCE_LINUX || JUCE_BSD
::Window windowOf (const juce::ComponentPeer& peer)
{
    return (::Window)(juce::pointer_sized_uint)peer.getNativeHandle();
}
#endif

void poll (const juce::Component::SafePointer<juce::Component>& target, std::function<void()> then,
           juce::uint32 deadline, int ticksOnScreen)
{
    juce::Timer::callAfterDelay (20,
                                 [target, then = std::move (then), deadline, ticksOnScreen]() mutable
                                 {
                                     if (target == nullptr)
                                         return;
                                     ticksOnScreen = isOnScreen (*target) ? ticksOnScreen + 1 : 0;
                                     // A few ticks on screen: JUCE has handled the first Expose and the window
                                     // manager's resize.
                                     if (ticksOnScreen < 3 && juce::Time::getMillisecondCounter() < deadline)
                                     {
                                         poll (target, std::move (then), deadline, ticksOnScreen);
                                         return;
                                     }
                                     if (then)
                                         then();
                                 });
}
} // namespace

void paintNow (juce::Component& component)
{
    auto* peer = component.getPeer();
    if (peer == nullptr)
        return;
    component.repaint();

#if JUCE_LINUX || JUCE_BSD
    auto* windowSystem = juce::XWindowSystem::getInstanceWithoutCreating();
    auto* display = windowSystem != nullptr ? windowSystem->getDisplay() : nullptr;
    if (display != nullptr)
    {
        auto* x11 = juce::X11Symbols::getInstance();
        const auto window = windowOf (*peer);
        {
            // Every frame sent so far is done once the server answers, and its "done" event is queued.
            juce::XWindowSystemUtilities::ScopedXLock lock;
            x11->xSync (display, False);
        }
        // Takes only those events off the queue (what the message loop would do), so JUCE paints again.
        windowSystem->processPendingPaintsForWindow (window);
        peer->performAnyPendingRepaintsNow();
        juce::XWindowSystemUtilities::ScopedXLock lock;
        x11->xFlush (display); // the new frame goes to the server now, not when the loop runs again
        return;
    }
#endif

    peer->performAnyPendingRepaintsNow();
}

bool isOnScreen (const juce::Component& component)
{
    auto* peer = component.getPeer();
    if (peer == nullptr || ! component.isShowing())
        return false;

#if JUCE_LINUX || JUCE_BSD
    auto* windowSystem = juce::XWindowSystem::getInstanceWithoutCreating();
    auto* display = windowSystem != nullptr ? windowSystem->getDisplay() : nullptr;
    if (display == nullptr)
        return false;
    XWindowAttributes attributes {};
    juce::XWindowSystemUtilities::ScopedXLock lock;
    if (juce::X11Symbols::getInstance()->xGetWindowAttributes (display, windowOf (*peer), &attributes) == 0)
        return false;
    return attributes.map_state == IsViewable;
#else
    return true;
#endif
}

void callWhenOnScreen (juce::Component& component, std::function<void()> then, int timeoutMs)
{
    poll (&component, std::move (then), juce::Time::getMillisecondCounter() + (juce::uint32)juce::jmax (0, timeoutMs),
          0);
}

} // namespace spinus::ui
