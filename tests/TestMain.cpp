#include <catch2/catch_session.hpp>

#include "core/FileDialog.h"
#include "engine/EngineHost.h"

#include <juce_events/juce_events.h>

#include <cstdlib>

#if JUCE_LINUX
#include <dlfcn.h>
#endif

#if JUCE_LINUX
namespace
{
/** X errors are ignored, as JUCE's own handler does while it runs. JUCE puts back the handler it found when it
    shuts down, and an error still on its way then (a window that closed a moment before) made Xlib's default
    handler end the test program as failed: tests running in parallel on one display hit it now and then. With this
    handler installed first, JUCE puts this one back. libX11 is loaded as JUCE loads it, so it's the same handler. */
void ignoreXErrors()
{
    using Handler = int (*) (void*, void*);
    using SetHandler = Handler (*) (Handler);
    if (auto* x11 = dlopen ("libX11.so.6", RTLD_NOW | RTLD_GLOBAL))
        if (auto* set = reinterpret_cast<SetHandler> (dlsym (x11, "XSetErrorHandler"))) // NOLINT
            set ([] (void*, void*) { return 0; });
}
} // namespace
#endif

// JUCE needs its message system initialised before any Engine is created.
int main (int argc, char* argv[])
{
#if JUCE_LINUX
    ignoreXErrors();
#endif
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Never read or write the user's own Spinus settings (~/.config/Spinus DAW/).
    spinus::EngineHost::setSettingsName ("Spinus DAW Tests");
    // Nor its logs: no test starts a session log of the user's (tests that check logging use folders of their
    // own), and helper programs the tests start do not write into the log of a Spinus that started this.
#if JUCE_WINDOWS
    _wputenv_s (L"SPINUS_LOG_FILE", L"");
#else
    unsetenv ("SPINUS_LOG_FILE"); // NOLINT(concurrency-mt-unsafe): before any other thread
#endif
    // Never show the desktop's file dialog through the user's session bus: Spinus's own browser, unless a
    // test starts a bus of its own (tests/core/FileDialogTests.cpp).
    spinus::core::FileDialog::setBusAddress (juce::String());
    return Catch::Session().run (argc, argv);
}
