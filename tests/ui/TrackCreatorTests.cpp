#include <catch2/catch_test_macros.hpp>

#include "AnalysisFakes.h"
#include "ai/AnalysisJobs.h"
#include "ai/Proposal.h"
#include "FakeProvider.h"
#include "engine/EngineHost.h"
#include "session/PartSounds.h"
#include "session/UserRoles.h"
#include "ui/PartSoundsPanel.h"
#include "session/SongPreparation.h"
#include "session/SoundDescriptors.h"
#include "ui/AnalysisLibraryView.h"
#include "ui/Analyzer.h"
#include "ui/Automixer.h"
#include "ui/SongWriterPanel.h"
#include "ui/SoundIndexView.h"
#include "ui/Theme.h"
#include "session/SongWriter.h"
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

    // One path: the songs' style is written into the description (step 2) and read; the parts and the song work
    // from it. Changing the words changes the song; the rest of the songs' style stays.
    auto& describer = creator.getDescriber();
    REQUIRE (waitFor (
        [&]
        {
            return describer.getText().startsWith ("Like your songs:") && ! describer.isBusy() &&
                   creator.isFromDescription();
        },
        20000));
    CHECK (describer.hasReferences());
    CHECK (describer.getUnderstoodText().startsWith ("Understood: like your songs"));
    CHECK ((bool)creator.getActiveSummary()["described"]["references"]);
    describer.setText (describer.getText() + " Make it 150 BPM.");
    REQUIRE (describer.read());
    REQUIRE (waitFor ([&] { return ! describer.isBusy(); }, 20000));
    CHECK (describer.getUnderstoodText().contains ("changed: 150 BPM"));
    CHECK ((int)creator.getActiveSummary()["tempo"]["median"] == 150);
    CHECK (creator.getActiveSummary()["perSong"].size() == creator.getSummary()["perSong"].size());

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
    creator.setSize (720, 720);
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

TEST_CASE ("Track creator: a song described in words is read, written from and remembered", "[ui][ai][trackcreator]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    const auto text = juce::String ("A slow, dark depressive black metal song in D minor at 72 BPM, with a piano");
    {
        TrackCreator creator (a.session, &a.service);
        creator.setSize (720, 720);
        creator.showPage (TrackCreator::Page::describe);
        auto& describer = creator.getDescriber();
        CHECK (describer.isVisible());
        describer.setText (text);
        // AI off: the page says what to do, and nothing starts.
        CHECK_FALSE (describer.read());
        CHECK (describer.getStatusText().contains ("Options > AI"));

        a.service.setEnabled (true);
        REQUIRE (describer.read());
        REQUIRE (waitFor ([&] { return ! describer.isBusy(); }, 120000));
        INFO (describer.getStatusText());
        CHECK (describer.getUnderstoodText() ==
               "Understood: 72 BPM, D minor, slow, dark; genre: depressive black metal; sounds: piano.");
        CHECK (describer.getDetailsText().contains ("As depressive black metal usually is"));
        REQUIRE (creator.isFromDescription());
        CHECK (creator.getActiveSummary()["described"]["genre"].toString() == "dsbm");

        // The song is written from it, in a full arrangement.
        auto& writer = creator.getSongWriter();
        writer.setSeed (1);
        REQUIRE (creator.getPartSounds().suggestParts ("full"));
        creator.showPage (TrackCreator::Page::song);
        REQUIRE (writer.write());
        REQUIRE (waitFor ([&] { return ! writer.isBusy(); }, 120000));
        INFO (writer.getStatusText());
        CHECK (writer.getText().startsWith ("From your description. Understood: 72 BPM"));
        // A description's style, not songs: said as such; parts without a chosen sound named as Spinus's own.
        CHECK (writer.getText().contains ("Compared with the style of your description:"));
        CHECK_FALSE (writer.getText().contains ("Compared with the songs"));
        CHECK (writer.getText().contains ("Spinus's own instruments for: Drums"));
        CHECK ((int)writer.getSong()["tempo"] == 72);
        CHECK (song::isFull (writer.getSong()));
        const auto channels = a.session.getProject().channels.size();
        REQUIRE (writer.apply());
        CHECK (a.session.getProject().channels.size() > channels + 8);
        CHECK (a.session.getHistory().getUndoDescriptions().size() == 1);
    }

    // The next start: the description and what was understood come back, and the pages still work from it.
    ai::AiService next (a.t.root, a.models);
    TrackCreator creator (a.session, nullptr);
    creator.setService (&next);
    CHECK (creator.isFromDescription());
    CHECK (creator.getDescriber().getText() == text);
    CHECK (creator.getDescriber().getUnderstoodText().startsWith ("Understood: 72 BPM, D minor"));
    CHECK ((int)creator.getActiveSummary()["tempo"]["median"] == 72);
    CHECK (next.getState() != ai::AiService::State::running);
}

TEST_CASE ("Track creator: the parts are the user's list: none at first, what the description names, suggested, "
           "any instrument",
           "[ui][trackcreator]")
{
    Analysis a;
    PartSoundsPanel::Context context;
    context.summary = juce::JSON::parse (R"({"kind": "spinus.style-summary", "tempo": {"median": 108},
        "parts": {"bass": {"songs": 3, "low": 28, "median": 38, "high": 50, "notesPerBar": 8},
                  "other": {"songs": 3, "low": 52, "median": 62, "high": 76, "notesPerBar": 6, "voices": 3},
                  "vocals": {"songs": 3, "low": 55, "median": 64, "high": 74, "notesPerBar": 3},
                  "drums": {"songs": 3, "notesPerBar": 13}}})");
    SoundSource heavy, mystery;
    heavy.key = "heavy-guitar";
    heavy.name = "Heavy Guitar";
    heavy.format = "VST3";
    heavy.file = "C:/Program Files/Common Files/VST3/Heavy Guitar.vst3";
    mystery.key = "mystery-synth";
    mystery.name = "Mystery Synth";
    mystery.format = "CLAP";
    context.sources = { heavy, mystery };
    context.indexFolder = a.t.root.getChildFile ("sounds");
    PartSoundsPanel panel (a.session, [&] { return context; });
    panel.setSize (700, 600);
    auto roles = [&]
    {
        juce::StringArray r;
        for (const auto& p : panel.getParts())
            r.add (p.role);
        return r.joinIntoString (" ");
    };
    // Songs' summary: no parts until the user adds or asks for some.
    panel.resetParts();
    CHECK (panel.getParts().empty());
    CHECK_FALSE (panel.chooseSounds());
    CHECK (panel.getStatusText().contains ("Suggest parts"));

    // A description names its parts.
    auto described = context.summary;
    described.getDynamicObject()->setProperty ("described", juce::JSON::parse (R"({"genreName": "rock",
        "parts": [{"role": "rhythm-guitar", "name": "Guitar"}, {"role": "bass", "name": "Bass"},
                  {"role": "drums", "name": "Drums"}],
        "suggested": ["drums", "bass", "rhythm-guitar", "rhythm-guitar", "lead-guitar"]})"));
    context.summary = described;
    panel.resetParts ("A new description was read.");
    CHECK (roles() == "rhythm-guitar bass drums");
    CHECK (panel.getParts()[0].name == "Guitar");
    CHECK (panel.getStatusText().startsWith ("A new description was read."));

    // Suggest parts: the genre's, or a sketch or a full arrangement.
    REQUIRE (panel.suggestParts ("genre"));
    CHECK (roles() == "drums bass rhythm-guitar rhythm-guitar lead-guitar");
    CHECK (panel.getParts()[3].name == "Rhythm guitar 2");
    REQUIRE (panel.suggestParts ("sketch"));
    CHECK (roles() == "drums bass chords lead");
    REQUIRE (panel.suggestParts ("full"));
    CHECK (roles() == "drums bass chords pad arp lead counter riser impact");
    REQUIRE (panel.addPart ("piano"));
    REQUIRE (panel.setRole (3, "strings"));
    REQUIRE (panel.removePart (8)); // the impacts
    CHECK (panel.getParts().size() == 9);
    CHECK_FALSE (panel.addPart ("kazoo"));

    // Any instrument, measured or not: a part from it takes the role its name says, and plays it.
    parts::Choice guitar;
    guitar.kind = parts::Choice::Kind::preset;
    guitar.source = heavy;
    guitar.instrument = heavy.key;
    guitar.name = "Default sound";
    guitar.why = { "chosen by you", "not measured" };
    REQUIRE (panel.addPartFrom (guitar));
    CHECK (panel.getParts().back().role == "rhythm-guitar");
    const auto& row = panel.getRows().back();
    REQUIRE (row.chosen() != nullptr);
    CHECK (row.chosen()->source.name == "Heavy Guitar");
    CHECK (row.picked.has_value());
    // Picked for another part, it says what it is.
    REQUIRE (panel.pickSound (0, guitar));
    CHECK (panel.getRows()[0].note.contains ("rhythm guitar"));
    CHECK (panel.makePlan().parts.front().choice.source.name == "Heavy Guitar"); // it wins

    // A name that says nothing: the user marks the instrument, and that is its role from then on (kept beside
    // the sound index, not in it).
    auto synth = guitar;
    synth.source = mystery;
    synth.instrument = mystery.key;
    REQUIRE (panel.markInstrument (mystery.key, "acoustic-guitar"));
    CHECK (UserRoles::fileIn (context.indexFolder).existsAsFile());
    REQUIRE (panel.addPartFrom (synth));
    CHECK (panel.getParts().back().role == "acoustic-guitar");
    UserRoles kept (context.indexFolder);
    kept.load();
    CHECK (kept.get (mystery.key) == "acoustic-guitar");
    CHECK (kept.get (mystery.key, "any preset") == "acoustic-guitar");
    REQUIRE (panel.markInstrument (mystery.key, {}));
    kept.load();
    CHECK (kept.get (mystery.key).isEmpty());

    // An effect part may play any sound: no "change the role" for impacts on a drum instrument.
    REQUIRE (panel.suggestParts ("full"));
    SoundSource drumMachine;
    drumMachine.key = "stock-spinus.drum-machine";
    drumMachine.name = "Drum Machine";
    drumMachine.stockType = "spinus.drum-machine";
    parts::Choice kitChoice;
    kitChoice.source = drumMachine;
    kitChoice.instrument = drumMachine.key;
    kitChoice.name = "Default sound";
    const auto impacts = (int)panel.getParts().size() - 1;
    REQUIRE (panel.getParts()[(size_t)impacts].role == "impact");
    REQUIRE (panel.pickSound (impacts, kitChoice));
    CHECK_FALSE (panel.getRows()[(size_t)impacts].note.contains ("change the role"));

    // An organ preset filed under keys, for an Organ part: no warning either (nor an electric piano for it).
    REQUIRE (panel.addPart ("organ"));
    parts::Choice organ;
    organ.source.key = organ.instrument = "a-synth";
    organ.source.name = "A Synth";
    organ.name = "House Organ";
    organ.category = "Keys";
    CHECK (parts::roleFromName ("A Synth House Organ Keys") == "organ"); // its name wins over its category
    REQUIRE (panel.pickSound ((int)panel.getParts().size() - 1, organ));
    CHECK_FALSE (panel.getRows().back().note.contains ("change the role"));
    // No reference songs: their part's button (The songs') is not shown.
    CHECK_FALSE (panel.hasReferences());

    CHECK (parts::roleFromName ("Fingered Bass") == "bass");
    CHECK (parts::roleFromName ("Electric Guitar") == "rhythm-guitar");
    CHECK (parts::roleFromName ("Grand Piano") == "piano");
    CHECK (parts::roleFromName ("Kick Drum Kit") == "drums");
    CHECK (parts::roleFromName ("Nylon Guitar") == "acoustic-guitar");
}

TEST_CASE ("Track creator: a sound the user marked is chosen first for its role, and the mark survives a new scan",
           "[ui][trackcreator]")
{
    TempFolder t;
    const auto folder = t.root.getChildFile ("sounds");
    SoundIndex index (folder);
    SoundIndex::Instrument synth;
    synth.source.key = "synth-x";
    synth.source.name = "Synth X";
    synth.source.format = "CLAP";
    synth.complete = true;
    for (const auto* name : { "Bright Lead", "Plain Patch" })
        synth.sounds.add (juce::JSON::parse ("{\"id\": \"" + juce::String (name) + "\", \"name\": \"" + name +
                                             "\", \"role\": \"lead\", \"low\": 36, \"high\": 96, "
                                             "\"tracksPitch\": true, \"polyphonic\": true}"));
    index.instruments[synth.source.key] = synth;
    REQUIRE (index.saveInstrument (synth));
    REQUIRE (index.saveIndex());

    parts::SongPart guitar { "guitar", "Guitar", "rhythm-guitar" };
    const auto target = parts::targetFor (guitar, 0, {});
    UserRoles roles (folder);
    roles.set ("synth-x", "Plain Patch", "rhythm-guitar");
    REQUIRE (roles.save());
    // A new scan writes the index again: the mark stays.
    REQUIRE (index.saveInstrument (synth));
    REQUIRE (index.saveIndex());
    SoundIndex again (folder);
    again.load();
    const auto found = parts::choose (target, {}, again, 5);
    REQUIRE_FALSE (found.empty());
    CHECK (found.front().name == "Plain Patch");
    CHECK (found.front().reasons().contains ("you marked it as rhythm guitar"));
    // Marked as something else, it comes after.
    roles.set ("synth-x", "Plain Patch", "strings");
    REQUIRE (roles.save());
    CHECK (parts::choose (target, {}, again, 5).front().name != "Plain Patch");
}

TEST_CASE ("Track creator describe a song screenshot for the manual", "[.screenshot][manual]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    a.service.setEnabled (true);
    SpinusLookAndFeel look (Theme::dark());
    TrackCreator creator (a.session, &a.service);
    creator.setLookAndFeel (&look);
    creator.setSize (720, 720);
    creator.showPage (TrackCreator::Page::describe);
    auto& describer = creator.getDescriber();
    describer.setText ("A sunny, upbeat indie pop song at 118 BPM in G major, with acoustic guitar, handclaps and a "
                       "catchy whistled lead");
    REQUIRE (describer.read());
    REQUIRE (waitFor ([&] { return ! describer.isBusy(); }, 120000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/track-creator-describe.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (
        juce::PNGImageFormat().writeImageToStream (creator.createComponentSnapshot (creator.getLocalBounds()), stream));
    creator.setLookAndFeel (nullptr);
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
    Session session (host.getEngine(), makeDevices (host.getEngine())); // Spinus's own and the built Surge XT
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

namespace
{
SoundSource partTestSynth()
{
    SoundSource synth;
    synth.key = "plugin-test-synth";
    synth.name = synth.pluginName = "Spinus Test Synth";
    synth.format = "CLAP";
    synth.badge = "CLAP";
    synth.file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName();
    synth.fingerprint = "1";
    return synth;
}

void writeMono (const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (48000).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

juce::AudioBuffer<float> drumSound (const juce::String& kind)
{
    juce::AudioBuffer<float> b (1, kind == "kick" ? 20000 : 9000);
    juce::Random random (kind.hashCode());
    double phase = 0.0;
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const auto t = i / 48000.0;
        float v = 0.0f;
        if (kind == "kick")
        {
            phase += (50.0 + 100.0 * std::exp (-t / 0.03)) / 48000.0;
            v = (float)(0.9 * std::sin (2.0 * M_PI * phase) * std::exp (-t / 0.12));
        }
        else
            v = (random.nextFloat() * 2.0f - 1.0f) * (float)std::exp (-t / (kind == "snare" ? 0.06 : 0.015)) *
                (kind == "snare" ? 0.7f : 0.4f);
        b.setSample (0, i, v);
    }
    return b;
}

juce::var notesJson (const std::vector<std::tuple<int, double, double>>& notes)
{
    juce::Array<juce::var> list;
    for (const auto& [pitch, start, end] : notes)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("pitch", pitch);
        o->setProperty ("start", start);
        o->setProperty ("end", end);
        o->setProperty ("velocity", 0.8);
        list.add (juce::var (o));
    }
    return list;
}
} // namespace

namespace
{
/** A sound index (the test synth's presets, drum samples) and a reference song in the analysis library whose
    bass the test synth's Dark Square plays, with its notes and drum hits; the context the panel gets. */
PartSoundsPanel::Context makePartSoundsContext (Analysis& a)
{
    SoundIndexer::Options helper;
    helper.helper = juce::File (SPINUS_PLUGIN_SCANNER);
    const auto synth = partTestSynth();

    // The sound index: the test synth's presets and a kick, a snare and two hats.
    const auto samples = a.t.root.getChildFile ("Samples");
    writeMono (samples.getChildFile ("Kicks/Kick 1.wav"), drumSound ("kick"));
    writeMono (samples.getChildFile ("Snares/Snare 1.wav"), drumSound ("snare"));
    writeMono (samples.getChildFile ("Hats/Hat 1.wav"), drumSound ("hat"));
    writeMono (samples.getChildFile ("Hats/Hat 2.wav"), drumSound ("hat"));
    const auto indexFolder = a.t.root.getChildFile ("sounds");
    SoundIndex index (indexFolder);
    SoundIndexer::update (index, helper, { synth }, { samples }, {}, {});
    juce::String dark;
    for (const auto& s : index.instruments.at (synth.key).sounds)
        if (s["name"].toString() == "Dark Square")
            dark = s["id"].toString();
    REQUIRE (dark.isNotEmpty());

    // A reference song in the analysis library: its bass played by Dark Square, a chord-less other part, a
    // tune, and drums, with their notes and hits (as Basic Pitch and the drum recogniser keep them).
    std::vector<std::tuple<int, double, double>> bass, other, vocals;
    for (int i = 0; i < 24; ++i)
    {
        bass.emplace_back (i % 4 == 2 ? 43 : 36, 0.5 + i * 0.5, 0.9 + i * 0.5);
        other.emplace_back (60 + (i % 3) * 4, 0.5 + i * 0.5, 0.8 + i * 0.5);
        vocals.emplace_back (67 + (i % 2) * 2, 0.5 + i * 0.5, 0.95 + i * 0.5);
    }
    const auto made = a.t.root.getChildFile ("made");
    auto render = [&] (const juce::String& part, const juce::String& preset,
                       const std::vector<std::tuple<int, double, double>>& notes)
    {
        juce::Array<juce::var> list;
        for (const auto& [pitch, start, end] : notes)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("note", pitch);
            o->setProperty ("start", start);
            o->setProperty ("length", end - start);
            list.add (juce::var (o));
        }
        auto* item = new juce::DynamicObject();
        item->setProperty ("id", part);
        item->setProperty ("preset", preset);
        item->setProperty ("notes", list);
        item->setProperty ("output", made.getChildFile (part + ".wav").getFullPathName());
        juce::String problem;
        parts::renderInHelper (helper, synth, { juce::var (item) }, {}, problem);
        INFO (problem);
        REQUIRE (made.getChildFile (part + ".wav").existsAsFile());
        return made.getChildFile (part + ".wav");
    };
    std::map<juce::String, juce::File> stems { { "bass", render ("bass", dark, bass) },
                                               { "other", render ("other", {}, other) },
                                               { "vocals", render ("vocals", {}, vocals) } };
    juce::AudioBuffer<float> drums (1, 48000 * 14);
    drums.clear();
    juce::Array<juce::var> hits;
    for (int i = 0; i < 48; ++i)
    {
        const auto kind = i % 4 == 0   ? juce::String ("kick")
                          : i % 4 == 2 ? juce::String ("snare")
                                       : juce::String ("closed_hat");
        const auto sound = drumSound (kind == "closed_hat" ? "hat" : kind);
        const auto at = (int)((0.5 + i * 0.25) * 48000);
        drums.addFrom (0, at, sound, 0, 0, juce::jmin (sound.getNumSamples(), drums.getNumSamples() - at));
        auto* h = new juce::DynamicObject();
        h->setProperty ("time", 0.5 + i * 0.25);
        h->setProperty ("kind", kind);
        hits.add (juce::var (h));
    }
    writeMono (made.getChildFile ("drums.wav"), drums);
    stems["drums"] = made.getChildFile ("drums.wav");
    AnalysisLibrary library (a.t.root.getChildFile ("library"));
    const juce::String digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    REQUIRE (library.keepStems (digest, "htdemucs", stems, [] { return false; }).wasOk());
    library.setIn (digest, "Song.wav", "notes", "htdemucs/bass", notesJson (bass));
    library.setIn (digest, "Song.wav", "notes", "htdemucs/other", notesJson (other));
    library.setIn (digest, "Song.wav", "notes", "htdemucs/vocals", notesJson (vocals));
    library.setIn (digest, "Song.wav", "drums", "htdemucs", hits);

    const auto summary = juce::JSON::parse (R"({"kind": "spinus.style-summary", "tempo": {"median": 120.0},
        "keys": [{"name": "C major", "count": 1}], "loudness": {"lufs": -10.0},
        "parts": {"bass": {"songs": 1, "low": 36, "median": 36, "high": 43, "notesPerBar": 8, "voices": 1.0},
                  "other": {"songs": 1, "low": 60, "median": 64, "high": 68, "notesPerBar": 8, "voices": 1.0},
                  "vocals": {"songs": 1, "low": 67, "median": 68, "high": 69, "notesPerBar": 8, "voices": 1.0},
                  "drums": {"songs": 1, "notesPerBar": 16}}})");
    PartSoundsPanel::Context c;
    c.summary = summary;
    c.songs.push_back ({ digest, "Song" });
    c.library = std::move (library);
    c.indexFolder = indexFolder;
    c.workFolder = a.t.root.getChildFile ("work");
    c.helper = std::move (helper);
    c.service = &a.service;
    return c;
}
} // namespace

TEST_CASE ("Track creator: sounds for each part, chosen, heard, tuned, mixed and added in one undo step",
           "[ui][trackcreator][sounds][parts]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    auto context = makePartSoundsContext (a);
    Session session { a.host.getEngine(), makeDevices (a.host.getEngine()) }; // with the Drum Machine
    PartSoundsPanel panel (session, [context] { return context; });
    panel.setSize (700, 600);

    // Without AI: chosen by the measurements alone, and it says so (for a sketch's parts).
    panel.setTune (false);
    REQUIRE (panel.suggestParts ("sketch"));
    REQUIRE (panel.chooseSounds());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    INFO (panel.getStatusText());
    CHECK (panel.getStatusText().contains ("AI is off"));
    const auto& rows = panel.getRows();
    REQUIRE (rows.size() == 4);
    for (const auto& row : rows)
        CHECK_FALSE (row.choices.empty());
    // The sketch's parts, in its order: drums, bass, chords, lead.
    CHECK (rows[0].part.role == "drums");
    CHECK (rows[1].target.part == "bass");
    CHECK (rows[1].measured >= 10);
    REQUIRE (rows[1].chosen() != nullptr);
    for (const auto& c : rows[1].choices)
        UNSCOPED_INFO (c.title() << " " << c.distance << ": " << c.reasons());
    CHECK (rows[1].chosen()->name == "Dark Square"); // its own sound, measured back
    CHECK (rows[0].chosen()->kind == parts::Choice::Kind::drumSamples);
    CHECK (rows[0].chosen()->drums.at ("kick").endsWith ("Kick 1.wav"));
    // Every kind of drum source there is can be picked: the sample set, Spinus's Drum Machine.
    CHECK (std::any_of (rows[0].choices.begin(), rows[0].choices.end(),
                        [] (const parts::Choice& c) { return c.source.stockType == "spinus.drum-machine"; }));
    CHECK (panel.describe (1).contains ("attack"));
    CHECK (rows[1].effects.isEmpty());

    // Heard: the chosen sound playing a phrase, and the song's part.
    REQUIRE (panel.listen (1));
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 60000));
    CHECK (panel.getLastPlayed().existsAsFile());
    REQUIRE (panel.listenToSongs (0));
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 60000));
    CHECK (panel.getLastPlayed().getFileName() == "drums-song.wav");

    // With AI on: tuned (in the helper), then effects and levels toward the songs (the CLAP model is not there:
    // listening closer is skipped, saying so).
    a.service.setEnabled (true);
    REQUIRE (a.installEngine());
    panel.setTune (true);
    panel.setTuneTries (30);
    REQUIRE (panel.suggestParts ("sketch"));
    REQUIRE (panel.chooseSounds());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 240000));
    INFO (panel.getStatusText());
    CHECK (panel.getStatusText().contains ("CLAP model"));
    REQUIRE (rows[1].tuned.has_value());
    CHECK (rows[1].note.contains ("Tuned toward the song"));
    const auto plan = panel.makePlan();
    REQUIRE (plan.parts.size() == 4);
    CHECK_FALSE (plan.master.isEmpty());
    CHECK (plan.master[0]["kind"].toString() == "limiter");
    CHECK (
        std::any_of (plan.parts.begin(), plan.parts.end(), [] (const auto& p) { return std::abs (p.volumeDb) > 0.0; }));

    // Swapped: its tuning and effects are dropped.
    if (rows[2].choices.size() > 1)
    {
        REQUIRE (panel.select (2, 1));
        CHECK (rows[2].effects.isEmpty());
        CHECK_FALSE (rows[2].tuned.has_value());
    }

    // Added as one undo step.
    const auto channels = session.getProject().channels.size();
    const auto steps = session.getHistory().getUndoDescriptions().size();
    REQUIRE (panel.addToProject());
    INFO (panel.getStatusText());
    CHECK (panel.getStatusText().contains ("one undo step"));
    CHECK_FALSE (panel.getStatusText().contains ("Not every sound loaded"));
    CHECK (session.getProject().channels.size() == channels + 4);
    CHECK (session.getHistory().getUndoDescriptions().size() == steps + 1);
    CHECK (session.getHistory().getUndoDescriptions().strings.getLast() == "Track creator: sounds for 4 parts");
    REQUIRE (panel.getAddedChannels().size() == 4);
    const auto* bassChannel = session.getProject().findChannel (panel.getAddedChannels()[1]);
    REQUIRE (bassChannel != nullptr);
    CHECK (bassChannel->name == "Bass (Track creator)");
    CHECK_FALSE (bassChannel->instrument.value().parameters.empty()); // the tuned settings
    REQUIRE (session.getHistory().undo());
    CHECK (session.getProject().channels.size() == channels);
}

TEST_CASE ("Track creator: a written song's parts start at their roles' levels, measured with their sounds",
           "[ui][trackcreator][sounds][parts]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    auto context = makePartSoundsContext (a);
    Session session { a.host.getEngine(), makeDevices (a.host.getEngine()) };
    PartSoundsPanel panel (session, [context] { return context; });
    a.service.setEnabled (true);
    panel.setTune (false);
    panel.setEffects (false); // no level toward the songs: the starting balance sets them
    panel.setListenCloser (false);
    REQUIRE (panel.suggestParts ("sketch"));
    REQUIRE (panel.chooseSounds());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));

    SongWriterPanel writer (session,
                            [&]
                            {
                                SongWriterPanel::Context c;
                                c.summary = context.summary;
                                c.service = &a.service;
                                c.workFolder = a.t.root.getChildFile ("song");
                                c.sounds = panel.makePlan();
                                c.indexFolder = context.indexFolder;
                                c.parts = panel.getParts();
                                c.partsFromPage = true;
                                c.helper = context.helper;
                                return c;
                            });
    writer.setSeed (3);
    REQUIRE (writer.write());
    REQUIRE (waitFor ([&] { return ! writer.isBusy(); }, 240000));
    INFO (writer.getStatusText());
    CHECK (writer.getStatusText().contains ("Each part starts at its role's level"));

    // The parts played with the test synth's presets were measured: their faders go in with Apply, the loudest
    // at 0 dB, none turned up.
    const auto proposal = writer.makeProposal();
    REQUIRE (proposal.isObject());
    int set = 0;
    for (const auto& step : *proposal["steps"].getArray())
        if (step["command"].toString() == "insert.set" && step["parameters"].hasProperty ("volumeDb"))
        {
            ++set;
            CHECK ((double)step["parameters"]["volumeDb"] <= 0.0);
        }
    CHECK (set >= 2);
}
#endif

#if defined(SPINUS_PLUGIN_SCANNER) && defined(SPINUS_BROKEN_PLUGINS_DIR)
TEST_CASE ("Track creator sounds for each part screenshot for the manual", "[.screenshot][manual]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Analysis a;
    auto context = makePartSoundsContext (a);
    a.service.setEnabled (true);
    REQUIRE (a.installEngine());
    SpinusLookAndFeel look (Theme::dark());
    PartSoundsPanel panel (a.session, [context] { return context; });
    panel.setLookAndFeel (&look);
    panel.setSize (700, 560);
    panel.setTune (true);
    panel.setTuneTries (30);
    REQUIRE (panel.suggestParts ("sketch"));
    REQUIRE (panel.chooseSounds());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 240000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/track-creator-sounds.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::Image image (juce::Image::RGB, panel.getWidth() + 20, panel.getHeight() + 20, true);
    {
        juce::Graphics g (image);
        g.fillAll (look.findColour (juce::ResizableWindow::backgroundColourId));
        g.drawImageAt (panel.createComponentSnapshot (panel.getLocalBounds()), 10, 10);
    }
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    panel.setLookAndFeel (nullptr);
}
#endif

#if defined(SPINUS_PLUGIN_SCANNER)
TEST_CASE ("Track creator: sounds for the parts of real songs (SPINUS_REALMUSIC_BATCH)", "[.realsounds]")
{
    // A check by hand, not run in CI, after "Track creator on real songs" analyzed the batch (its summary is
    // <batch>.json in SPINUS_REALMUSIC_OUT, its analyses in SPINUS_REALMUSIC_SETTINGS' library). Builds or
    // updates the sound index in SPINUS_REALSOUNDS_INDEX from Spinus's 3x Osc and Drum Machine, Surge XT
    // (SPINUS_SURGE_CLAP) and the sample folder SPINUS_REALSOUNDS_SAMPLES, then chooses, tunes and mixes the
    // sounds for each part with the AI service on. Writes <batch>-sounds.txt. Only reads the songs.
    auto env = [] (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); };
    const juce::File batch (env ("SPINUS_REALMUSIC_BATCH")), out (env ("SPINUS_REALMUSIC_OUT")),
        settings (env ("SPINUS_REALMUSIC_SETTINGS")), indexFolder (env ("SPINUS_REALSOUNDS_INDEX"));
    if (! batch.isDirectory() || ! settings.isDirectory() || indexFolder == juce::File())
        SKIP ("set SPINUS_REALMUSIC_BATCH, _OUT, _SETTINGS and SPINUS_REALSOUNDS_INDEX");
    const auto summary = juce::JSON::parse (out.getChildFile (batch.getFileName() + ".json"));
    REQUIRE (summary.isObject());

    SoundIndexer::Options helper;
    helper.helper = juce::File (SPINUS_PLUGIN_SCANNER);
    std::vector<SoundSource> sources;
    for (const auto* own : { "3x-osc", "drum-machine" })
    {
        SoundSource s;
        s.key = juce::String ("stock-spinus.") + own;
        s.name = juce::String (own) == "3x-osc" ? "3x Osc" : "Drum Machine";
        s.format = "Spinus";
        s.file = juce::String ("spinus:") + own;
        s.stockType = juce::String ("spinus.") + own;
        s.badge = "Spinus";
        s.fingerprint = "real check";
        sources.push_back (s);
    }
    if (const juce::File surge (env ("SPINUS_SURGE_CLAP")); surge.exists())
    {
        SoundSource s;
        s.key = "surge-xt";
        s.name = s.pluginName = "Surge XT";
        s.format = "CLAP";
        s.file = surge.getFullPathName();
        s.stockType = "bundled.surge-xt";
        s.badge = "CLAP";
        s.fingerprint = "real check";
        sources.push_back (s);
    }
    juce::Array<juce::File> folders;
    if (const juce::File samples (env ("SPINUS_REALSOUNDS_SAMPLES")); samples.isDirectory())
        folders.add (samples);
    SoundIndex index (indexFolder);
    index.load();
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto updated = SoundIndexer::update (index, helper, sources, folders, {}, {});
    juce::StringArray lines;
    lines.add ("Sound index: " + juce::String (index.summarize().presets) + " presets, " +
               juce::String (index.summarize().samples) + " samples (" + juce::String (updated.soundsMeasured) +
               " sounds and " + juce::String (updated.samplesMeasured) + " samples measured now, " +
               juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 0) +
               " s); roles learnt from " + juce::String (index.roleExamples) + " named presets");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine())); // Spinus's own and the built Surge XT
    ai::AiService service (settings);
    service.setEnabled (true);
    const auto library = AnalysisLibrary::inAiFolder (service.getFolder());
    if (env ("SPINUS_REALSOUNDS_CLAP") == "1")
    {
        // Closest preset's CLAP model, downloaded once into the models folder of the HOME this runs with.
        auto alive = std::make_shared<bool> (true);
        bool done = false;
        juce::String error;
        ai::runJob (service, "ai.v1.stems.download", juce::JSON::parse (R"({"model": "clap_music"})"), alive, {},
                    [&] (const juce::var&, const juce::String& e)
                    {
                        error = e;
                        done = true;
                    });
        REQUIRE (waitFor ([&] { return done; }, 3600000));
        INFO (error);
        CHECK (error.isEmpty());
    }
    PartSoundsPanel::Context context;
    context.summary = summary;
    for (const auto& entry : juce::RangedDirectoryIterator (batch, true, "*.mp3;*.flac;*.wav", juce::File::findFiles))
        context.songs.push_back ({ contentDigest (entry.getFile()), entry.getFile().getFileNameWithoutExtension() });
    context.library = library;
    context.indexFolder = indexFolder;
    context.workFolder = out.getChildFile (batch.getFileName() + "-work");
    context.helper = helper;
    context.service = &service;
    PartSoundsPanel panel (session, [context] { return context; });
    panel.setTune (env ("SPINUS_REALSOUNDS_TUNE") != "0");
    panel.setTuneTries (
        env ("SPINUS_REALSOUNDS_TRIES").getIntValue() > 0 ? env ("SPINUS_REALSOUNDS_TRIES").getIntValue() : 300);
    const auto chose = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (panel.suggestParts ("sketch"));
    REQUIRE (panel.chooseSounds());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 4 * 3600000));
    lines.add ("Batch " + batch.getFileName() + ": " + juce::String (context.songs.size()) + " songs, " +
               juce::String ((juce::Time::getMillisecondCounterHiRes() - chose) / 1000.0, 0) + " s");
    lines.add (panel.getStatusText());
    for (int i = 0; i < (int)panel.getRows().size(); ++i)
    {
        const auto& row = panel.getRows()[(size_t)i];
        lines.add ({});
        lines.add (panel.describe (i));
        lines.add ("  reference song: " + row.songName + ", excerpt at " + juce::String (row.excerpt.start, 1) + " s");
        for (size_t k = 0; k < row.choices.size(); ++k)
        {
            const auto& c = row.choices[k];
            lines.add ("  " + juce::String ((int)k + 1) + ". " + c.title() + " | role " + c.role + " (" + c.roleFrom +
                       ") | distance " + juce::String (c.distance, 2) +
                       (c.clap > -2.0 ? " | CLAP " + juce::String (c.clap, 3) : juce::String()) + " | " + c.reasons());
        }
        for (const auto& e : row.effects)
            lines.add ("  effect: " + e["title"].toString() + ": " + e["reason"].toString());
        if (std::abs (row.volumeDb) > 0.0)
            lines.add ("  fader " + juce::String (row.volumeDb, 1) + " dB");
    }
    for (const auto& m : panel.makePlan().master)
        lines.add ("master: " + m["title"].toString() + ": " + m["reason"].toString());
    REQUIRE (panel.addToProject());
    lines.add ({});
    lines.add (panel.getStatusText());
    out.getChildFile (batch.getFileName() + "-sounds.txt").replaceWithText (lines.joinIntoString ("\n") + "\n");
}
#endif

#if defined(SPINUS_PLUGIN_SCANNER)
TEST_CASE ("Track creator: a new song in the style of real songs (SPINUS_REALMUSIC_BATCH)", "[.realsong]")
{
    // A check by hand, not run in CI, after "Track creator on real songs" analyzed the batch and the sound index
    // was built (as for "[.realsounds]"). Chooses the sounds for each part and adds them, writes a song in the
    // style (rules only, seed SPINUS_REALSONG_SEED, default 1), applies it, exports it to <batch>-unmixed.flac,
    // runs the automixer toward the first song (rules only) and exports <batch>.flac, into SPINUS_REALSONG_OUT,
    // with the song (<batch>-song.json) and what the panel says about it (<batch>-song.txt). Never plays a sound
    // out loud; only reads the songs.
    auto env = [] (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); };
    const juce::File batch (env ("SPINUS_REALMUSIC_BATCH")), out (env ("SPINUS_REALMUSIC_OUT")),
        settings (env ("SPINUS_REALMUSIC_SETTINGS")), indexFolder (env ("SPINUS_REALSOUNDS_INDEX")),
        songOut (env ("SPINUS_REALSONG_OUT"));
    if (! batch.isDirectory() || ! settings.isDirectory() || indexFolder == juce::File() || songOut == juce::File())
        SKIP ("set SPINUS_REALMUSIC_BATCH, _OUT, _SETTINGS, SPINUS_REALSOUNDS_INDEX and SPINUS_REALSONG_OUT");
    const auto summary = juce::JSON::parse (out.getChildFile (batch.getFileName() + ".json"));
    REQUIRE (summary.isObject());
    songOut.createDirectory();
    const auto name = batch.getFileName();
    juce::StringArray log;
    auto note = [&] (const juce::String& line)
    {
        log.add (line);
        songOut.getChildFile (name + "-run.log").replaceWithText (log.joinIntoString ("\n") + "\n");
    };

    SoundIndexer::Options helper;
    helper.helper = juce::File (SPINUS_PLUGIN_SCANNER);
    std::vector<SoundSource> sources;
    for (const auto* own : { "3x-osc", "drum-machine" })
    {
        SoundSource s;
        s.key = juce::String ("stock-spinus.") + own;
        s.name = juce::String (own) == "3x-osc" ? "3x Osc" : "Drum Machine";
        s.format = "Spinus";
        s.file = juce::String ("spinus:") + own;
        s.stockType = juce::String ("spinus.") + own;
        s.badge = "Spinus";
        s.fingerprint = "real check";
        sources.push_back (s);
    }
    if (const juce::File surge (env ("SPINUS_SURGE_CLAP")); surge.exists())
    {
        SoundSource s;
        s.key = "surge-xt";
        s.name = s.pluginName = "Surge XT";
        s.format = "CLAP";
        s.file = surge.getFullPathName();
        s.stockType = "bundled.surge-xt";
        s.badge = "CLAP";
        s.fingerprint = "real check";
        sources.push_back (s);
    }
    juce::Array<juce::File> folders;
    if (const juce::File samples (env ("SPINUS_REALSOUNDS_SAMPLES")); samples.isDirectory())
        folders.add (samples);
    SoundIndex index (indexFolder);
    index.load();
    SoundIndexer::update (index, helper, sources, folders, {}, {});
    note ("Sound index: " + juce::String (index.summarize().presets) + " presets, " +
          juce::String (index.summarize().samples) + " samples");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    ai::AiService service (settings);
    service.setEnabled (true);
    PartSoundsPanel::Context context;
    context.summary = summary;
    juce::Array<juce::File> songs;
    for (const auto& entry : juce::RangedDirectoryIterator (batch, true, "*.mp3;*.flac;*.wav", juce::File::findFiles))
    {
        songs.add (entry.getFile());
        context.songs.push_back ({ contentDigest (entry.getFile()), entry.getFile().getFileNameWithoutExtension() });
    }
    songs.sort();
    context.library = AnalysisLibrary::inAiFolder (service.getFolder());
    context.indexFolder = indexFolder;
    context.workFolder = songOut.getChildFile (name + "-work");
    context.helper = helper;
    context.service = &service;
    PartSoundsPanel sounds (session, [context] { return context; });
    sounds.setTune (env ("SPINUS_REALSOUNDS_TUNE") != "0");
    sounds.setTuneTries (
        env ("SPINUS_REALSOUNDS_TRIES").getIntValue() > 0 ? env ("SPINUS_REALSOUNDS_TRIES").getIntValue() : 300);
    REQUIRE (sounds.chooseSounds());
    REQUIRE (waitFor ([&] { return ! sounds.isBusy(); }, 4 * 3600000));
    note (sounds.getStatusText());
    for (int i = 0; i < (int)sounds.getRows().size(); ++i)
        note (sounds.describe (i).upToFirstOccurrenceOf ("\n", false, false));
    REQUIRE (sounds.addToProject());
    note (sounds.getStatusText());

    SongWriterPanel writer (session,
                            [&]
                            {
                                SongWriterPanel::Context c;
                                c.summary = summary;
                                c.service = &service;
                                c.workFolder = songOut.getChildFile (name + "-work");
                                c.songs = songs;
                                c.sounds = sounds.makePlan();
                                return c;
                            });
    writer.setSeed (env ("SPINUS_REALSONG_SEED").isNotEmpty() ? env ("SPINUS_REALSONG_SEED").getIntValue() : 1);
    REQUIRE (writer.write());
    REQUIRE (waitFor ([&] { return ! writer.isBusy(); }, 600000));
    INFO (writer.getStatusText());
    REQUIRE (writer.getSong().isObject());
    REQUIRE (writer.apply());
    note (writer.getStatusText());
    songOut.getChildFile (name + "-song.json").replaceWithText (juce::JSON::toString (writer.getSong()));
    songOut.getChildFile (name + "-song.txt").replaceWithText (writer.getText() + "\n");

    auto exportTo = [&] (const juce::File& file)
    {
        engine::ExportSettings exportSettings;
        exportSettings.format = engine::ExportSettings::Format::flac;
        std::optional<engine::ExportJob::Result> result;
        auto job =
            session.createExportJob (file, exportSettings, [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        REQUIRE (waitFor ([&] { return result.has_value(); }, 3600000));
        INFO (result->error);
        CHECK (result->ok);
        note ("Exported " + file.getFullPathName());
    };
    exportTo (songOut.getChildFile (name + "-unmixed.flac"));

    // Then mixed: the automixer toward the first reference song (rules only: no AI connection here).
    Automixer automixer (session, &service);
    if (! songs.isEmpty())
        automixer.setReference (songs.getFirst());
    REQUIRE (automixer.analyze (Automixer::Target::club));
    REQUIRE (waitFor ([&] { return ! automixer.isBusy(); }, 3600000));
    note ("Automixer: " + automixer.getStatusText() + " " + automixer.getNote());
    for (const auto& item : automixer.getItems())
        note ("  " + item.json["title"].toString() + ": " + item.json["reason"].toString());
    if (! automixer.getItems().empty())
        CHECK (automixer.apply());
    exportTo (songOut.getChildFile (name + ".flac"));
}
#endif
