#include <catch2/catch_test_macros.hpp>

#include "Acceptance.h"
#include "AudioChecks.h"
#include "model/StockDevices.h"

/** The core acceptance criteria in one go, done the way a user does them (through the panels):
    a song with every kind of instrument, effects, an audio clip and an automation clip; exported; saved,
    closed and opened again; everything undone and redone. Each time it must sound the same.

    Hardware parts (installing the package, the sound card, a MIDI keyboard) are the owner's checks before a
    release. */

#if defined(SPINUS_PLUGINS_DIR) && defined(SPINUS_OWN_VST3) && defined(SPINUS_OWN_LV2)

using namespace spinus;

using namespace spinus::acceptance;

TEST_CASE ("Core acceptance: a whole song, exported, reopened, undone and redone", "[acceptance][core]")
{
    Fixture f ("spinus-core-acceptance");
    auto& history = f.session->getHistory();
    const auto sample = writeTone (f.root.getChildFile ("samples/Pluck.wav"), 0.5, 440.0);
    const auto loop = writeTone (f.root.getChildFile ("samples/Pad Loop.wav"), 4.0, 220.0);

    // 3. Instruments: Spinus's own, Surge XT, and plugins in VST3 and LV2 form.
    REQUIRE (f.rack->addChannel ("spinus.drum-machine", "Drums"));
    REQUIRE (f.rack->addChannel ("spinus.3x-osc", "Bass"));
    REQUIRE (f.rack->addSamplerChannel (sample));
    // Surge XT is not built in every build (e.g. the AddressSanitizer one).
    const bool surge = juce::File (SPINUS_PLUGINS_DIR).getChildFile ("Surge XT.clap").existsAsFile();
    if (surge)
        REQUIRE (f.rack->addChannel ("bundled.surge-xt", "Surge"));
    REQUIRE (f.rack->addChannelWith (loadPlugin ("VST3", SPINUS_OWN_VST3, f.host.getEngine()), "VST3 Osc"));
    REQUIRE (f.rack->addChannelWith (loadPlugin ("LV2", SPINUS_OWN_LV2, f.host.getEngine()), "LV2 Drums"));
    CHECK (f.session->getAdapter().getMissingDevices().empty());

    // 4. Steps and piano-roll notes.
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (f.rack->toggleStep (f.channel ("Drums").id, step));
    for (int step : { 2, 6, 10, 14 })
        REQUIRE (f.rack->toggleStep (f.channel ("LV2 Drums").id, step));
    REQUIRE (f.rack->toggleStep (f.channel ("Pluck").id, 3));
    f.session->setSelectedChannel (f.channel ("Bass").id);
    f.roll->handleUpdateNowIfNeeded();
    for (int i = 0; i < 4; ++i)
        REQUIRE (f.roll->addNote (36 + (i % 2) * 7, i * model::ticksPerQuarterNote, 400, 0.8f));
    if (surge)
    {
        f.session->setSelectedChannel (f.channel ("Surge").id);
        f.roll->handleUpdateNowIfNeeded();
        REQUIRE (f.roll->addNote (60, 0, 2 * model::ticksPerQuarterNote, 0.9f));
    }
    f.session->setSelectedChannel (f.channel ("VST3 Osc").id);
    f.roll->handleUpdateNowIfNeeded();
    REQUIRE (f.roll->addNote (72, 2 * model::ticksPerQuarterNote, model::ticksPerQuarterNote, 0.7f));

    // 5. Two bars of the pattern and an audio clip on the playlist.
    REQUIRE (f.playlist->placePattern (0, 0));
    REQUIRE (f.playlist->placePattern (0, bar));
    REQUIRE (f.playlist->addAudioFile (1, 0, loop));

    // 6. Every channel has its own insert. Effects: EQ, compressor, reverb, delay (Surge XT Effects),
    //    limiter; an automation clip on the bass volume.
    const auto& project = f.session->getProject();
    std::set<model::InsertId> targets;
    for (auto& c : project.channels)
        targets.insert (c.targetInsert);
    CHECK (targets.size() == project.channels.size());
    const auto bassInsert = f.channel ("Bass").targetInsert;
    REQUIRE (f.mixer->addEffect (bassInsert, "spinus.eq"));
    REQUIRE (f.mixer->addEffect (bassInsert, "spinus.compressor"));
    if (surge)
        REQUIRE (f.mixer->addEffect (f.channel ("Surge").targetInsert, "bundled.surge-xt-effects"));
    REQUIRE (f.mixer->addEffect (f.channel ("Drums").targetInsert, "bundled.dragonfly.hall"));
    REQUIRE (f.mixer->addEffect (model::masterInsertId, "spinus.limiter"));
    model::ParameterTarget bassVolume;
    bassVolume.kind = model::ParameterTarget::Kind::insertVolume;
    bassVolume.insert = bassInsert;
    REQUIRE (f.session->createAutomationClip (bassVolume).isValid());
    for (auto id : f.session->getAdapter().getMissingDevices())
        UNSCOPED_INFO ("missing: " << model::getDeviceDisplayName (*f.session->getProject().findDevice (id)));
    CHECK (f.session->getAdapter().getMissingDevices().empty());
    f.session->setMode (Session::Mode::song);

    // 9. Export: the mix and the stems.
    const auto first = f.exportSong ("first", true);
    CHECK (first.seconds > 5.0);
    CHECK (rmsOf (first, 0.1, 7.5) > 0.01f);
    CHECK (f.lastFiles.size() == 1 + project.channels.size());
    const auto firstFiles = f.lastFiles;

    // 8. Saved, closed, opened again: the same sound.
    REQUIRE (f.document->saveAs (f.root.getChildFile ("Acceptance")).wasOk());
    f.document->newProject();
    CHECK (f.session->getProject().channels.empty());
    REQUIRE (f.document->open (f.root.getChildFile ("Acceptance")).wasOk());
    CHECK (f.session->getAdapter().getMissingDevices().empty());
    CHECK (f.session->getAdapter().getMissingAudioClips().empty());
    f.session->setMode (Session::Mode::song);
    const auto reopened = f.exportSong ("reopened", true);
    REQUIRE (f.lastFiles.size() == firstFiles.size());
    for (size_t i = 1; i < firstFiles.size(); ++i)
    {
        // Each insert's stem. Surge XT is never exactly the same twice (its oscillators drift at
        // random, like analog ones), so it only has to be as loud.
        INFO (firstFiles[i].getFileName());
        const auto before = readWav (firstFiles[i]), after = readWav (f.lastFiles[i]);
        if (firstFiles[i].getFileName().contains ("Surge"))
            CHECK (std::abs (rmsOf (after, 0.0, 4.0) / rmsOf (before, 0.0, 4.0) - 1.0f) < 0.1f);
        else
            CHECK (difference (before, after) < 0.001f);
    }

    // 7. Opening starts a fresh undo history. A song made step by step undoes back to nothing, and
    //    redoes to the same sound.
    CHECK (! history.canUndo());
    f.document->newProject();
    REQUIRE (f.rack->addChannel ("spinus.3x-osc", "Bass"));
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (f.rack->toggleStep (f.channel ("Bass").id, step));
    REQUIRE (f.playlist->placePattern (0, 0));
    REQUIRE (f.playlist->addAudioFile (1, 0, loop));
    REQUIRE (f.mixer->addEffect (f.channel ("Bass").targetInsert, "spinus.eq"));
    bassVolume.insert = f.channel ("Bass").targetInsert;
    REQUIRE (f.session->createAutomationClip (bassVolume).isValid());
    f.session->setMode (Session::Mode::song);
    const auto built = f.exportSong ("built");
    const auto builtProject = f.session->getProject();

    int steps = 0;
    while (history.canUndo())
    {
        REQUIRE (history.undo());
        ++steps;
    }
    CHECK (steps >= 9);
    CHECK (f.session->getProject().channels.empty());
    CHECK (f.session->getProject().playlist.clips.empty());
    while (history.canRedo())
        REQUIRE (history.redo());
    CHECK (f.session->getProject() == builtProject);
    CHECK (difference (built, f.exportSong ("redone")) < 0.01f);
}

#endif
