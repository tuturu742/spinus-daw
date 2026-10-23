#include <catch2/catch_test_macros.hpp>

#include "engine/EngineHost.h"

using namespace spinus;

TEST_CASE ("EngineHost starts headless without an audio device", "[engine]")
{
    EngineHost host ({ .openAudioDevice = false });

    CHECK (host.describeAudioDevice() == "No audio device open");
}

TEST_CASE ("A Tracktion Edit can be created and holds an audio track", "[engine]")
{
    EngineHost host ({ .openAudioDevice = false });

    auto edit = tracktion::Edit::createSingleTrackEdit (host.getEngine());
    REQUIRE (edit != nullptr);

    auto tracks = tracktion::getAudioTracks (*edit);
    CHECK (tracks.size() == 1);
}
