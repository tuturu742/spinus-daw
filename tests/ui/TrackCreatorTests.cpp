#include <catch2/catch_test_macros.hpp>

#include "AnalysisFakes.h"
#include "FakeProvider.h"
#include "engine/EngineHost.h"
#include "session/SongPreparation.h"
#include "session/SoundDescriptors.h"
#include "ui/AnalysisLibraryView.h"
#include "ui/Analyzer.h"
#include "ui/SoundIndexView.h"
#include "ui/Theme.h"
#include "ui/TrackCreator.h"

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;
using namespace spinus::test::analysis;

/* The Track creator's first step: reference songs in, the style they share out. The AI service runs for
   real, with stand-ins for PyTorch, Demucs and Beat This! (as in the Analyzer's tests). */

namespace
{
bool allSettled (const TrackCreator& creator) { return ! creator.isBusy(); }

TrackCreator::Status statusOf (const TrackCreator& creator, int index)
{
    return creator.getReferences()[(size_t)index].status;
}

/** The AI service, with the analysis engine's stand-in once installEngine() is called and the model files
    the Track creator needs once placeModels() is. */
struct Analysis
{
    TempFolder t;
    FakeAnalysis fake;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    juce::File models = t.root.getChildFile ("models");
    ai::AiService service { t.root, models };

    bool installEngine()
    {
        service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
        return waitFor (
            [this] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); },
            60000);
    }

    void placeModels()
    {
        placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
        placeModel (models, "htdemucs", { { "htdemucs.yaml", 21 }, { "955717e8.safetensors", 84025440 } },
                    "models: ['955717e8']\n");
        placeBasicPitch (models);
    }
};

using Notes = std::initializer_list<std::tuple<int, double, double>>;

/** As if Demucs had split the song and Basic Pitch had found these notes in its bass and its other
    instruments before (Basic Pitch's model is not in the repository); the drums are clicks. */
void placeFindings (const TrackCreator& creator, const juce::File& song, Notes bass, Notes other)
{
    const auto digest = contentDigest (song);
    const auto work = creator.getWorkFolder (digest);
    const auto prepared = preparedSongFile (work, song, digest);
    std::atomic<double> progress { 0.0 };
    REQUIRE (prepareSong (song, prepared, [] { return false; }, progress).wasOk());
    const auto made = work.getChildFile ("stems made by the test"); // deleted with the work folder
    const std::map<juce::String, juce::File> stems {
        { "bass", makeStem (made.getChildFile ("bass.wav"), 110.0, false) },
        { "drums", makeStem (made.getChildFile ("drums.wav"), 0.0, true) },
        { "other", makeStem (made.getChildFile ("other.wav"), 220.0, false) },
        { "vocals", makeStem (made.getChildFile ("vocals.wav"), 440.0, false) },
    };
    placeSeparation (work, prepared, "htdemucs", "cbc8a9b1a87023b7fd74e7b3412e6321c0eab003", stems);
    placeNotes (work, stems.at ("bass"), bass);
    placeNotes (work, stems.at ("other"), other);
    placeNotes (work, stems.at ("vocals"), { { 69, 0.5, 1.0 }, { 72, 1.0, 1.5 } });
}

// Bars start at 0.1 s and last 2 s (the stand-in's beat: 120 BPM). A minor, F, A minor:
const Notes minorBass { { 45, 0.1, 1.0 }, { 45, 1.1, 2.0 }, { 41, 2.1, 3.0 },
                        { 41, 3.1, 4.0 }, { 45, 4.1, 5.0 }, { 45, 5.1, 5.9 } };
const Notes minorChords { { 57, 0.1, 2.0 }, { 60, 0.1, 2.0 }, { 64, 0.1, 2.0 }, { 53, 2.1, 4.0 }, { 57, 2.1, 4.0 },
                          { 60, 2.1, 4.0 }, { 57, 4.1, 5.9 }, { 60, 4.1, 5.9 }, { 64, 4.1, 5.9 } };

/** Another 6-second song: a C major chord with the clicks (makeSong's is A minor). */
juce::File makeMajorSong (const juce::File& folder, const juce::String& name)
{
    const auto file = folder.getChildFile (name + ".wav");
    const double rate = 44100.0;
    juce::AudioBuffer<float> audio (2, (int)(6.0 * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto t = (double)i / rate;
        float v = 0.0f;
        for (auto f : { 261.63, 329.63, 392.0 })
            v += 0.1f * (float)std::sin (juce::MathConstants<double>::twoPi * f * t);
        if (i % 22050 < 300)
            v += 0.5f;
        audio.setSample (0, i, v);
        audio.setSample (1, i, v);
    }
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (16));
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}
} // namespace

TEST_CASE ("Track creator: songs from files and an album folder, removed and cleared", "[ui][trackcreator]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    TrackCreator creator (session, nullptr);
    creator.setSize (680, 600);
    CHECK (creator.getPanelId() == "trackCreator");
    CHECK (creator.getPanelTitle() == "Track creator (experimental)");

    // An album: its songs, and those one folder down (not deeper); other files are left out.
    const auto album = t.root.getChildFile ("Album");
    REQUIRE (album.getChildFile ("CD 2/Extras").createDirectory().wasOk());
    makeSong (album, "02 Second");
    makeSong (album, "01 First");
    album.getChildFile ("cover.jpg").create();
    album.getChildFile ("notes.txt").replaceWithText ("liner notes");
    makeSong (album.getChildFile ("CD 2"), "03 Third");
    makeSong (album.getChildFile ("CD 2/Extras"), "04 Too deep");
    const auto single = makeSong (t.root, "Single");

    CHECK (creator.addReferences ({ album }) == 3);
    REQUIRE (creator.getReferences().size() == 3);
    CHECK (creator.getReferences()[0].file.getFileName() == "01 First.wav");
    CHECK (creator.getReferences()[1].file.getFileName() == "02 Second.wav");
    CHECK (creator.getReferences()[2].file.getFileName() == "03 Third.wav");
    // Added again: nothing new. A single song, and a file that is not audio.
    CHECK (creator.addReferences ({ album }) == 0);
    CHECK (creator.addReferences ({ single, album.getChildFile ("notes.txt") }) == 1);
    CHECK (creator.getReferences().size() == 4);

    // Without the AI service, the first song is read and decoded, then it says AI is needed; all wait.
    REQUIRE (waitFor ([&] { return allSettled (creator); }));
    CHECK (creator.getNeedsText().contains ("turn on AI"));
    for (int i = 0; i < 4; ++i)
        CHECK (statusOf (creator, i) == TrackCreator::Status::waiting);
    CHECK (creator.describe (creator.getReferences()[0]) == "Waiting");
    CHECK (! creator.getSummary().isObject());
    CHECK (! creator.saveSummary (t.root.getChildFile ("summary.json")));

    CHECK (creator.removeReference (1));
    CHECK (creator.getReferences().size() == 3);
    CHECK (creator.getReferences()[1].file.getFileName() == "03 Third.wav");
    CHECK (! creator.removeReference (7));
    creator.clearReferences();
    CHECK (creator.getReferences().empty());
    CHECK (! creator.isBusy());

    // Nothing here changes the project.
    CHECK (session.getHistory().getUndoDescriptions().isEmpty());
}

TEST_CASE ("Track creator: the kept analyses follow the style's version", "[ui][trackcreator]")
{
    // spinus_ai.style.FEATURES_VERSION and TrackCreator::analysisVersion change together, so an analysis
    // kept by an older Spinus is made again rather than refused by the AI service.
    const auto style = juce::File (SPINUS_TEST_FILES_DIR)
                           .getParentDirectory()
                           .getChildFile ("ai/spinus_ai/style.py")
                           .loadFileAsString();
    REQUIRE (style.contains ("FEATURES_VERSION = "));
    CHECK (style.fromFirstOccurrenceOf ("FEATURES_VERSION = ", false, false).getIntValue() ==
           TrackCreator::analysisVersion);
}

TEST_CASE ("Track creator: says what is missing, analyzes a song, keeps it, sums up the style",
           "[ui][ai][trackcreator]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    Analysis a;
    auto& t = a.t;
    auto& session = a.session;
    auto& service = a.service;
    TrackCreator creator (session, &service);
    creator.setSize (680, 600);
    const auto song = makeSong (t.root, "Song One");

    // AI off: it says so, and the song waits.
    REQUIRE (creator.addReferences ({ song }) == 1);
    REQUIRE (waitFor ([&] { return allSettled (creator); }));
    CHECK (creator.getNeedsText().contains ("turn on AI"));
    CHECK (statusOf (creator, 0) == TrackCreator::Status::waiting);

    // AI on, no engine: it offers to set it up.
    service.setEnabled (true);
    creator.analyzeAgain();
    REQUIRE (waitFor ([&] { return allSettled (creator); }));
    CHECK (creator.getNeedsText().contains ("analysis engine"));

    // The engine (the stand-in) installed: the model files are missing, with their size.
    REQUIRE (a.installEngine());
    creator.analyzeAgain();
    REQUIRE (waitFor ([&] { return allSettled (creator); }));
    CHECK (creator.getNeedsText().contains ("MB"));
    CHECK (creator.getDownloadSize() == 84025440 + 21 + 8451101 + 230444); // Demucs, Beat This!, Basic Pitch
    CHECK (statusOf (creator, 0) == TrackCreator::Status::waiting);

    // With the model files there, and what Demucs and Basic Pitch found as if from before.
    a.placeModels();
    placeFindings (creator, song, minorBass, minorChords);
    const auto digest = contentDigest (song);
    const auto work = creator.getWorkFolder (digest);

    creator.analyzeAgain();
    CHECK (creator.isBusy());
    // The drum recogniser learns Spinus's drum sounds the first time: allow for it.
    REQUIRE (waitFor ([&] { return allSettled (creator); }, 240000));
    INFO (creator.getStatusText());
    INFO (creator.describe (creator.getReferences()[0]));
    REQUIRE (statusOf (creator, 0) == TrackCreator::Status::done);
    CHECK (creator.describe (creator.getReferences()[0]) == "Done");
    CHECK (creator.getNeedsText().isEmpty());

    const auto& features = creator.getReferences()[0].features;
    CHECK (juce::approximatelyEqual ((double)features["tempo"], 120.0));
    CHECK (features["key"]["name"].toString() == "A minor");
    REQUIRE (features["chords"].size() >= 3);
    CHECK (features["chords"][0]["roman"].toString() == "i");
    CHECK (features["chords"][1]["roman"].toString() == "bVI");
    CHECK (! (bool)features["parts"]["bass"]["present"]); // 6 notes: too few to count as a part

    // The summary, in plain words, and saved as JSON.
    const auto& summary = creator.getSummary();
    REQUIRE (summary.isObject());
    CHECK (summary["kind"].toString() == "spinus.style-summary");
    CHECK ((int)summary["songs"] == 1);
    const auto text = creator.getSummaryText();
    CHECK (text.contains ("1 song: Song One."));
    CHECK (text.contains ("Tempo: 120 BPM"));
    CHECK (text.contains ("A minor"));
    CHECK (text.contains ("Loudness: about"));
    const auto saved = t.root.getChildFile ("Style summary.json");
    REQUIRE (creator.saveSummary (saved));
    CHECK (juce::JSON::parse (saved)["kind"].toString() == "spinus.style-summary");
    CHECK (juce::JSON::parse (saved)["plain"].size() == summary["plain"].size());

    // Everything found is in the analysis library (the settings folder), by the song's contents: the stems as
    // FLAC too. The large files are gone.
    const auto library = creator.getLibrary();
    CHECK (library.getFolder().isAChildOf (t.root));
    const auto entry = library.read (digest);
    CHECK ((int)entry["features"]["version"] == TrackCreator::analysisVersion);
    CHECK (entry["rhythm"].isObject());
    CHECK (entry["key"]["name"].toString() == "A minor");
    CHECK (entry["loudness"].isObject());
    CHECK (entry["notes"]["htdemucs/bass"].isArray());
    CHECK (entry["drums"]["htdemucs"].isArray());
    CHECK (library.findStems (digest, "htdemucs").size() == 4);
    CHECK (! work.exists());

    // Removed and added again (a copy under another name): ready at once, from what was kept.
    REQUIRE (creator.removeReference (0));
    REQUIRE (waitFor ([&] { return allSettled (creator); }));
    CHECK (! creator.getSummary().isObject());
    const auto copy = t.root.getChildFile ("Copy of Song One.wav");
    REQUIRE (song.copyFileTo (copy));
    REQUIRE (creator.addReferences ({ copy }) == 1);
    REQUIRE (waitFor ([&] { return allSettled (creator); }, 20000));
    REQUIRE (statusOf (creator, 0) == TrackCreator::Status::done);
    CHECK (creator.getReferences()[0].keptBefore);
    CHECK (creator.describe (creator.getReferences()[0]) == "Done (analyzed before)");
    CHECK (! work.exists());                                                   // not decoded again
    CHECK (creator.getSummary()["names"][0].toString() == "Copy of Song One"); // named as it is now

    // A song that cannot be read: a problem on its row, the others carry on.
    const auto broken = t.root.getChildFile ("Broken.wav");
    broken.replaceWithText ("not audio");
    REQUIRE (creator.addReferences ({ broken }) == 1);
    REQUIRE (waitFor ([&] { return allSettled (creator); }, 20000));
    CHECK (statusOf (creator, 1) == TrackCreator::Status::failed);
    CHECK (creator.describe (creator.getReferences()[1]).startsWith ("Problem: Spinus cannot read Broken.wav"));
    CHECK (statusOf (creator, 0) == TrackCreator::Status::done);

    // Nothing here changes the project.
    CHECK (session.getHistory().getUndoDescriptions().isEmpty());
}

TEST_CASE ("Track creator screenshot for the manual", "[.screenshot][manual]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    a.service.setEnabled (true);
    REQUIRE (a.installEngine());
    a.placeModels();
    SpinusLookAndFeel look (Theme::dark());
    TrackCreator creator (a.session, &a.service);
    creator.setLookAndFeel (&look);
    creator.setSize (680, 680);
    // A sound index as a computer with Surge XT and a few sample packs might have.
    const auto sounds = a.t.root.getChildFile ("sounds");
    sounds.createDirectory();
    sounds.getChildFile ("index.json")
        .replaceWithText (R"({"kind": "spinus.sound-index", "version": )" + juce::String (SoundIndex::version) +
                          R"(, "descriptorsVersion": )" + juce::String (sounds::descriptorsVersion) +
                          R"(, "updated": "2026-10-26T14:02:00", "summary": {"instruments": 6, "presets": 3204,
                              "samples": 418, "problems": 0}})");
    creator.setSoundIndexFolder (sounds);
    const auto album = a.t.root.getChildFile ("Album");
    REQUIRE (album.createDirectory().wasOk());
    const auto first = makeSong (album, "01 Morning Light");
    const auto second = makeMajorSong (album, "02 Night Drive");
    placeFindings (creator, first, minorBass, minorChords);
    placeFindings (creator, second,
                   { { 36, 0.1, 1.0 }, { 36, 1.1, 2.0 }, { 43, 2.1, 3.0 }, { 43, 3.1, 4.0 }, { 36, 4.1, 5.0 } },
                   { { 60, 0.1, 2.0 },
                     { 64, 0.1, 2.0 },
                     { 67, 0.1, 2.0 },
                     { 55, 2.1, 4.0 },
                     { 59, 2.1, 4.0 },
                     { 62, 2.1, 4.0 },
                     { 60, 4.1, 5.9 },
                     { 64, 4.1, 5.9 },
                     { 67, 4.1, 5.9 } });
    REQUIRE (creator.addReferences ({ album }) == 2);
    REQUIRE (waitFor ([&] { return ! creator.isBusy(); }, 240000));
    REQUIRE (creator.getSummary().isObject());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/track-creator.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (
        juce::PNGImageFormat().writeImageToStream (creator.createComponentSnapshot (creator.getLocalBounds()), stream));
    creator.setLookAndFeel (nullptr);
}

TEST_CASE ("Track creator: the songs and the summary are remembered, without starting the AI service",
           "[ui][ai][trackcreator]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    a.service.setEnabled (true);
    REQUIRE (a.installEngine());
    a.placeModels();
    const auto song = makeSong (a.t.root, "Song One");
    const auto waiting = makeMajorSong (a.t.root, "Not Yet");
    juce::String summaryText;
    {
        TrackCreator creator (a.session, &a.service);
        placeFindings (creator, song, minorBass, minorChords);
        REQUIRE (creator.addReferences ({ song }) == 1);
        REQUIRE (waitFor ([&] { return allSettled (creator); }, 240000));
        REQUIRE (statusOf (creator, 0) == TrackCreator::Status::done);
        summaryText = creator.getSummaryText();
        REQUIRE (summaryText.contains ("Song One"));
        // A second song, stopped before it is analyzed.
        REQUIRE (creator.addReferences ({ waiting }) == 1);
        creator.cancel();
        CHECK (creator.getListFile().existsAsFile());
    }

    // The next start: a new AI service (not running), the same settings folder.
    ai::AiService next (a.t.root, a.models);
    TrackCreator creator (a.session, nullptr);
    CHECK (creator.getReferences().empty());
    creator.setService (&next);
    REQUIRE (creator.getReferences().size() == 2);
    INFO (creator.getListFile().loadFileAsString());
    CHECK (statusOf (creator, 0) == TrackCreator::Status::done);
    CHECK (creator.getReferences()[0].keptBefore);
    CHECK (statusOf (creator, 1) != TrackCreator::Status::done);
    CHECK (creator.getSummaryText() == summaryText);
    CHECK (! creator.isBusy());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    CHECK (next.getState() != ai::AiService::State::running);
    CHECK (next.getState() != ai::AiService::State::starting);
    CHECK (creator.getMethodsRun().isEmpty());
}

TEST_CASE ("Track creator and Analyzer share the analysis library: nothing is found twice", "[ui][ai][trackcreator]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    a.service.setEnabled (true);
    REQUIRE (a.installEngine());
    a.placeModels();
    const auto song = makeSong (a.t.root, "Shared Song");
    const auto digest = contentDigest (song);

    // The Analyzer finds the tempo and key and separates (the stand-in Demucs); the stems go into the
    // library as FLAC in the background.
    Analyzer analyzer (a.session, &a.service);
    analyzer.setSize (660, 560);
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    REQUIRE (analyzer.getRhythm().has_value());
    CHECK (! analyzer.hasEarlierAnalysis()); // the first time
    REQUIRE (analyzer.separate (4));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    REQUIRE (analyzer.getStems().size() == 4);
    const auto library = analyzer.getLibrary();
    REQUIRE (waitFor ([&] { return library.findStems (digest, "htdemucs").size() == 4; }, 30000));
    CHECK (library.read (digest)["rhythm"].isObject());
    CHECK (library.read (digest)["key"].isObject());

    // The same song dropped again: continue from the earlier analysis, nothing separated again.
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    REQUIRE (analyzer.hasEarlierAnalysis());
    CHECK (analyzer.getStatusText().contains ("analyzed this song before (tempo, key, 4 stems)"));
    CHECK (analyzer.getStems().empty());
    REQUIRE (analyzer.continueFromEarlier());
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getStatusText().startsWith ("Continued from the earlier analysis"));
    CHECK (analyzer.getTempoText() == "120.0 BPM, 4 beats per bar");
    CHECK (analyzer.getKeyText().startsWith ("A minor"));
    REQUIRE (analyzer.getStems().size() == 4);
    CHECK (analyzer.getStems().at ("bass").getParentDirectory().getParentDirectory().getFileName() == "kept-stems");
    CHECK (analyzer.getStems().at ("bass").hasFileExtension ("wav"));

    // The Track creator takes all that from the library: only the loudness, the drums and the style are new.
    // (The notes, as if the Analyzer had found them with Basic Pitch, which is not in the repository.)
    auto lib = library;
    lib.setIn (digest, {}, "notes", "htdemucs/bass",
               juce::JSON::parse (R"([{"start": 0.1, "end": 1.9, "pitch": 45}])"));
    lib.setIn (digest, {}, "notes", "htdemucs/other",
               juce::JSON::parse (R"([{"start": 0.1, "end": 1.9, "pitch": 57}, {"start": 0.1, "end": 1.9, "pitch": 60},
                                     {"start": 0.1, "end": 1.9, "pitch": 64}])"));
    lib.setIn (digest, {}, "notes", "htdemucs/vocals", juce::JSON::parse ("[]"));
    TrackCreator creator (a.session, &a.service);
    REQUIRE (creator.addReferences ({ song }) == 1);
    REQUIRE (waitFor ([&] { return allSettled (creator); }, 240000));
    INFO (creator.describe (creator.getReferences()[0]));
    REQUIRE (statusOf (creator, 0) == TrackCreator::Status::done);
    const auto& run = creator.getMethodsRun();
    INFO (run.joinIntoString (", "));
    CHECK (! run.contains ("ai.v1.analyze.tempo"));
    CHECK (! run.contains ("ai.v1.analyze.key"));
    CHECK (! run.contains ("ai.v1.stems.separate"));
    CHECK (! run.contains ("ai.v1.transcribe.notes"));
    CHECK (run.contains ("ai.v1.style.summarize"));
    CHECK (creator.getReferences()[0].features["chords"][0]["roman"].toString() == "i");

    // Analyze anew: everything is found again, the notes of the old stems forgotten.
    REQUIRE (analyzer.analyze (song));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    REQUIRE (analyzer.hasEarlierAnalysis());
    REQUIRE (analyzer.analyzeAnew());
    CHECK (! library.read (digest)["notes"].isObject());
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    CHECK (analyzer.getStatusText().startsWith ("Done."));
    CHECK (analyzer.getStems().empty());
    CHECK (library.read (digest)["features"].isObject()); // the style stays
}

TEST_CASE ("Track creator on real songs (a folder named by SPINUS_REALMUSIC_BATCH)", "[.realmusic]")
{
    // A check by hand, not run in CI: real songs through the real analysis engine and models, installed the
    // way Spinus installs them into the settings and models folders of the HOME it runs with (use a scratch
    // HOME). Writes <batch>.txt (per song: time, tempo, key; the summary in words) and <batch>.json (the
    // summary) into SPINUS_REALMUSIC_OUT. The songs are only read.
    const juce::File batch (juce::SystemStats::getEnvironmentVariable ("SPINUS_REALMUSIC_BATCH", {}));
    const juce::File out (juce::SystemStats::getEnvironmentVariable ("SPINUS_REALMUSIC_OUT", {}));
    const juce::File settings (juce::SystemStats::getEnvironmentVariable ("SPINUS_REALMUSIC_SETTINGS", {}));
    if (! batch.isDirectory() || out == juce::File() || settings == juce::File())
        SKIP ("set SPINUS_REALMUSIC_BATCH, SPINUS_REALMUSIC_OUT and SPINUS_REALMUSIC_SETTINGS");
    REQUIRE (out.createDirectory().wasOk());

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ai::AiService service (settings);
    service.setEnabled (true);
    const auto python = ai::AiService::getDefaultModelsFolder().getSiblingFile ("python");
    if (! python.getChildFile ("cpu/spinus-build.json").existsAsFile() &&
        python.findChildFiles (juce::File::findFiles, true, "spinus-build.json").isEmpty())
    {
        service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
        REQUIRE (waitFor ([&] { return python.getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 3600000));
    }

    TrackCreator creator (session, &service);
    creator.clearReferences(); // only this batch
    const auto started = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (creator.addReferences ({ batch }) > 0);
    // Each song's time: from when it starts to when it is done. The model files are downloaded when asked
    // for (the first time), then the songs carry on.
    std::map<juce::String, double> begun, took;
    bool downloaded = false;
    REQUIRE (waitFor (
        [&]
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            for (const auto& r : creator.getReferences())
            {
                const auto name = r.file.getFileName();
                if (r.status == TrackCreator::Status::preparing || r.status == TrackCreator::Status::analyzing)
                    begun.emplace (name, now);
                if ((r.status == TrackCreator::Status::done || r.status == TrackCreator::Status::failed) &&
                    ! took.contains (name))
                    took[name] = (now - (begun.contains (name) ? begun[name] : started)) / 1000.0;
            }
            if (! creator.isBusy() && ! downloaded && creator.getDownloadSize() > 0 &&
                creator.getNeedsText().isNotEmpty())
            {
                downloaded = true;
                creator.downloadModels(); // it carries on with the songs once they are there
                return false;
            }
            return ! creator.isBusy();
        },
        6 * 3600000));

    juce::StringArray lines;
    lines.add ("Batch: " + batch.getFileName() + " (" + juce::String (creator.getReferences().size()) + " songs, " +
               juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 0) + " s in all)");
    for (const auto& r : creator.getReferences())
    {
        const auto& f = r.features;
        lines.add (r.file.getFileName() + " | " + creator.describe (r) + " | " +
                   juce::String (took.contains (r.file.getFileName()) ? took[r.file.getFileName()] : 0.0, 0) +
                   " s | tempo " + juce::String ((double)f["tempo"], 1) +
                   (f["tempoDoubt"].isString() ? " (" + f["tempoDoubt"].toString() + "?)" : juce::String()) +
                   " | key " + f["key"]["name"].toString() + " (" + f["key"]["confidence"].toString() + ", found " +
                   f["keyFound"].toString() + ") | bars " + f["bars"].toString() + " | sections " +
                   juce::String (f["sections"].size()) + " | LUFS " + juce::String ((double)f["loudness"]["lufs"], 1) +
                   " | swing " + f["groove"]["swing"].toString() + " | drums from " + f["groove"]["from"].toString());
    }
    lines.add ({});
    lines.add (creator.getSummaryText());
    out.getChildFile (batch.getFileName() + ".txt").replaceWithText (lines.joinIntoString ("\n") + "\n");
    creator.saveSummary (out.getChildFile (batch.getFileName() + ".json"));
    CHECK (creator.getSummary().isObject());
}

//==============================================================================
#if defined(SPINUS_PLUGIN_SCANNER) && defined(SPINUS_BROKEN_PLUGINS_DIR)
TEST_CASE ("Track creator: the sound index is updated in the background, listed and deleted, without AI",
           "[ui][trackcreator][sounds]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    TrackCreator creator (session, nullptr); // no AI service: the sound index does not need it
    const auto folder = t.root.getChildFile ("sounds");
    creator.setSoundIndexFolder (folder);
    SoundIndexer::Options options;
    options.helper = juce::File (SPINUS_PLUGIN_SCANNER);
    creator.setSoundIndexOptions (options);
    SoundSource synth;
    synth.key = "plugin-test-synth";
    synth.name = synth.pluginName = "Spinus Test Synth";
    synth.format = "CLAP";
    synth.file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName();
    synth.fingerprint = "1";
    creator.getSoundSources = [synth] { return std::vector<SoundSource> { synth }; };
    const auto samples = t.root.getChildFile ("Samples");
    {
        // A kick: a falling sine, half a second.
        samples.createDirectory();
        juce::AudioBuffer<float> kick (1, 24000);
        double phase = 0.0;
        for (int i = 0; i < kick.getNumSamples(); ++i)
        {
            const auto time = i / 48000.0;
            phase += (50.0 + 100.0 * std::exp (-time / 0.03)) / 48000.0;
            kick.setSample (0, i, (float)(0.9 * std::sin (2.0 * M_PI * phase) * std::exp (-time / 0.12)));
        }
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::FileOutputStream> (samples.getChildFile ("one.wav"));
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (kick, 0, kick.getNumSamples());
    }
    creator.getSampleFolders = [samples] { return juce::Array<juce::File> { samples }; };

    CHECK (creator.getSoundIndexText().contains ("No sound index yet"));
    creator.updateSoundIndex();
    CHECK (creator.isIndexingSounds());
    REQUIRE (waitFor ([&] { return ! creator.isIndexingSounds(); }, 120000));
    REQUIRE (creator.getLastSoundIndexResult());
    CHECK (creator.getLastSoundIndexResult().value().soundsMeasured == 6);
    CHECK (creator.getLastSoundIndexResult().value().samplesMeasured == 1);
    CHECK (creator.getSoundIndexText().contains ("1 instrument, 6 presets, 1 sample"));

    // The list, with search.
    SoundIndexView view (folder);
    CHECK (view.getShown().size() == 7);
    view.search ("pad");
    REQUIRE (view.getShown().size() == 1);
    CHECK (view.getShown()[0].name == "Soft Sine Pad");
    view.search ("kick sample");
    REQUIRE (view.getShown().size() == 1);
    CHECK (view.getShown()[0].name == "one.wav");
    CHECK (SoundIndexView::describe (view.getShown()[0]).contains ("kick"));

    // Its space in Saved analyses, and deleting it.
    AnalysisLibraryView saved (AnalysisLibrary (t.root.getChildFile ("library")), folder);
    CHECK (saved.getSoundIndexText().contains ("KB"));
    REQUIRE (saved.deleteSoundIndex());
    CHECK_FALSE (folder.exists());
    CHECK (saved.getSoundIndexText().contains ("none yet"));
}
#endif
