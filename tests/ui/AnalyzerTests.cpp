#include <catch2/catch_test_macros.hpp>

#include "core/ChildProcess.h"
#include "FakeProvider.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "plugins/PluginCatalog.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "AnalysisFakes.h"
#include "ProgramsTestPlugin.h"
#include "ui/Analyzer.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;

using namespace spinus::test::analysis;

TEST_CASE ("Analyzer: opens with the presets it counted before", "[ui][ai][analyzer]")
{
    // Read while Spinus opens: a crash at every start if it is read wrongly (seen with one Windows plugin).
    TempFolder t;
    REQUIRE (t.root.getChildFile ("ai").createDirectory().wasOk());
    REQUIRE (t.root.getChildFile ("ai/instrument-presets.json")
                 .replaceWithText (R"({ "VST3|/plugins/AME.vst3|Ample Metal Eclipse|1792567665000": 0 })"));
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    ai::AiService service (t.root, t.root.getChildFile ("models"));
    Analyzer analyzer (session, &service);
    analyzer.setService (&service);
    analyzer.setService (nullptr);
}

TEST_CASE ("Analyzer: says what is missing, finds tempo and key, separates, adds the stems", "[ui][ai][analyzer]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto models = t.root.getChildFile ("models");
    ai::AiService service (t.root, models);
    Analyzer analyzer (session, &service);
    analyzer.setSize (660, 460);
    const auto song = makeSong (t.root);

    // AI off: it says so.
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }));
    CHECK (analyzer.getPreparedFile().existsAsFile()); // the song was prepared anyway
    CHECK (analyzer.getNeedsText().contains ("turn on AI"));

    // AI on, no engine: it offers to set it up.
    service.setEnabled (true);
    bool setUp = false;
    analyzer.onSetUpEngine = [&] { setUp = true; };
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }));
    CHECK (analyzer.getNeedsText().contains ("analysis engine"));

    // Install the engine (the stand-in), then: the model files are missing, with their size.
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }));
    CHECK (analyzer.getNeedsText().contains ("MB"));
    CHECK (analyzer.getDownloadSize() == 84025440 + 21 + 8451101 + 230444); // Demucs, Beat This!, Basic Pitch
    // Each says its licence and where to read about it.
    REQUIRE (analyzer.getMissingSources().size() == 3);
    for (const auto& source : analyzer.getMissingSources())
    {
        CHECK ((source.license == "MIT" || source.license == "Apache-2.0"));
        CHECK (source.page.startsWith ("https://"));
    }

    // With the model files there: tempo, beats per bar and key.
    placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
    placeModel (models, "htdemucs", { { "htdemucs.yaml", 21 }, { "955717e8.safetensors", 84025440 } },
                "models: ['955717e8']\n");
    placeBasicPitch (models);
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return analyzer.getStep() == Analyzer::Step::done || ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    const auto rhythm = analyzer.getRhythm();
    REQUIRE (rhythm.has_value());
    CHECK (analyzer.getTempoText() == "120.0 BPM, 4 beats per bar");
    // Sources and licences still lists the models once they are downloaded.
    CHECK (std::count_if (analyzer.getAllSources().begin(), analyzer.getAllSources().end(),
                          [] (const Analyzer::ModelSource& m) { return m.downloaded; }) >= 3);
    CHECK (std::abs (rhythm.value_or (Analyzer::Rhythm {}).firstDownbeat - 0.1) < 1e-6);
    REQUIRE (analyzer.getKey());
    CHECK (analyzer.getKeyText().startsWith ("A minor"));

    // Use them for the song: one undo step each.
    session.getHistory().reset (model::Project());
    REQUIRE (analyzer.useTempo());
    CHECK (juce::approximatelyEqual (session.getProject().settings.tempoBpm, 120.0));
    REQUIRE (analyzer.useKey());
    CHECK (session.getProject().settings.key == model::Key { 9, "minor" });

    // Stems, then as audio on tracks of their own, the first downbeat on bar 1, as one undo step.
    REQUIRE (analyzer.separate (4));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    REQUIRE (analyzer.getStems().size() == 4);
    // Anywhere in the panel (the stems' rows are in a scrolling area), visible and laid out.
    const std::function<bool (juce::Component&, const juce::String&)> showsIn =
        [&showsIn] (juce::Component& parent, const juce::String& text)
    {
        for (auto* child : parent.getChildren())
        {
            if (auto* button = dynamic_cast<juce::TextButton*> (child);
                button != nullptr && button->getButtonText() == text)
                return button->isVisible() && ! button->getBounds().isEmpty();
            if (auto* box = dynamic_cast<juce::ComboBox*> (child); box != nullptr && box->getText() == text)
                return box->isVisible() && ! box->getBounds().isEmpty();
            if (child->isVisible() && showsIn (*child, text))
                return true;
        }
        return false;
    };
    const auto shows = [&] (const juce::String& text) { return showsIn (analyzer, text); };
    CHECK (shows ("Add to the project"));
    // Beside each stem, what happens to it: the drums and the bass replicated, the rest as audio; a
    // replicated stem also shows what plays it and how.
    CHECK (shows ("Replicate the drums"));
    CHECK (shows ("Drum Machine"));
    CHECK (shows ("Synth Kit"));
    CHECK (shows ("3x Osc"));
    CHECK (shows ("As is (default sound)"));
    CHECK (shows ("Add the vocals as audio"));
    {
        const auto uses = analyzer.getChosenUses();
        CHECK (uses.at ("bass") == Analyzer::StemUse::replicate);
        CHECK (uses.at ("other") == Analyzer::StemUse::audio);
    }
    analyzer.setChosenUse ("drums", Analyzer::StemUse::leaveOut);
    CHECK (shows ("Leave out the drums"));
    CHECK (! shows ("Drum Machine"));
    CHECK (! shows ("Synth Kit"));
    analyzer.setChosenUse ("drums", Analyzer::StemUse::replicate);
    const auto steps = session.getHistory().getUndoDescriptions().size();
    REQUIRE (analyzer.addStemsAsAudio ({ "bass", "drums", "other", "vocals" }));
    const auto& playlist = session.getProject().playlist;
    REQUIRE (playlist.lanes.size() == 4);
    for (auto& lane : playlist.lanes)
    {
        CHECK (lane.name.endsWith (" (Test Song)"));
        CHECK (! lane.muted);
    }
    REQUIRE (playlist.clips.size() == 4);
    const auto& clip = std::get<model::AudioClip> (playlist.clips[0].content);
    CHECK (clip.offset == 192); // 0.1 s at 120 BPM: the first downbeat starts bar 1
    CHECK (session.getHistory().getUndoDescriptions().size() == steps + 1);
    session.getHistory().undo();
    CHECK (session.getProject().playlist.lanes.empty());

    // Notes from the bass and the "vocals" (with Basic Pitch's real model when it is there).
    if (juce::File (juce::SystemStats::getEnvironmentVariable ("SPINUS_TEST_BASIC_PITCH", {})).existsAsFile())
    {
        session.getHistory().reset (model::Project());
        REQUIRE (analyzer.makeNotes ({ "bass", "vocals" }));
        REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
        INFO (analyzer.getStatusText());
        REQUIRE (analyzer.getTranscribed().size() == 2);
        const auto& project = session.getProject();
        REQUIRE (project.channels.size() == 2);
        CHECK (project.channels[0].name == "Bass (Test Song)");
        REQUIRE (project.patterns.size() == 1);
        CHECK (project.patterns[0].name == "Analysis: Test Song");
        CHECK (! project.patterns[0].notes.empty());
        CHECK (project.playlist.clips.size() == 1);
        CHECK (session.getHistory().getUndoDescriptions().strings.getLast() == "Notes from Test Song");
    }

    // Drums become Drum Machine steps (the recogniser learns Spinus's drum sounds the first time).
    session.getHistory().reset (model::Project());
    REQUIRE (analyzer.makeNotes ({ "drums" }));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 180000));
    INFO (analyzer.getStatusText());
    {
        const auto& project = session.getProject();
        REQUIRE (project.channels.size() == 1);
        CHECK (project.channels[0].name == "Drums (Test Song)");
        REQUIRE (project.channels[0].instrument.has_value());
        CHECK (project.channels[0].instrument.value_or (model::Device {}).stockType == "spinus.drum-machine");
        REQUIRE (project.patterns.size() == 1);
        CHECK (! project.patterns[0].notes.empty()); // the clicks
    }

    // Each stem as chosen, all in one undo step: the drums replicated, the vocals and "other" as audio,
    // the bass left out.
    session.getHistory().reset (model::Project());
    REQUIRE (analyzer.addStems ({ { "drums", Analyzer::StemUse::replicate },
                                  { "vocals", Analyzer::StemUse::audio },
                                  { "other", Analyzer::StemUse::audio },
                                  { "bass", Analyzer::StemUse::leaveOut } }));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 180000));
    INFO (analyzer.getStatusText());
    {
        const auto& project = session.getProject();
        juce::StringArray channels;
        for (auto& channel : project.channels)
            channels.add (channel.name);
        // Audio clips have channels of their own.
        CHECK (channels.joinIntoString (", ") == "Other stem (Test Song), Vocals stem (Test Song), Drums (Test Song)");
        juce::StringArray lanes;
        for (auto& lane : project.playlist.lanes)
            lanes.add (lane.name);
        CHECK (lanes.joinIntoString (", ") == "Other stem (Test Song), Vocals stem (Test Song), Notes (Test Song)");
        CHECK (project.playlist.clips.size() == 3);
        CHECK (analyzer.getStatusText().contains ("and 2 stems as audio"));
        CHECK (session.getHistory().getUndoDescriptions() == juce::StringArray { "Stems of Test Song" });
        session.getHistory().undo();
        CHECK (session.getProject().playlist.lanes.empty());
        CHECK (session.getProject().channels.empty());
    }
    // An instrument of the user's choice is not for the drums (their notes are the Drum Machine's pads),
    // and a stem to play on one needs it named.
    {
        model::Device guitar;
        guitar.kind = model::Device::Kind::plugin;
        CHECK (! analyzer.addStems ({ { "drums", Analyzer::StemUse::replicate } },
                                    { { "drums", Analyzer::Sounds::instrument } }, { { "drums", guitar } }));
        CHECK (! analyzer.addStems ({ { "bass", Analyzer::StemUse::replicate } },
                                    { { "bass", Analyzer::Sounds::instrument } }));
    }
    CHECK (! analyzer.addStems ({ { "bass", Analyzer::StemUse::leaveOut } })); // nothing to add
    CHECK (! analyzer.addStems ({ { "piano", Analyzer::StemUse::audio } }));   // no such stem

    // With the song's own sounds: the drums' pads get its hits, the bass becomes a Sampler of its phrases.
    session.getHistory().reset (model::Project());
    REQUIRE (analyzer.makeNotes ({ "bass", "drums" }, Analyzer::Sounds::song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 180000));
    INFO (analyzer.getStatusText());
    {
        const auto& project = session.getProject();
        REQUIRE (project.channels.size() == 2);
        CHECK (analyzer.getStatusText().contains ("new channels")); // the sounds were loaded too
        CHECK (project.channels[0].instrument.value_or (model::Device {}).stockType == "spinus.sampler");
        CHECK (project.channels[1].instrument.value_or (model::Device {}).stockType == "spinus.drum-machine");
        const auto& pieces = analyzer.getPieces();
        REQUIRE (pieces.contains ("bass"));
        CHECK (! pieces.at ("bass").empty());
        CHECK (pieces.at ("bass")[0].note == 60);
        CHECK (pieces.at ("bass")[0].name == "Test Song - phrase 1");
        CHECK (pieces.contains ("drums"));
        REQUIRE (project.patterns.size() == 1);
        CHECK (! project.patterns[0].notes.empty());
    }

    // With the closest Surge XT presets: the presets are measured once, the bass
    // plays on Surge XT with the closest one, and the others are offered.
    // (It also needs Basic Pitch's real model for the bass's notes.)
    if (bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile() &&
        juce::File (juce::SystemStats::getEnvironmentVariable ("SPINUS_TEST_BASIC_PITCH", {})).existsAsFile())
    {
        placeModel (
            models, "clap_music",
            { { "config.json", 628 }, { "preprocessor_config.json", 541 }, { "pytorch_model.bin", 776444665 } });
        session.getHistory().reset (model::Project());
        analyzer.limitPresetsForTests (60);
        REQUIRE (analyzer.makeNotes ({ "bass", "drums" }, Analyzer::Sounds::presets));
        REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
        INFO (analyzer.getStatusText());
        CHECK (analyzer.getStatusText().contains ("new channels"));
        REQUIRE (analyzer.getClosestPresets().contains ("bass"));
        CHECK (analyzer.getClosestPresets().at ("bass").size() == 5);
        const auto& project = session.getProject();
        REQUIRE (project.channels.size() == 2);
        CHECK (project.channels[0].instrument.value_or (model::Device {}).stockType == "bundled.surge-xt");

        // The drums: their groups, each with Synth Kit samples suggested (and the best one on its pad).
        REQUIRE (! analyzer.getDrumGroups().empty());
        const auto pad = analyzer.getDrumGroups().front().pad;
        REQUIRE (analyzer.getDrumSuggestions().contains (pad));
        CHECK (! analyzer.getDrumSuggestions().at (pad).empty());
        CHECK (analyzer.getDrumSuggestions().at (pad).front().getParentDirectory().getFileName() == "Synth Kit");
        CHECK (analyzer.useDrumSuggestion (pad, 1));
        CHECK (! analyzer.useDrumSuggestion (pad, 9));
        CHECK (analyzer.useClosestPreset ("bass", 2));
        CHECK (! analyzer.useClosestPreset ("bass", 9));
        CHECK (! analyzer.useClosestPreset ("vocals", 0));

        // Tuned: Surge XT from the closest preset, then 3x Osc, each tuned to the bass (few tries here).
        analyzer.limitTriesForTests (40);
        for (auto [sounds, stockType] : { std::pair { Analyzer::Sounds::tunedSurge, "bundled.surge-xt" },
                                          std::pair { Analyzer::Sounds::tunedThreeOsc, "spinus.3x-osc" } })
        {
            session.getHistory().reset (model::Project());
            REQUIRE (analyzer.makeNotes ({ "bass" }, sounds));
            REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
            INFO (analyzer.getStatusText());
            CHECK (analyzer.getStatusText().contains ("new channels"));
            REQUIRE (analyzer.getTuned().contains ("bass"));
            const auto& found = analyzer.getTuned().at ("bass");
            CHECK (found.tries >= 40);
            CHECK (found.distance <= found.startDistance);
            REQUIRE (session.getProject().channels.size() == 1);
            CHECK (session.getProject().channels[0].instrument.value_or (model::Device {}).stockType == stockType);
            CHECK (analyzer.getClosestPresets().empty()); // tuned: no preset list to undo the tuning
        }

        // Chosen per stem: the bass on 3x Osc tuned to it, the drums with the song's own hits, in one go.
        session.getHistory().reset (model::Project());
        REQUIRE (analyzer.makeNotesWith (
            { { "bass", Analyzer::Sounds::tunedThreeOsc }, { "drums", Analyzer::Sounds::song } }));
        REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
        INFO (analyzer.getStatusText());
        CHECK (analyzer.getStatusText().contains ("new channels"));
        CHECK (analyzer.getTuned().contains ("bass"));
        CHECK (analyzer.getPieces().contains ("drums"));
        CHECK (! analyzer.getPieces().contains ("bass"));

        // On an instrument of the user's choice (an installed plugin): the bass's notes on a channel with it.
        {
            session.getHistory().reset (model::Project());
            model::Device guitar;
            guitar.kind = model::Device::Kind::plugin;
            guitar.plugin = { "VST3", "Some Guitar", "Someone", "/nowhere/Some Guitar.vst3", 1234 };
            REQUIRE (analyzer.addStems ({ { "bass", Analyzer::StemUse::replicate } },
                                        { { "bass", Analyzer::Sounds::instrument } }, { { "bass", guitar } }));
            REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 180000));
            INFO (analyzer.getStatusText());
            const auto& made = session.getProject();
            REQUIRE (made.channels.size() == 1);
            const auto instrument = made.channels[0].instrument.value_or (model::Device {});
            CHECK (instrument.kind == model::Device::Kind::plugin);
            CHECK (instrument.plugin.name == "Some Guitar");
            REQUIRE (made.patterns.size() == 1);
            CHECK (! made.patterns[0].notes.empty());
        }
    }

    // The drum samples folder is remembered.
    analyzer.setDrumSamplesFolder (t.root);
    {
        Analyzer again (session, &service);
        CHECK (again.getDrumSamplesFolder() == t.root);
    }

    // The same song again: stems from the cache.
    REQUIRE (analyzer.separate (4));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    CHECK (analyzer.getStems().size() == 4);
    service.setEnabled (false);
}

TEST_CASE ("Analyzer screenshot for the manual", "[.screenshot][analyzer]")
{
    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    const auto models = t.root.getChildFile ("models");
    ai::AiService service (t.root, models);
    service.setEnabled (true);
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
    placeModel (models, "htdemucs", { { "htdemucs.yaml", 21 }, { "955717e8.safetensors", 84025440 } },
                "models: ['955717e8']\n");
    placeBasicPitch (models);
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    Analyzer analyzer (session, &service);
    analyzer.setLookAndFeel (&lookAndFeel);
    analyzer.setSize (660, 560);
    REQUIRE (analyzer.analyze (makeSong (t.root, "Night Drive")));
    REQUIRE (waitFor ([&] { return analyzer.getStep() == Analyzer::Step::done; }, 60000));
    REQUIRE (analyzer.separate (4));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    const auto image = analyzer.createComponentSnapshot (analyzer.getLocalBounds(), true, 1.0f);
    analyzer.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/analyzer.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    service.setEnabled (false);
}

TEST_CASE ("Analyzer: reads Opus and M4A through ffmpeg", "[ui][analyzer]")
{
    TempFolder t;
    const auto song = t.root.getChildFile ("tone.opus");
    spinus::core::ChildProcess make;
    if (! make.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i",
                                          "sine=frequency=440:duration=2", song.getFullPathName() }) ||
        ! make.waitForProcessToFinish (30000) || ! song.existsAsFile())
        SKIP ("ffmpeg is not installed");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    Analyzer analyzer (session, nullptr);
    CHECK (analyzer.isInterestedInFileDrag ({ song.getFullPathName() }));
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 30000));
    const auto prepared = analyzer.getPreparedFile();
    REQUIRE (prepared.existsAsFile());
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (prepared.createInputStream().release(), true));
    REQUIRE (reader != nullptr);
    CHECK (reader->numChannels == 2);
    CHECK (std::abs ((double)reader->lengthInSamples / reader->sampleRate - 2.0) < 0.05);

    // Not a song at all: ffmpeg says so, and so does the Analyzer.
    const auto broken = t.root.getChildFile ("broken.m4a");
    broken.replaceWithText ("not audio");
    REQUIRE (analyzer.analyze (broken));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 30000));
    CHECK (analyzer.getStatusText().contains ("cannot read broken.m4a"));
}

TEST_CASE ("Analyzer: many instruments is a third choice, and stems get readable names", "[ui][analyzer]")
{
    CHECK (Analyzer::stemDisplayName ("lead-vocal") == "lead vocals");
    CHECK (Analyzer::stemDisplayName ("hh") == "hi-hat");
    CHECK (Analyzer::stemDisplayName ("acoustic-guitar") == "acoustic guitar");
    CHECK (Analyzer::stemDisplayName ("bowed_strings") == "bowed strings");
    CHECK (Analyzer::stemDisplayName ("drums") == "drums");
    CHECK (Analyzer::manyInstruments == 53);
}

TEST_CASE ("Analyzer: a song that cannot be found says so", "[ui][analyzer]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    Analyzer analyzer (session, nullptr);
    const auto missing = t.root.getChildFile ("???????.opus"); // how a mangled Cyrillic name arrives
    CHECK (! analyzer.analyze (missing));
    CHECK (! analyzer.isBusy());
    CHECK (analyzer.getStatusText().contains ("cannot find"));
    CHECK (analyzer.getStatusText().contains ("???????.opus"));
}

TEST_CASE ("Analyzer: what it needs to download follows the Stems choice", "[ui][ai][analyzer]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    const auto models = t.root.getChildFile ("models");
    ai::AiService service (t.root, models);
    service.setEnabled (true);
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    Analyzer analyzer (session, &service);
    analyzer.setSize (660, 560);
    const auto song = makeSong (t.root);

    // Nothing downloaded: with 4 stems chosen, Demucs's 4-stem model.
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    CHECK (analyzer.getNeedsText().contains ("Demucs, 4 stems"));
    CHECK (! analyzer.getNeedsText().contains ("MVSep"));
    constexpr juce::int64 beats = 8451101, notes = 230444, many = 1368919887 + 4184;
    CHECK (analyzer.getDownloadSize() == beats + notes + 84025440 + 21);

    // Many instruments chosen: at once the MVSep model instead, and back again.
    analyzer.setStemChoice (Analyzer::manyInstruments);
    CHECK (analyzer.getNeedsText().contains ("MVSep"));
    CHECK (! analyzer.getNeedsText().contains ("Demucs"));
    CHECK (analyzer.getDownloadSize() == beats + notes + many);
    analyzer.setStemChoice (6);
    CHECK (analyzer.getNeedsText().contains ("Demucs, 6 stems"));
    analyzer.setStemChoice (4);
    CHECK (analyzer.getNeedsText().contains ("Demucs, 4 stems"));
    CHECK (! analyzer.getNeedsText().contains ("MVSep"));

    // With what many instruments needs downloaded (and not Demucs), the song is analyzed on that choice...
    placeModel (models, "beat_this_small0", { { "small0.ckpt", beats } });
    placeModel (models, "basic_pitch", { { "nmp.onnx", notes } });
    placeManyInstruments (models);
    analyzer.setStemChoice (Analyzer::manyInstruments);
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return analyzer.getStep() == Analyzer::Step::done || ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getRhythm().has_value());
    CHECK (analyzer.getNeedsText().isEmpty());
    // ...and 4 stems then needs only Demucs, many instruments nothing.
    analyzer.setStemChoice (4);
    CHECK (analyzer.getNeedsText().contains ("Demucs, 4 stems"));
    CHECK (! analyzer.getNeedsText().contains ("MVSep"));
    CHECK (! analyzer.getNeedsText().contains ("Beat This"));
    analyzer.setStemChoice (Analyzer::manyInstruments);
    CHECK (analyzer.getNeedsText().isEmpty());
    service.setEnabled (false);
}

TEST_CASE ("Analyzer: many instruments, replicated and tuned, added in one undo step", "[ui][ai][analyzer]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    session.getDeviceList().setBundledFolders ({ bundledPlugins() });
    const auto models = t.root.getChildFile ("models");
    ai::AiService service (t.root, models);
    service.setEnabled (true);
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
    placeModel (models, "basic_pitch", { { "nmp.onnx", 230444 } });
    placeManyInstruments (models);

    Analyzer analyzer (session, &service);
    analyzer.setSize (660, 560);
    analyzer.setStemChoice (Analyzer::manyInstruments);
    REQUIRE (analyzer.analyze (makeSong (t.root)));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    REQUIRE (analyzer.getRhythm().has_value());

    // What MVSep's model keeps of a song: instruments by its names, two of them with notes (as if Basic Pitch
    // had heard them before).
    const auto cache = analyzer.getCacheFolder();
    std::map<juce::String, juce::File> made;
    const std::map<juce::String, double> tones { { "accordion", 440.0 }, { "bass", 55.0 },
                                                 { "drums", 0.0 },       { "electric-guitar", 196.0 },
                                                 { "keys", 261.63 },     { "lead-vocal", 329.63 },
                                                 { "strings", 293.66 },  { "synth", 523.25 },
                                                 { "wind", 349.23 } };
    for (auto& [name, frequency] : tones)
        made[name] = makeStem (t.root.getChildFile ("many").getChildFile (name + ".wav"), frequency, name == "drums");
    placeNotes (cache, made["accordion"], { { 69, 0.1, 0.6 }, { 69, 1.1, 1.6 }, { 69, 2.1, 2.6 }, { 69, 3.1, 3.6 } });
    placeNotes (cache, made["bass"], { { 33, 0.1, 0.9 }, { 33, 1.1, 1.9 }, { 33, 2.1, 2.9 } });
    placeSeparation (cache, analyzer.getPreparedFile(), "mvsep_mega_53", "v1.0.21", made);

    REQUIRE (analyzer.separate (Analyzer::manyInstruments));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    REQUIRE (analyzer.getStems().size() == tones.size());
    CHECK (analyzer.getNeedsText().isEmpty());

    // Each row: what happens to the stem; replicated, what plays it (each instrument once) and how.
    using Way = Analyzer::Way;
    const auto sources = analyzer.getSoundSources (false);
    INFO (sources.joinIntoString (", "));
    CHECK (sources[0] == "Its own phrases (Sampler)");
    CHECK (sources.contains ("3x Osc"));
    CHECK (! sources.contains ("Sampler"));
    CHECK (! sources.contains ("Drum Machine"));
    for (int i = 2; i < sources.size(); ++i) // the instruments: one list, in alphabetical order
        CHECK (sources[i - 1].compareNatural (sources[i]) <= 0);
    CHECK (analyzer.getSoundSources (true) == juce::StringArray { "Drum Machine", "Its own hits (Drum Machine)" });
    CHECK (analyzer.getChosenSource ("bass") == "3x Osc");
    CHECK (analyzer.getWays ("bass") == std::vector<Way> { Way::asIs, Way::tuned });
    CHECK (analyzer.getChosenSource ("drums") == "Drum Machine");
    CHECK (analyzer.getWays ("drums") == std::vector<Way> { Way::asIs, Way::closestPreset });
    CHECK (analyzer.getChosenUses().at ("accordion") == Analyzer::StemUse::audio);
    analyzer.setChosenUse ("accordion", Analyzer::StemUse::replicate);
    REQUIRE (analyzer.setChosenSound ("accordion", "Its own phrases (Sampler)"));
    CHECK (analyzer.getWays ("accordion") == std::vector<Way> { Way::asIs }); // no second list
    CHECK (! analyzer.setChosenSound ("accordion", "Its own phrases (Sampler)", Way::tuned));
    CHECK (analyzer.getChosenSounds().at ("accordion") == Analyzer::Sounds::song);

    // The accordion on a synth tuned to it: Surge XT when it is built (its tuning compares sounds by ear, so
    // the window says at once that CLAP is needed), else 3x Osc.
    const bool surge = bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile();
    CHECK (sources.contains ("Surge XT") == surge);
    CHECK (std::count (sources.begin(), sources.end(), juce::String ("Surge XT")) == (surge ? 1 : 0));
    REQUIRE (analyzer.setChosenSound ("accordion", surge ? "Surge XT" : "3x Osc", Way::tuned));
    CHECK (analyzer.getChosenSounds().at ("accordion") ==
           (surge ? Analyzer::Sounds::tunedSurge : Analyzer::Sounds::tunedThreeOsc));
    if (surge)
    {
        CHECK (analyzer.getWays ("accordion") == std::vector<Way> { Way::asIs, Way::closestPreset, Way::tuned });
        CHECK (analyzer.getNeedsText().contains ("CLAP"));
        // Plain Surge XT plays it as is: no CLAP then.
        REQUIRE (analyzer.setChosenSound ("accordion", "Surge XT", Way::asIs));
        CHECK (analyzer.getChosenSounds().at ("accordion") == Analyzer::Sounds::instrument);
        CHECK (analyzer.getChosenInstruments().at ("accordion").stockType == "bundled.surge-xt");
        CHECK (analyzer.getNeedsText().isEmpty());
        REQUIRE (analyzer.setChosenSound ("accordion", "Surge XT", Way::tuned));
        placeModel (
            models, "clap_music",
            { { "config.json", 628 }, { "preprocessor_config.json", 541 }, { "pytorch_model.bin", 776444665 } });
    }

    // "Add to the project": the accordion, the bass and the drums replicated, the rest as audio, in one step.
    session.getHistory().reset (model::Project());
    analyzer.limitTriesForTests (20);
    analyzer.limitPresetsForTests (4);
    REQUIRE (analyzer.addChosen());
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getStatusText().contains ("3 new channels"));
    CHECK (analyzer.getStatusText().contains ("6 stems as audio"));
    CHECK (analyzer.getTuned().contains ("accordion"));
    {
        const auto& project = session.getProject();
        std::map<juce::String, juce::String> channels;
        for (auto& channel : project.channels)
            channels[channel.name] = channel.instrument.value_or (model::Device {}).stockType;
        CHECK (channels.size() == tones.size());
        CHECK (channels["Accordion (Test Song)"] == (surge ? "bundled.surge-xt" : "spinus.3x-osc"));
        CHECK (channels["Bass (Test Song)"] == "spinus.3x-osc");
        CHECK (channels["Drums (Test Song)"] == "spinus.drum-machine");
        CHECK (channels.contains ("Lead-vocal stem (Test Song)"));
        CHECK (project.playlist.lanes.size() == 7);
        CHECK (session.getHistory().getUndoDescriptions() == juce::StringArray { "Stems of Test Song" });
        session.getHistory().undo();
        CHECK (session.getProject().channels.empty());
    }

    // When nothing can be added, the status line says why.
    analyzer.setChosenUse ("accordion", Analyzer::StemUse::leaveOut);
    for (auto& [name, use] : analyzer.getChosenUses())
        analyzer.setChosenUse (name, Analyzer::StemUse::leaveOut);
    CHECK (! analyzer.addChosen());
    CHECK (analyzer.getStatusText().startsWith ("Nothing was added"));
    made["wind"].deleteFile();
    CHECK (! analyzer.addStemsAsAudio ({ "wind" }));
    CHECK (analyzer.getStatusText().contains ("cannot read the wind stem"));
    CHECK (session.getHistory().getUndoDescriptions().isEmpty());
    service.setEnabled (false);
}

TEST_CASE ("Analyzer: an installed instrument tuned (experimental), and one with nothing to tune as it is",
           "[ui][ai][analyzer]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    session.getDeviceList().setBundledFolders ({ bundledPlugins() });
    // Two small test instruments, installed: one with settings to tune, one (an organ) without.
    plugins::PluginCatalog catalog (t.root.getChildFile ("settings"), t.root);
    {
        plugins::ClapPluginFormat format;
        juce::OwnedArray<juce::PluginDescription> found;
        format.findAllTypesForFile (
            found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName());
        REQUIRE (found.size() == 2);
        for (auto* d : found)
        {
            catalog.getKnownPlugins().addType (*d);
            host.getEngine().getPluginManager().knownPluginList.addType (*d);
        }
    }
    session.getDeviceList().setCatalog (&catalog);

    const auto models = t.root.getChildFile ("models");
    ai::AiService service (t.root, models);
    service.setEnabled (true);
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
    placeModel (models, "basic_pitch", { { "nmp.onnx", 230444 } });
    placeManyInstruments (models);

    Analyzer analyzer (session, &service);
    analyzer.setSize (660, 560);
    analyzer.setStemChoice (Analyzer::manyInstruments);
    REQUIRE (analyzer.analyze (makeSong (t.root)));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    const auto cache = analyzer.getCacheFolder();
    std::map<juce::String, juce::File> made;
    for (auto [name, frequency] : { std::pair { "accordion", 440.0 }, std::pair { "bass", 55.0 } })
        made[name] =
            makeStem (t.root.getChildFile ("many").getChildFile (juce::String (name) + ".wav"), frequency, false);
    placeNotes (cache, made["accordion"], { { 69, 0.1, 0.6 }, { 69, 1.1, 1.6 }, { 69, 2.1, 2.6 }, { 69, 3.1, 3.6 } });
    placeNotes (cache, made["bass"], { { 33, 0.1, 0.9 }, { 33, 1.1, 1.9 }, { 33, 2.1, 2.9 } });
    placeSeparation (cache, analyzer.getPreparedFile(), "mvsep_mega_53", "v1.0.21", made);
    REQUIRE (analyzer.separate (Analyzer::manyInstruments));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    REQUIRE (analyzer.getStems().size() == 2);

    // Spinus's own synth is tuned as before; an installed one is offered too, marked experimental.
    using Way = Analyzer::Way;
    CHECK (analyzer.getWayNames ("bass") == juce::StringArray { "As is (default sound)", "Tuned to it (slow)" });
    const auto sources = analyzer.getSoundSources (false);
    INFO (sources.joinIntoString (", "));
    REQUIRE (sources.contains ("Spinus Test Synth (CLAP)"));
    REQUIRE (sources.contains ("Spinus Test Organ (CLAP)"));
    REQUIRE (analyzer.setChosenSound ("bass", "Spinus Test Synth (CLAP)", Way::tuned));
    CHECK (analyzer.getWays ("bass") == std::vector<Way> { Way::asIs, Way::closestPreset, Way::tuned });
    CHECK (analyzer.getWayNames ("bass") ==
           juce::StringArray { "As is (default sound)", "Closest preset", "Tuned to it (experimental)" });
    CHECK (analyzer.getChosenSounds().at ("bass") == Analyzer::Sounds::tunedInstrument);
    CHECK (analyzer.getChosenInstruments().at ("bass").plugin.name == "Spinus Test Synth");
    CHECK (analyzer.getNeedsText().isEmpty()); // tuning compares sounds itself: no CLAP model needed
    analyzer.setChosenUse ("accordion", Analyzer::StemUse::replicate);
    REQUIRE (analyzer.setChosenSound ("accordion", "Spinus Test Organ (CLAP)", Way::tuned));
    // Odin 2 and OB-Xf, when they are downloaded, are tuned with Spinus's own profiles.
    for (auto* name : { "Odin 2", "OB-Xf" })
        if (sources.contains (name))
        {
            REQUIRE (analyzer.setChosenSound ("accordion", name, Way::tuned));
            CHECK (analyzer.getWayNames ("accordion").contains ("Tuned to it (slow)"));
        }
    REQUIRE (analyzer.setChosenSound ("accordion", "Spinus Test Organ (CLAP)", Way::tuned));

    // Added: the bass's synth tuned and on its channel; the organ plays as it is, and the status line says why.
    session.getHistory().reset (model::Project());
    analyzer.limitTriesForTests (30);
    REQUIRE (analyzer.addChosen());
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getStatusText().contains ("2 new channels"));
    CHECK (analyzer.getStatusText().contains (
        "Spinus Test Organ has no settings Spinus can tune; it plays its own sound."));
    REQUIRE (analyzer.getTuned().contains ("bass"));
    CHECK (! analyzer.getTuned().contains ("accordion"));
    const auto& found = analyzer.getTuned().at ("bass");
    CHECK (found.settings.size() == 7);
    CHECK (found.tries >= 30);
    CHECK (found.distance <= found.startDistance);
    const auto& project = session.getProject();
    REQUIRE (project.channels.size() == 2);
    for (const auto& channel : project.channels)
    {
        const auto instrument = channel.instrument.value_or (model::Device {});
        INFO (channel.name);
        CHECK (instrument.kind == model::Device::Kind::plugin);
        if (instrument.plugin.name != "Spinus Test Synth")
            continue;
        // The settings found are the channel's instrument's own now.
        auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (instrument.id));
        REQUIRE (plugin != nullptr);
        REQUIRE (plugin->getAudioPluginInstance() != nullptr);
        for (const auto& [id, value] : found.settings)
        {
            auto* parameter = SynthMatcher::findParameter (*plugin->getAudioPluginInstance(), id);
            REQUIRE (parameter != nullptr);
            CHECK (std::abs (parameter->getValue() - value) < 1.0e-3f);
        }
    }
    CHECK (session.getHistory().getUndoDescriptions().size() == 1);
    session.getDeviceList().setCatalog (nullptr);
    service.setEnabled (false);
}

TEST_CASE ("Analyzer: closest presets of installed instruments (a CLAP plugin's own, programs), and none",
           "[ui][ai][analyzer]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    test::ProgramsTestPlugin::install (host.getEngine());
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    session.getDeviceList().setBundledFolders ({ bundledPlugins() });
    // Installed: the test synth (six presets through CLAP preset discovery), the organ (none) and an
    // instrument with programs (four real ones).
    plugins::PluginCatalog catalog (t.root.getChildFile ("settings"), t.root);
    {
        plugins::ClapPluginFormat format;
        juce::OwnedArray<juce::PluginDescription> found;
        format.findAllTypesForFile (
            found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName());
        REQUIRE (found.size() == 2);
        for (auto* d : found)
        {
            catalog.getKnownPlugins().addType (*d);
            host.getEngine().getPluginManager().knownPluginList.addType (*d);
        }
        catalog.getKnownPlugins().addType (test::ProgramsTestPlugin::describe());
    }
    session.getDeviceList().setCatalog (&catalog);

    const auto models = t.root.getChildFile ("models");
    ai::AiService service (t.root, models);
    service.setEnabled (true);
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
    placeModel (models, "basic_pitch", { { "nmp.onnx", 230444 } });
    placeModel (models, "clap_music",
                { { "config.json", 628 }, { "preprocessor_config.json", 541 }, { "pytorch_model.bin", 776444665 } });
    placeManyInstruments (models);

    Analyzer analyzer (session, &service);
    analyzer.setSize (660, 560);
    analyzer.setStemChoice (Analyzer::manyInstruments);
    REQUIRE (analyzer.analyze (makeSong (t.root)));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    const auto cache = analyzer.getCacheFolder();
    std::map<juce::String, juce::File> made;
    for (auto [name, frequency] :
         { std::pair { "accordion", 440.0 }, std::pair { "bass", 110.0 }, std::pair { "keys", 261.63 } })
        made[name] =
            makeStem (t.root.getChildFile ("many").getChildFile (juce::String (name) + ".wav"), frequency, false);
    placeNotes (cache, made["accordion"], { { 69, 0.1, 0.6 }, { 69, 1.1, 1.6 }, { 69, 2.1, 2.6 } });
    placeNotes (cache, made["bass"], { { 45, 0.1, 0.9 }, { 45, 1.1, 1.9 }, { 45, 2.1, 2.9 } });
    placeNotes (cache, made["keys"], { { 60, 0.1, 0.9 }, { 60, 1.1, 1.9 } });
    placeSeparation (cache, analyzer.getPreparedFile(), "mvsep_mega_53", "v1.0.21", made);
    REQUIRE (analyzer.separate (Analyzer::manyInstruments));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    REQUIRE (analyzer.getStems().size() == 3);

    // Every installed instrument offers its closest preset (whether it has presets is known once it is
    // loaded), with the same label as Surge XT's.
    using Way = Analyzer::Way;
    const auto sources = analyzer.getSoundSources (false);
    INFO (sources.joinIntoString (", "));
    for (auto* name : { "Spinus Test Synth (CLAP)", "Spinus Test Organ (CLAP)", "Spinus Test Programs (VST3)" })
    {
        INFO (name);
        REQUIRE (analyzer.setChosenSound ("bass", name, Way::closestPreset));
        CHECK (analyzer.getWays ("bass") == std::vector<Way> { Way::asIs, Way::closestPreset, Way::tuned });
        CHECK (analyzer.getWayNames ("bass") ==
               juce::StringArray { "As is (default sound)", "Closest preset", "Tuned to it (experimental)" });
        CHECK (analyzer.getChosenSounds().at ("bass") == Analyzer::Sounds::presets);
    }
    for (auto [stem, name] :
         { std::pair { "bass", "Spinus Test Synth (CLAP)" }, std::pair { "accordion", "Spinus Test Programs (VST3)" },
           std::pair { "keys", "Spinus Test Organ (CLAP)" } })
    {
        analyzer.setChosenUse (stem, Analyzer::StemUse::replicate);
        REQUIRE (analyzer.setChosenSound (stem, name, Way::closestPreset));
    }
    CHECK (analyzer.getChosenInstruments().at ("accordion").plugin.name == "Spinus Test Programs");

    // Added: the synth and the programs with their closest presets, the organ as it is (and why).
    session.getHistory().reset (model::Project());
    REQUIRE (analyzer.addChosen());
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getStatusText().contains ("3 new channels"));
    CHECK (analyzer.getStatusText().contains (
        "Spinus Test Organ has no presets Spinus can choose from; it plays its own sound."));
    const auto& closest = analyzer.getClosestPresets();
    REQUIRE (closest.contains ("bass"));
    REQUIRE (closest.contains ("accordion"));
    CHECK (! closest.contains ("keys"));
    CHECK (closest.at ("bass").size() == 5);
    CHECK (closest.at ("bass")[0].kind == InstrumentPreset::Kind::discovered);
    CHECK (closest.at ("accordion").size() == 4); // every real program
    CHECK (closest.at ("accordion")[0].kind == InstrumentPreset::Kind::program);

    // The closest one is on each channel's instrument, in its saved state.
    const std::map<juce::String, float> cutoffs { { "Bright Saw", 1.0f },    { "Dark Square", 0.3f },
                                                  { "Soft Sine Pad", 0.8f }, { "Square Pluck", 0.7f },
                                                  { "Resonant Saw", 0.45f }, { "Sine Pluck", 0.5f } };
    const auto instrumentOf = [&] (const juce::String& plugin) -> juce::AudioPluginInstance*
    {
        for (const auto& channel : session.getProject().channels)
            if (const auto device = channel.instrument.value_or (model::Device {}); device.plugin.name == plugin)
                if (auto* p =
                        dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device.id)))
                    return p->getAudioPluginInstance();
        return nullptr;
    };
    const auto cutoffOf = [] (juce::AudioPluginInstance& instance)
    {
        for (auto* parameter : instance.getParameters())
            if (parameter->getName (100) == "Cutoff")
                return parameter->getValue();
        return -1.0f;
    };
    auto* synth = instrumentOf ("Spinus Test Synth");
    auto* programs = instrumentOf ("Spinus Test Programs");
    REQUIRE (synth != nullptr);
    REQUIRE (programs != nullptr);
    REQUIRE (instrumentOf ("Spinus Test Organ") != nullptr);
    CHECK (std::abs (cutoffOf (*synth) - cutoffs.at (closest.at ("bass")[0].name)) < 1.0e-3f);
    CHECK (programs->getCurrentProgram() == closest.at ("accordion")[0].program);

    // The next closest ones, picked to hear them in place: still one undo step for the whole Add.
    REQUIRE (analyzer.useClosestPreset ("bass", 1));
    CHECK (std::abs (cutoffOf (*synth) - cutoffs.at (closest.at ("bass")[1].name)) < 1.0e-3f);
    REQUIRE (analyzer.useClosestPreset ("accordion", 2));
    CHECK (programs->getCurrentProgram() == closest.at ("accordion")[2].program);
    CHECK (! analyzer.useClosestPreset ("keys", 0));
    CHECK (session.getHistory().getUndoDescriptions().size() == 1);
    session.getHistory().undo();
    CHECK (session.getProject().channels.empty());

    // Each preset's measurements are kept per instrument: the second time, none is measured again.
    CHECK (t.root.getChildFile ("ai/cache/presets")
               .findChildFiles (juce::File::findFiles, false, "plugin-*.json")
               .size() == 2);
    // The organ has no presets: once Spinus has looked, its second list no longer offers them.
    session.getDeviceList().setCatalog (nullptr);
    session.getDeviceList().setCatalog (&catalog); // the lists are filled again
    CHECK (waitFor ([&] { return analyzer.getWays ("keys") == std::vector<Way> { Way::asIs, Way::tuned }; }));
    CHECK (analyzer.getChosenSource ("keys") == "Spinus Test Organ (CLAP)");
    CHECK (analyzer.getWays ("bass") == std::vector<Way> { Way::asIs, Way::closestPreset, Way::tuned });
    REQUIRE (analyzer.setChosenSound ("keys", "Spinus Test Synth (CLAP)", Way::closestPreset));
    session.getHistory().reset (model::Project());
    REQUIRE (analyzer.addChosen());
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
    CHECK (analyzer.getStatusText().contains ("3 new channels"));
    CHECK (analyzer.getClosestPresets().contains ("keys"));
    session.getDeviceList().setCatalog (nullptr);
    service.setEnabled (false);
}

TEST_CASE ("Analyzer: the sound lists offer every instrument once, and no effects", "[ui][analyzer]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    session.getDeviceList().setBundledFolders ({ bundledPlugins() });
    plugins::PluginCatalog catalog (t.root.getChildFile ("settings"), t.root);
    auto describe = [] (const juce::String& format, const juce::String& name, const juce::String& file, bool instrument,
                        const juce::String& maker)
    {
        juce::PluginDescription d;
        d.pluginFormatName = format;
        d.name = name;
        d.manufacturerName = maker;
        d.fileOrIdentifier = file;
        d.isInstrument = instrument;
        d.uniqueId = name.hashCode();
        return d;
    };
    auto& known = catalog.getKnownPlugins();
    known.addType (describe ("VST3", "Fake Synth", "/usr/lib/vst3/Fake Synth.vst3", true, "Fake Audio"));
    known.addType (describe ("LV2", "Fake Organ", "urn:fake:organ", true, "Fake Audio"));
    known.addType (describe ("VST", "Old Delay", "/usr/lib/vst/OldDelay.so", false, "Old"));
    // Surge XT installed by the system (or a Flatpak extension) as well as downloaded through Spinus.
    known.addType (describe ("CLAP", "Surge XT", "/home/me/.clap/Surge XT.clap", true, "Surge Synth Team"));
    session.getDeviceList().setCatalog (&catalog);

    Analyzer analyzer (session, nullptr);
    const auto sources = analyzer.getSoundSources (false);
    INFO (sources.joinIntoString (", "));
    CHECK (sources.contains ("Fake Synth (VST3)"));
    CHECK (sources.contains ("Fake Organ (LV2)"));
    CHECK (! sources.contains ("Old Delay (VST2)"));
    const auto surges = std::count_if (sources.begin(), sources.end(),
                                       [] (const juce::String& s) { return s.startsWith ("Surge XT"); });
    CHECK (surges == 1);
    // The free open-source synths are offered when they are downloaded (built here: in the build's plugins).
    CHECK (sources.contains ("Odin 2") == bundledPlugins().getChildFile ("Odin2.clap").existsAsFile());
    if (bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        CHECK (sources.contains ("Surge XT"));

    // The Browser lists the same installed instruments.
    juce::StringArray installed;
    for (auto& entry : session.getDeviceList().getEntries())
        if (entry.instrument && entry.origin == plugins::DeviceList::Origin::installed)
            installed.add (entry.name + " (" + entry.badge + ")");
    for (auto& name : installed)
        CHECK (sources.contains (name));
    session.getDeviceList().setCatalog (nullptr);
}
