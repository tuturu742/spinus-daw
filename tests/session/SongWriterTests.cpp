#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/Proposal.h"
#include "commands/CommandHistory.h"
#include "session/PartSounds.h"
#include "session/SongWriter.h"

using namespace spinus;
using Catch::Approx;

/* The Track creator's new song into the project (session/SongWriter): one proposal, one undo step, nothing
   changed before Apply; one part or section written again into what the song made, the rest kept. The song is
   made up here as spinus_ai.songwriter writes them (its own tests are in ai/tests/test_songwriter.py). */

namespace
{
/** A song of two sections (2 bars each), verse chorus verse, every part with a few notes. `bassPitch` makes
    another bass, `chorusKick` other chorus drums. */
juce::var makeSong (int bassPitch = 45, int chorusKick = 0)
{
    auto json = juce::String (R"({
        "kind": "spinus.song", "version": 1, "tempo": 126.0, "beatsPerBar": 4, "stepsPerBar": 16, "barTicks": 3840,
        "swing": 0.6, "patternSwing": 0.4, "bars": 6,
        "key": {"tonic": 9, "scale": "minor", "name": "A minor"},
        "sections": [
            {"id": "verse", "kind": "verse", "name": "Verse", "bars": 2, "loop": ["i", "bVI"],
             "chords": [{"bar": 0, "roman": "i", "name": "Am", "tones": [9, 0, 4]},
                        {"bar": 1, "roman": "bVI", "name": "F", "tones": [5, 9, 0]}]},
            {"id": "chorus", "kind": "chorus", "name": "Chorus", "bars": 2, "loop": ["bIII", "bVII"],
             "chords": [{"bar": 0, "roman": "bIII", "name": "C", "tones": [0, 4, 7]},
                        {"bar": 1, "roman": "bVII", "name": "G", "tones": [7, 11, 2]}]}],
        "arrangement": [{"section": "verse", "bar": 0}, {"section": "chorus", "bar": 2}, {"section": "verse", "bar": 4}],
        "parts": {
            "drums": {"verse": [{"start": 0, "length": 120, "drum": "kick", "velocity": 0.9},
                                {"start": 960, "length": 120, "drum": "snare", "velocity": 0.8},
                                {"start": 480, "length": 120, "drum": "hat", "velocity": 0.5},
                                {"start": 1440, "length": 120, "drum": "cowbell", "velocity": 0.5}],
                      "chorus": [{"start": CHORUSKICK, "length": 120, "drum": "kick", "velocity": 0.9},
                                 {"start": 0, "length": 120, "drum": "crash", "velocity": 0.8}]},
            "bass": {"verse": [{"start": 0, "length": 900, "pitch": BASS, "velocity": 0.8},
                               {"start": 3840, "length": 900, "pitch": 41, "velocity": 0.8}],
                     "chorus": [{"start": 0, "length": 1800, "pitch": 48, "velocity": 0.8}]},
            "chords": {"verse": [{"start": 0, "length": 3600, "pitch": 57, "velocity": 0.6},
                                 {"start": 0, "length": 3600, "pitch": 60, "velocity": 0.6}],
                       "chorus": [{"start": 0, "length": 3600, "pitch": 60, "velocity": 0.6}]},
            "lead": {"verse": [], "chorus": [{"start": 960, "length": 480, "pitch": 64, "velocity": 0.7}]}},
        "plain": ["A new song of 6 bars at 126 BPM in A minor."]
    })");
    return juce::JSON::parse (
        json.replace ("BASS", juce::String (bassPitch)).replace ("CHORUSKICK", juce::String (chorusKick)));
}

const model::Pattern* patternNamed (const model::Project& project, const juce::String& name)
{
    for (const auto& p : project.patterns)
        if (p.name == name)
            return &p;
    return nullptr;
}

/** The same project, apart from the next ID to hand out (IDs are never used again, undone or not). */
bool sameContent (const model::Project& a, const model::Project& b)
{
    return a.settings == b.settings && a.channels == b.channels && a.patterns == b.patterns &&
           a.playlist == b.playlist && a.mixer == b.mixer && a.launcher == b.launcher;
}

std::vector<int> pitchesOf (const model::Pattern& pattern, model::ChannelId channel)
{
    std::vector<int> pitches;
    if (const auto found = pattern.notes.find (channel); found != pattern.notes.end())
        for (const auto& n : found->second)
            pitches.push_back (n.pitch);
    std::sort (pitches.begin(), pitches.end());
    return pitches;
}
} // namespace

TEST_CASE ("Song writer: the parts' channels are named as the sounds for each part names them", "[song]")
{
    CHECK (song::channelName ("bass") == "Bass (Track creator)");
    CHECK (song::channelName ("chords") == "Chords and melody (Track creator)");
    CHECK (song::channelName ("lead") == "Lead (Track creator)");
    CHECK (song::channelName ("drums") == "Drums (Track creator)");
    for (const auto& part : song::partNames())
        CHECK (song::channelName (part) ==
               parts::displayName (song::summaryPart (part)).upToFirstOccurrenceOf (" (", false, false) +
                   " (Track creator)");
    model::Channel drumMachine;
    drumMachine.instrument = model::Device {};
    drumMachine.instrument->stockType = "spinus.drum-machine";
    CHECK (song::drumNotesFor (drumMachine).at ("snare") == 61);
    CHECK (song::drumNotesFor (model::Channel {}).at ("snare") == 38); // General MIDI for any other
}

TEST_CASE ("Song writer: the whole song is one proposal, nothing changes before Apply, one undo step", "[song]")
{
    commands::CommandHistory history { model::Project {} };
    const auto before = history.getProject();
    const auto song = makeSong();
    song::Options options;
    options.name = song::nextSongName (history.getProject());
    CHECK (options.name == "Song 1");
    juce::String error;
    const auto json = song::makeProposal (song, history.getProject(), options, &error);
    INFO (error);
    REQUIRE (json.isObject());
    auto proposal = ai::Proposal::prepare (json, history);
    INFO (proposal.error);
    REQUIRE (proposal.canApply());

    // Not applied: the project is as it was; the preview has the song (to listen to).
    CHECK (sameContent (history.getProject(), before));
    CHECK_FALSE (history.canUndo());
    CHECK (proposal.preview.patterns.size() == before.patterns.size() + 2);
    CHECK (proposal.preview.settings.tempoBpm == Approx (126.0));

    proposal.undoName = "Track creator: " + proposal.summary;
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();
    CHECK (project.settings.tempoBpm == Approx (126.0));
    REQUIRE (project.settings.key.has_value());
    CHECK (project.settings.key.value().tonic == 9);
    CHECK (project.settings.key.value().scale == "minor");

    const auto channels = song::findChannels (project);
    REQUIRE (channels.size() == 4);
    for (const auto& [part, id] : channels)
    {
        const auto* c = project.findChannel (id);
        REQUIRE (c != nullptr);
        CHECK (c->group == "Track creator");
        REQUIRE (c->instrument.has_value());
        CHECK (c->instrument.value().stockType == (part == "drums" ? "spinus.drum-machine" : "spinus.3x-osc"));
    }

    const auto* verse = patternNamed (project, "Song 1: Verse");
    const auto* chorus = patternNamed (project, "Song 1: Chorus");
    REQUIRE (verse != nullptr);
    REQUIRE (chorus != nullptr);
    CHECK (verse->length == 2 * 3840);
    CHECK (chorus->length == 2 * 3840);
    CHECK (verse->swing == Approx (0.4f));
    // Drums on the Drum Machine's pads; a drum it does not know is left out.
    CHECK (pitchesOf (*verse, channels.at ("drums")) == std::vector<int> { 60, 61, 63 });
    CHECK (pitchesOf (*chorus, channels.at ("drums")) == std::vector<int> { 60, 70 });
    CHECK (pitchesOf (*verse, channels.at ("bass")) == std::vector<int> { 41, 45 });
    CHECK (pitchesOf (*verse, channels.at ("chords")) == std::vector<int> { 57, 60 });
    CHECK (pitchesOf (*chorus, channels.at ("lead")) == std::vector<int> { 64 });
    CHECK (pitchesOf (*verse, channels.at ("lead")).empty());

    // The Playlist plays verse, chorus, verse on a track of its own, with markers.
    REQUIRE (project.playlist.lanes.size() == before.playlist.lanes.size() + 1);
    CHECK (project.playlist.lanes.back().name == "Track creator: Song 1");
    std::vector<std::pair<model::Ticks, model::PatternId>> clips;
    for (const auto& clip : project.playlist.clips)
        if (const auto* p = std::get_if<model::PatternClip> (&clip.content))
            clips.emplace_back (clip.start, p->pattern);
    std::sort (clips.begin(), clips.end());
    REQUIRE (clips.size() == 3);
    CHECK (clips[0] == std::make_pair (model::Ticks { 0 }, verse->id));
    CHECK (clips[1] == std::make_pair (model::Ticks { 7680 }, chorus->id));
    CHECK (clips[2] == std::make_pair (model::Ticks { 15360 }, verse->id));
    REQUIRE (project.playlist.markers.size() == 3);
    CHECK (project.playlist.markers[1].name == "Chorus");

    // One undo step takes it all away.
    CHECK (history.getUndoDescriptions().size() == 1);
    CHECK (history.getUndoDescriptions()[0].startsWith ("Track creator: a new song"));
    REQUIRE (history.undo());
    CHECK (sameContent (history.getProject(), before));

    // A second song is named after the first.
    REQUIRE (history.redo());
    CHECK (song::nextSongName (history.getProject()) == "Song 2");
}

TEST_CASE ("Song writer: the sounds for each part's channels are written into, the missing ones made", "[song]")
{
    commands::CommandHistory history { model::Project {} };
    // As milestone 3 adds them: a bass, and drums on an instrument that plays General MIDI drums.
    for (const auto* part : { "bass", "drums" })
    {
        auto add = juce::JSON::parse (
            R"({"summary": "s", "steps": [{"command": "channel.add", "parameters": {"name": ")" +
            song::channelName (part) + R"(", "instrument": {"kind": "stock", "stockType": "spinus.sampler"}}}]})");
        auto p = ai::Proposal::prepare (add, history);
        REQUIRE (p.apply (history).wasOk());
    }
    // And a drum loop on a track of its own (a loop chosen for the drums plays as audio).
    auto loop = ai::Proposal::prepare (
        juce::JSON::parse (
            R"j({"summary": "s", "steps": [{"command": "lane.add", "parameters": {"name": "Drums (Track creator)"}}]})j"),
        history);
    REQUIRE (loop.apply (history).wasOk());
    const auto existing = song::findChannels (history.getProject());
    REQUIRE (existing.size() == 2);
    const auto count = history.getProject().channels.size();

    song::Options options;
    options.channels = existing;
    options.sounds["lead"] = { juce::JSON::parse (R"({"kind": "stock", "stockType": "spinus.sampler"})"), {}, {} };
    auto proposal = ai::Proposal::prepare (song::makeProposal (makeSong(), history.getProject(), options), history);
    INFO (proposal.error);
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();
    CHECK (project.channels.size() == count + 2); // chords and lead
    const auto channels = song::findChannels (project);
    CHECK (channels.at ("bass") == existing.at ("bass"));
    CHECK (project.findChannel (channels.at ("lead"))->instrument.value().stockType == "spinus.sampler"); // as chosen
    const auto* verse = patternNamed (project, "Song 1: Verse");
    REQUIRE (verse != nullptr);
    CHECK (pitchesOf (*verse, existing.at ("drums")) == std::vector<int> { 36, 38, 42 }); // General MIDI
    CHECK (pitchesOf (*verse, existing.at ("bass")) == std::vector<int> { 41, 45 });
    CHECK (project.playlist.lanes.front().name == "Drums (Track creator)");
    CHECK (project.playlist.lanes.front().muted); // the song's drums instead
}

TEST_CASE ("Song writer: one part or one section written again, the rest kept, one undo step each", "[song]")
{
    commands::CommandHistory history { model::Project {} };
    const auto song = makeSong();
    song::Options options;
    auto proposal = ai::Proposal::prepare (song::makeProposal (song, history.getProject(), options), history);
    REQUIRE (proposal.apply (history).wasOk());
    const auto applied = song::findApplied (song, history.getProject(), "Song 1");
    REQUIRE (applied.isIn (history.getProject()));
    REQUIRE (applied.patterns.size() == 2);
    REQUIRE (applied.channels.size() == 4);
    const auto after = history.getProject();

    // A new bass: only the bass notes change, in every section.
    juce::String error;
    const auto redo = song::makeRedoProposal (makeSong (40), applied, "bass", {}, &error);
    INFO (error);
    REQUIRE (redo.isObject());
    CHECK (redo["summary"].toString() == "new bass");
    auto bass = ai::Proposal::prepare (redo, history);
    INFO (bass.error);
    bass.undoName = "Track creator: new bass";
    REQUIRE (bass.apply (history).wasOk());
    const auto& project = history.getProject();
    const auto* verse = project.findPattern (applied.patterns.at ("verse"));
    REQUIRE (verse != nullptr);
    CHECK (pitchesOf (*verse, applied.channels.at ("bass")) == std::vector<int> { 40, 41 });
    for (const auto* part : { "drums", "chords", "lead" })
        for (const auto& [section, id] : applied.patterns)
            CHECK (pitchesOf (*project.findPattern (id), applied.channels.at (part)) ==
                   pitchesOf (*after.findPattern (id), applied.channels.at (part)));
    CHECK (project.channels == after.channels);
    CHECK (project.playlist == after.playlist);
    CHECK (history.getUndoDescriptions().size() == 2);
    REQUIRE (history.undo());
    CHECK (sameContent (history.getProject(), after));

    // The chorus again: only the chorus pattern changes.
    auto chorus = ai::Proposal::prepare (song::makeRedoProposal (makeSong (45, 1920), applied, {}, "chorus"), history);
    REQUIRE (chorus.apply (history).wasOk());
    CHECK (*history.getProject().findPattern (applied.patterns.at ("verse")) ==
           *after.findPattern (applied.patterns.at ("verse")));
    const auto& kicks =
        history.getProject().findPattern (applied.patterns.at ("chorus"))->notes.at (applied.channels.at ("drums"));
    CHECK (std::any_of (kicks.begin(), kicks.end(),
                        [] (const model::Note& n) { return n.start == 1920 && n.pitch == 60; }));

    // Once the song is undone, a part cannot be written into it: it says so.
    REQUIRE (history.undo());
    REQUIRE (history.undo());
    CHECK_FALSE (applied.isIn (history.getProject()));
    const auto gone = song::makeRedoProposal (song, applied, "bass", {}, &error);
    auto tried = ai::Proposal::prepare (gone, history);
    CHECK_FALSE (tried.canApply());
    CHECK_FALSE (song::makeRedoProposal (song, applied, "cowbell", {}, &error).isObject());
    CHECK (error.contains ("cowbell"));
    CHECK_FALSE (
        song::makeProposal (juce::JSON::parse (R"({"kind": "other"})"), history.getProject(), {}, &error).isObject());
    CHECK (error.contains ("not a song"));
}
