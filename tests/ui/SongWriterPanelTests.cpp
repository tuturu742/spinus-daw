#include <catch2/catch_test_macros.hpp>

#include "AnalysisFakes.h"
#include "FakeProvider.h"
#include "dsp/sampler/SampleStorage.h"
#include "engine/EngineHost.h"
#include "session/PartSounds.h"
#include "session/ProjectDocument.h"
#include "session/SongWriter.h"
#include "ui/MainView.h"
#include "ui/Playlist.h"
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

TEST_CASE ("Track creator: a full arrangement's parts are written and applied as one undo step; none, none",
           "[ui][trackcreator][song]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    service.setEnabled (true);
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    context.workFolder = t.root.getChildFile ("song");
    context.partsFromPage = true;
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setSize (700, 560);
    panel.setSeed (5);
    // The Sounds page has no parts yet: it says so.
    CHECK_FALSE (panel.write());
    CHECK (panel.getStatusText().contains ("no parts yet"));
    context.parts = parts::suggestedParts ("full", context.summary);

    const auto before = session.getProject();
    REQUIRE (panel.write());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    INFO (panel.getStatusText());
    REQUIRE (song::isFull (panel.getSong()));
    const auto layers = song::layersOf (panel.getSong());
    CHECK (layers.size() >= 8);
    CHECK (panel.getText().contains ("parts, a channel each"));
    CHECK (panel.getText().contains ("Drums, Bass, Synths, Leads, FX"));

    REQUIRE (panel.apply());
    CHECK (session.getHistory().getUndoDescriptions().size() == 1);
    CHECK (session.getProject().channels.size() == before.channels.size() + layers.size());
    REQUIRE (panel.getApplied().full);
    REQUIRE (panel.getApplied().isIn (session.getProject()));

    // A new lead: the lead and counter-melody layers change, as one more undo step.
    REQUIRE (panel.redo ("lead"));
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    CHECK (panel.makeProposal()["summary"].toString() == "new lead");
    REQUIRE (panel.apply());
    CHECK (session.getHistory().getUndoDescriptions().size() == 2);

    REQUIRE (session.getHistory().undo());
    REQUIRE (session.getHistory().undo());
    CHECK (session.getProject().channels == before.channels);
}

TEST_CASE ("Track creator: the user's parts are written with the Sounds page's sounds, and a new song replaces the "
           "last after asking",
           "[ui][trackcreator][song]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    service.setEnabled (true);
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    // Three parts, as the owner's example: the Drum Machine, a guitar (here 3x Osc) and a synth lead.
    context.parts = { { "kit", "Drum Machine", "drums" },
                      { "guitar", "Guitar", "chords" },
                      { "synth", "Synth", "lead" } };
    // The Sounds page chose the Drum Machine for the drums.
    parts::Plan::Part drums;
    drums.target.id = "kit";
    drums.target.part = "drums";
    drums.choice.kind = parts::Choice::Kind::kit;
    drums.choice.source.stockType = "spinus.drum-machine";
    drums.choice.source.name = "Drum Machine";
    drums.choice.drumNotes = { { "kick", 60 }, { "snare", 61 }, { "hat", 63 }, { "crash", 70 } };
    context.sounds.parts.push_back (drums);
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setSize (700, 560);
    panel.setSeed (21);
    juce::StringArray asked;
    int answer = 0;
    panel.setMessageBoxHandler (
        [&] (const juce::MessageBoxOptions& o, const std::function<void (int)>& done)
        {
            asked.add (o.getMessage());
            done (answer);
        });

    REQUIRE (panel.write());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    const auto layers = song::layersOf (panel.getSong());
    REQUIRE (layers.size() == 3);
    CHECK (layers[1].name == "Guitar");
    const auto before = session.getProject().channels.size();
    REQUIRE (panel.apply());
    CHECK (asked.isEmpty()); // nothing to replace yet
    const auto& project = session.getProject();
    REQUIRE (project.channels.size() == before + 3);
    const model::Channel* kit = nullptr;
    for (const auto& c : project.channels)
        if (c.name == "Drum Machine")
            kit = &c;
    REQUIRE (kit != nullptr);
    REQUIRE (kit->instrument.has_value());
    CHECK (kit->instrument.value().stockType == "spinus.drum-machine"); // the Sounds page's choice
    const auto first = session.getProject();

    // Another version, applied: it asks, and replaces Song 1 in its place (one more undo step).
    REQUIRE (panel.anotherVersion());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    REQUIRE (panel.apply());
    REQUIRE (asked.size() == 1);
    CHECK (asked[0] == "Replace Song 1? It will be overwritten (Ctrl+Z brings it back).");
    CHECK (session.getProject().channels.size() == first.channels.size());
    CHECK (session.getProject().patterns.size() == first.patterns.size());
    CHECK (song::lastSongName (session.getProject()) == "Song 1");
    CHECK (session.getHistory().getUndoDescriptions().size() == 2);
    REQUIRE (session.getHistory().undo());
    CHECK (session.getProject().patterns == first.patterns);

    // Keep both: Song 2 beside it.
    answer = 1;
    REQUIRE (panel.apply());
    CHECK (song::lastSongName (session.getProject()) == "Song 2");
    // Cancel: nothing.
    answer = 2;
    const auto steps = session.getHistory().getUndoDescriptions().size();
    REQUIRE (panel.anotherVersion());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    panel.apply();
    CHECK (session.getHistory().getUndoDescriptions().size() == steps);
}

TEST_CASE ("Track creator: the AI connection composes the song with the harness's tools, opt-in",
           "[ui][trackcreator][song][ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    // The fake model: looks at the form, lets the rules write two parts, writes the drums itself, checks, finishes
    // (refused once while check finds issues, then again).
    const auto replies = t.root.getChildFile ("replies.json");
    replies.replaceWithText (R"([
        {"tool": ["form", {}]},
        {"tool": ["rules_write", {"part": "bass"}]},
        {"tool": ["rules_write", {"part": "chords"}]},
        {"tool": ["write", {"part": "drums", "section": "verse", "notes": "0 kick 1 110; 4 snare 1 100; 8 kick 1 100; 12 snare 1 100", "loop_bars": 1}]},
        {"tool": ["write", {"part": "kazoo", "section": "verse", "notes": "0 60 1 90"}]},
        {"tool": ["check", {}]},
        {"tool": ["finish", {"summary": "A short test song."}]},
        {"tool": ["finish", {"summary": "A short test song."}]},
        {"text": "Done."}
    ])");
    test::FakeProvider provider (replies);
    REQUIRE (provider.ready);
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    service.setEnabled (true);
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    context.workFolder = t.root.getChildFile ("song");
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setSize (900, 600);
    panel.setSeed (3);

    // Without a connection the AI can't be chosen, and Spinus writes as before.
    panel.refreshConnection();
    REQUIRE (waitFor ([&] { return panel.getInfoText().contains ("Options > AI"); }));
    panel.setAiWriter (true);
    CHECK_FALSE (panel.isAiWriter());

    juce::String id;
    service.call ("ai.v1.connections.save",
                  juce::JSON::parse (R"({"name": "My local model", "provider": "local", "baseUrl": ")" +
                                     provider.url() + R"("})"),
                  [&] (const juce::var& r, const juce::String&) { id = r["id"].toString(); });
    REQUIRE (waitFor ([&] { return id.isNotEmpty(); }));
    bool tested = false;
    service.call ("ai.v1.connections.test", juce::JSON::parse (R"({"id": ")" + id + R"("})"),
                  [&] (const juce::var&, const juce::String&) { tested = true; });
    REQUIRE (waitFor ([&] { return tested; }));

    // With one: the page names it, and says the tokens are shown as it works (no estimate before).
    panel.refreshConnection();
    REQUIRE (waitFor ([&] { return panel.getInfoText().contains ("My local model"); }));
    INFO (panel.getInfoText());
    CHECK (panel.getInfoText().contains ("shown as it works"));
    CHECK (panel.getInfoText().contains ("AI models can be unreliable"));
    CHECK (panel.getEffort() == "low"); // the default
    CHECK_FALSE (panel.getInfoText().contains ("About"));
    panel.setAiWriter (true);
    REQUIRE (panel.isAiWriter());
    panel.setEffort ("xhigh"); // where the model offers it
    CHECK (panel.getEffort() == "xhigh");
    panel.setEffort ("high");
    CHECK (panel.getEffort() == "high");
    CHECK (panel.getRounds() == 1);

    const auto before = session.getProject();
    REQUIRE (panel.write());
    CHECK (panel.isComposing());
    REQUIRE (waitFor ([&] { return ! panel.isBusy() && ! panel.isComposing(); }, 120000));
    INFO (panel.getStatusText());
    INFO (panel.getText());
    const auto& song = panel.getSong();
    REQUIRE (song["composed"].isObject());
    CHECK ((bool)song["composed"]["harness"]);
    CHECK (song["composed"]["reason"].toString() == "A short test song.");
    CHECK (song["composed"]["tools"].size() == 2); // Basic + arrangement, the default
    CHECK (song["note"].toString().contains ("Tools: Basic + arrangement."));
    CHECK (panel.getStatusText().contains ("Written by the AI (fake-model, reasoning high)"));
    CHECK (panel.getText().contains ("Tokens used"));
    CHECK (panel.getText().contains ("8 requests")); // the second finish ends it: no turn after
    // The drums as it wrote them, every bar of the verse; the lead (never written) by the rules.
    int verseBars = 0;
    for (const auto& section : *song["sections"].getArray())
        if (section["id"] == "verse")
            verseBars = (int)section["bars"];
    REQUIRE (verseBars > 0);
    CHECK (song["layerParts"]["drums"]["verse"].size() == 4 * verseBars);
    CHECK (song["layerParts"]["lead"].isObject());
    CHECK (session.getProject() == before); // nothing before Apply
    REQUIRE (panel.apply());
    CHECK (session.getHistory().getUndoDescriptions().size() == 1);
}

TEST_CASE ("Track creator: how deep the AI may go is a list with a Custom choice, remembered",
           "[ui][trackcreator][song]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    auto& settings = session.getEdit().engine.getPropertyStorage().getPropertiesFile();
    settings.removeValue ("trackCreatorAiTools");
    SongWriterPanel::Context context;
    {
        SongWriterPanel panel (session, [&context] { return context; });
        panel.setSize (900, 600);
        panel.setAiWriter (true);
        // Basic + arrangement at first, with the cost said below the list.
        CHECK (panel.getDepthChoice() == 2);
        CHECK (panel.getToolGroups() == juce::StringArray { "basic", "arrangement" });
        CHECK (panel.getDepthNoteText().contains ("costs more"));
        CHECK (panel.getDepthNoteText().contains ("several times the tokens of Basic"));
        // A list that is no preset is Custom; Basic stays on whatever is asked.
        panel.setToolGroups ({ "mix" });
        CHECK (panel.getDepthChoice() == 5);
        CHECK (panel.getToolGroups() == juce::StringArray { "basic", "mix" });
        panel.setToolGroups ({ "basic", "arrangement", "transitions", "mix" });
        CHECK (panel.getDepthChoice() == 4);
    }
    // Remembered once the user changes it on the page.
    {
        SongWriterPanel panel (session, [&context] { return context; });
        CHECK (panel.getDepthChoice() == 2); // setToolGroups (from code) is not the user's choice
    }
    settings.setValue ("trackCreatorAiTools", "basic,arrangement,transitions");
    {
        SongWriterPanel panel (session, [&context] { return context; });
        CHECK (panel.getDepthChoice() == 3);
    }
    settings.removeValue ("trackCreatorAiTools");
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
    context.parts = parts::suggestedParts ("full", context.summary);
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

TEST_CASE ("Track creator AI writer screenshot for the manual", "[.screenshot][manual]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    const auto replies = t.root.getChildFile ("replies.json");
    replies.replaceWithText (R"([
        {"tool": ["form", {}]},
        {"tool": ["write", {"part": "drums", "section": "verse", "notes": "0 kick 1 110; 4 snare 1 100; 8 kick 1 100; 10 kick 1 90; 12 snare 1 100", "loop_bars": 1}]},
        {"tool": ["copy", {"from_section": "verse", "to_section": "chorus", "parts": ["drums"]}]},
        {"tool": ["rules_write", {"part": "bass"}]},
        {"tool": ["rules_write", {"part": "chords"}]},
        {"tool": ["rules_write", {"part": "lead"}]},
        {"tool": ["check", {}]},
        {"tool": ["finish", {"summary": "A driving minor-key song: a busy kick under held chords, the lead answering the bass."}]}
    ])");
    test::FakeProvider provider (replies);
    REQUIRE (provider.ready);
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    service.setEnabled (true);
    juce::String id;
    service.call (
        "ai.v1.connections.save",
        juce::JSON::parse (R"({"name": "My AI", "provider": "local", "baseUrl": ")" + provider.url() + R"("})"),
        [&] (const juce::var& r, const juce::String&) { id = r["id"].toString(); });
    REQUIRE (waitFor ([&] { return id.isNotEmpty(); }));
    bool tested = false;
    service.call ("ai.v1.connections.test", juce::JSON::parse (R"({"id": ")" + id + R"("})"),
                  [&] (const juce::var&, const juce::String&) { tested = true; });
    REQUIRE (waitFor ([&] { return tested; }));
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    SpinusLookAndFeel look (Theme::dark());
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setLookAndFeel (&look);
    panel.setSize (860, 560);
    panel.setSeed (3);
    panel.refreshConnection();
    REQUIRE (waitFor ([&] { return panel.getInfoText().contains ("My AI"); }));
    panel.setToolGroups ({ "basic", "arrangement", "mix" }); // Custom: its checkboxes show
    panel.setAiWriter (true);
    REQUIRE (panel.write());
    REQUIRE (waitFor ([&] { return ! panel.isBusy() && ! panel.isComposing(); }, 120000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/track-creator-ai-writer.png");
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

TEST_CASE ("Track creator full arrangement Playlist screenshot for the manual", "[.screenshot][manual]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    ai::AiService service { t.root, t.root.getChildFile ("models") };
    service.setEnabled (true);
    SongWriterPanel::Context context;
    context.summary = testSummary();
    context.service = &service;
    SongWriterPanel panel (session, [&context] { return context; });
    panel.setSeed (3);
    context.parts = parts::suggestedParts ("full", context.summary);
    REQUIRE (panel.write());
    REQUIRE (waitFor ([&] { return ! panel.isBusy(); }, 120000));
    REQUIRE (panel.apply());

    ProjectDocument document (session, t.root.getChildFile ("settings"));
    MainView view (session, &document);
    view.setSize (1280, 800);
    REQUIRE (view.useLayout ("Arrange"));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    auto* playlist = dynamic_cast<Playlist*> (view.getWorkspace().getPanel ("playlist"));
    REQUIRE (playlist != nullptr);
    playlist->zoom (1.0 / 2.5, 0.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    auto* frame = playlist->getParentComponent();
    REQUIRE (frame != nullptr);
    const auto image = view.createComponentSnapshot (view.getLocalArea (frame, frame->getLocalBounds()), true, 1.0f);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/track-creator-full.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
