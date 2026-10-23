#include <catch2/catch_session.hpp>

#include "core/FileDialog.h"
#include "engine/EngineHost.h"

#include <juce_events/juce_events.h>

// JUCE needs its message system initialised before any Engine is created.
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Never read or write the user's own Spinus settings (~/.config/Spinus DAW/).
    spinus::EngineHost::setSettingsName ("Spinus DAW Tests");
    // Never show the desktop's file dialog through the user's session bus: Spinus's own browser, unless a
    // test starts a bus of its own (tests/core/FileDialogTests.cpp).
    spinus::core::FileDialog::setBusAddress (juce::String());
    return Catch::Session().run (argc, argv);
}
