// The X11 declarations JUCE keeps private by default; only this file needs them.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "WindowStacking.h"

namespace spinus::ui
{

void keepAboveOwner (juce::Component& window, juce::Component& owner)
{
#if JUCE_LINUX || JUCE_BSD
    auto* top = owner.getTopLevelComponent();
    if (window.getPeer() == nullptr || top == nullptr || top->getPeer() == nullptr)
        return;

    auto* display = juce::XWindowSystem::getInstance()->getDisplay();
    const auto child = (::Window)(juce::pointer_sized_uint)window.getWindowHandle();
    const auto parent = (::Window)(juce::pointer_sized_uint)top->getWindowHandle();
    if (display == nullptr || child == 0 || parent == 0)
        return;

    juce::XWindowSystemUtilities::ScopedXLock lock;
    auto* x11 = juce::X11Symbols::getInstance();
    x11->xChangeProperty (display, child, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32, PropModeReplace,
                          reinterpret_cast<const unsigned char*> (&parent), 1);
    x11->xFlush (display);
#else
    juce::ignoreUnused (window, owner);
#endif
}

} // namespace spinus::ui
