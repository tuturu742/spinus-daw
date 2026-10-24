#include <catch2/catch_session.hpp>

#include "core/FileDialog.h"
#include "engine/EngineHost.h"

#include <juce_events/juce_events.h>

#include <cstdlib>

// JUCE needs its message system initialised before any Engine is created.
int main (int argc, char* argv[])
{
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
