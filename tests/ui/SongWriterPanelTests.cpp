#include <catch2/catch_test_macros.hpp>

#include "AnalysisFakes.h"
#include "FakeProvider.h"
#include "dsp/sampler/SampleStorage.h"
#include "engine/EngineHost.h"
#include "ui/SongWriterPanel.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;
using namespace spinus::test::analysis;

/* The Track creator's Write a song page: the AI service runs for real (spinus_ai/songwriter.py, rules only, no AI
   connection); the song is heard through the instruments before Apply, applied in one undo step, and one part
   written again keeps the rest. */

namespace
{
/** A style summary as spinus_ai.style.summarize writes them: two minor songs, a short form (so the song renders
    quickly). */
juce::var testSummary()
{
    return juce::JSON::parse (R"({
        "kind": "spinus.style-summary", "version": 1, "songs": 2, "names": ["One", "Two"],
        "tempo": {"median": 128.0, "min": 124.0, "max": 132.0}, "beatsPerBar": {"4": 2},
        "keys": [{"name": "A minor", "count": 1}, {"name": "D minor", "count": 1}], "modes": {"minor": 1.0},
        "chords": {"barsPerChord": 1.0},
        "drums": {"songs": 2, "steps": 16, "swing": 0.5, "lanes": {
            "kick": [0.95, 0, 0.1, 0, 0.9, 0, 0.3, 0, 0.92, 0, 0.1, 0.2, 0.9, 0, 0.1, 0],
            "snare": [0, 0, 0, 0, 0.95, 0, 0, 0.1, 0, 0, 0, 0, 0.93, 0, 0.2, 0.3],
            "hats": [0.9, 0.1, 0.85, 0.1, 0.9, 0.1, 0.85, 0.3, 0.9, 0.1, 0.85, 0.1, 0.9, 0.1, 0.85, 0.4]}},
        "parts": {"bass": {"songs": 2, "notesPerBar": 5.0, "low": 31, "median": 38, "high": 47, "voices": 1.1},
                  "drums": {"songs": 2, "notesPerBar": 13.0},
                  "other": {"songs": 2, "notesPerBar": 12.0, "low": 48, "median": 58, "high": 72, "voices": 2.6},
                  "vocals": {"songs": 2, "notesPerBar": 3.0, "low": 55, "median": 64, "high": 76, "voices": 1.0}},
        "sections": {"lengths": [{"bars": 4, "count": 6}, {"bars": 8, "count": 2}], "perSong": 5.0, "songBars": 32.0},
        "loudness": {"lufs": -9.0},
        "perSong": [
            {"name": "One", "key": {"tonic": 9, "scale": "minor", "name": "A minor"},
             "chords": [{"roman": "i"}, {"roman": "bVI"}, {"roman": "bIII"}, {"roman": "bVII"},
                        {"roman": "i"}, {"roman": "bVI"}, {"roman": "bIII"}, {"roman": "bVII"},
                        {"roman": "i"}, {"roman": "iv"}, {"roman": "bVI"}, {"roman": "V"}]},
            {"name": "Two", "key": {"tonic": 2, "scale": "minor", "name": "D minor"},
             "chords": [{"roman": "i"}, {"roman": "iv"}, {"roman": "bVI"}, {"roman": "V"},
                        {"roman": "i"}, {"roman": "iv"}, {"roman": "bVI"}, {"roman": "V"},
                        {"roman": "bVII"}, {"roman": "bIII"}, {"roman": "iv"}, {"roman": "i"}]}],
        "plain": []
    })");
}

float peakOf (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (file));
    if (reader == nullptr)
        return 0.0f;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio.getMagnitude (0, audio.getNumSamples());
}
} // namespace

TEST_CASE ("Track creator: a song written, heard before Apply, applied and a part redone, each one undo step",
           "[ui][trackcreator][song]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    context.workFolder = t.root.getChildFile ("song");
    context.songs.add (t.root.getChildFile ("reference.wav"));
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setSize (700, 560);
    panel.setSeed (11);

    // AI off: no service is started, and the panel says what to do.
    CHECK_FALSE (panel.write());
    CHECK (panel.getStatusText().contains ("Options > AI"));
    CHECK (service.getState() != ai::AiService::State::running);

    service.setEnabled (true);
    const auto before = session.getProject();
    REQUIRE (panel.write());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    INFO (panel.getStatusText());
    REQUIRE (panel.getSong().isObject());
    CHECK (panel.getText().contains ("BPM"));
    CHECK (panel.getText().contains ("Compared with the songs"));
    const auto first = juce::JSON::toString (panel.getSong()["parts"]);
    // Nothing changes the project before Apply.
    CHECK (session.getProject() == before);
    CHECK_FALSE (session.getHistory().canUndo());

    // Another version: another song.
    REQUIRE (panel.anotherVersion());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    CHECK (juce::JSON::toString (panel.getSong()["parts"]) != first);

    // Heard before Apply: the song rendered through the instruments, the project untouched.
    REQUIRE (panel.listen());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 600000));
    INFO (panel.getStatusText());
    REQUIRE (panel.getLastPreview().existsAsFile());
    CHECK (peakOf (panel.getLastPreview()) > 0.01f);
    CHECK (session.getProject() == before);
    session.getSamplePreview().stop();

    // Applied: one undo step.
    REQUIRE (panel.apply());
    INFO (panel.getStatusText());
    CHECK (panel.getStatusText().contains ("one undo step"));
    CHECK (session.getHistory().getUndoDescriptions().size() == 1);
    CHECK (session.getHistory().getUndoDescriptions()[0].startsWith ("Track creator: a new song"));
    CHECK (session.getProject().channels.size() == before.channels.size() + 4);
    REQUIRE (panel.getApplied().isIn (session.getProject()));
    const auto applied = session.getProject();

    // Mix it: the automixer, with the reference song.
    juce::File reference;
    panel.onMix = [&] (const juce::File& f) { reference = f; };
    REQUIRE (panel.mix());
    CHECK (reference.getFileName() == "reference.wav");

    // A new bass: only the bass changes, as one more undo step.
    REQUIRE (panel.redo ("bass"));
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    CHECK (session.getProject() == applied); // not before Apply
    CHECK (panel.makeProposal()["summary"].toString() == "new bass");
    REQUIRE (panel.apply());
    CHECK (session.getHistory().getUndoDescriptions().size() == 2);
    const auto& channels = panel.getApplied().channels;
    for (const auto& [section, id] : panel.getApplied().patterns)
    {
        const auto* now = session.getProject().findPattern (id);
        const auto* was = applied.findPattern (id);
        REQUIRE (now != nullptr);
        REQUIRE (was != nullptr);
        for (const auto* part : { "drums", "chords", "lead" })
        {
            const auto channel = channels.at (part);
            CHECK ((now->notes.contains (channel) ? now->notes.at (channel) : std::vector<model::Note> {}) ==
                   (was->notes.contains (channel) ? was->notes.at (channel) : std::vector<model::Note> {}));
        }
    }
    CHECK (session.getProject().playlist == applied.playlist);
    // Nothing new: Apply says so.
    CHECK_FALSE (panel.apply());

    // Undone: the song is gone, and Apply would add it whole again.
    REQUIRE (session.getHistory().undo());
    REQUIRE (session.getHistory().undo());
    CHECK (session.getProject().channels == before.channels);
    CHECK (session.getProject().patterns == before.patterns);
    CHECK (panel.makeProposal()["summary"].toString().startsWith ("a new song"));
}

TEST_CASE ("Track creator: without the AI service, the song page says so", "[ui][trackcreator][song]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    SongWriterPanel::Context context;
    context.summary = testSummary();
    SongWriterPanel panel (session, [&context] { return context; });
    CHECK_FALSE (panel.write());
    CHECK (panel.getStatusText().contains ("AI service"));
    CHECK_FALSE (panel.apply());
    CHECK (panel.getStatusText().contains ("Write a song first"));
    context.summary = juce::var();
    CHECK_FALSE (panel.write());
    CHECK (panel.getStatusText().contains ("no style summary"));
}

TEST_CASE ("Track creator write a song screenshot for the manual", "[.screenshot][manual]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    service.setEnabled (true);
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    SpinusLookAndFeel look (Theme::dark());
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setLookAndFeel (&look);
    panel.setSize (700, 520);
    panel.setSeed (3);
    REQUIRE (panel.write());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/track-creator-song.png");
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
