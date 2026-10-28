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

namespace
{
/** makeSong with its parts as layers: the drums on one channel, a pad in the chorus only, a riser before the
    chorus and an impact where it starts. `bassPitch` makes another bass. */
juce::var makeFullSong (int bassPitch = 45)
{
    auto song = makeSong (bassPitch);
    const auto extra = juce::JSON::parse (juce::String (R"({
        "size": "full",
        "layers": [
            {"id": "drums", "name": "Drums", "family": "Drums", "part": "drums", "role": "drums",
             "soundRole": "drums", "drums": true, "instance": 0},
            {"id": "bass", "name": "Bass", "family": "Bass", "part": "bass", "role": "bass", "soundRole": "bass",
             "drums": false, "instance": 0, "register": [31, 38, 47]},
            {"id": "pad", "name": "Pad", "family": "Synths", "part": "chords", "role": "pad", "soundRole": "pad",
             "drums": false, "instance": 0, "register": [52, 61, 73]},
            {"id": "lead", "name": "Lead", "family": "Leads", "part": "lead", "role": "lead", "soundRole": "lead",
             "drums": false, "instance": 0},
            {"id": "riser", "name": "Riser", "family": "FX", "part": "fx", "role": "riser", "soundRole": "fx",
             "drums": false, "instance": 0},
            {"id": "impact", "name": "Impacts", "family": "FX", "part": "fx", "role": "impact", "soundRole": "drums",
             "drums": true, "instance": 0}],
        "layerParts": {
            "drums": {"verse": [{"start": 0, "length": 120, "drum": "kick", "velocity": 0.9},
                                {"start": 960, "length": 120, "drum": "snare", "velocity": 0.8},
                                {"start": 480, "length": 120, "drum": "hat", "velocity": 0.5}],
                      "chorus": [{"start": 0, "length": 120, "drum": "kick", "velocity": 0.9},
                                 {"start": 0, "length": 120, "drum": "crash", "velocity": 0.8}]},
            "bass": {"verse": [{"start": 0, "length": 900, "pitch": BASS, "velocity": 0.8}],
                     "chorus": [{"start": 0, "length": 1800, "pitch": 48, "velocity": 0.8}]},
            "pad": {"chorus": [{"start": 0, "length": 7000, "pitch": 60, "velocity": 0.5},
                               {"start": 0, "length": 7000, "pitch": 64, "velocity": 0.5}]},
            "lead": {"chorus": [{"start": 960, "length": 480, "pitch": 64, "velocity": 0.7}]}},
        "fx": {
            "riser": {"bars": 2, "at": [0], "notes": [{"start": 0, "length": 7400, "pitch": 69, "velocity": 0.8}]},
            "impact": {"bars": 1, "at": [2], "notes": [{"start": 0, "length": 120, "drum": "kick", "velocity": 1.0},
                                                       {"start": 0, "length": 120, "drum": "crash", "velocity": 1.0}]}}
    })")
                                              .replace ("BASS", juce::String (bassPitch)));
    for (const auto& p : extra.getDynamicObject()->getProperties())
        song.getDynamicObject()->setProperty (p.name, p.value);
    return song;
}

const model::Channel* channelNamed (const model::Project& project, const juce::String& name)
{
    for (const auto& c : project.channels)
        if (c.name == name)
            return &c;
    return nullptr;
}

const model::Lane* laneNamed (const model::Project& project, const juce::String& name)
{
    for (const auto& l : project.playlist.lanes)
        if (l.name == name)
            return &l;
    return nullptr;
}

std::vector<model::Ticks> clipStartsOn (const model::Project& project, const model::Lane& lane)
{
    std::vector<model::Ticks> starts;
    for (const auto& clip : project.playlist.clips)
        if (clip.lane == lane.id)
            starts.push_back (clip.start);
    std::sort (starts.begin(), starts.end());
    return starts;
}
} // namespace

TEST_CASE ("Song writer: a full arrangement is laid out by family, its layers coming and going, one undo step",
           "[song]")
{
    commands::CommandHistory history { model::Project {} };
    const auto before = history.getProject();
    const auto song = makeFullSong();
    REQUIRE (song::isFull (song));
    REQUIRE (song::layersOf (song).size() == 6);
    song::Options options;
    options.name = song::nextSongName (history.getProject());
    juce::String error;
    const auto json = song::makeProposal (song, history.getProject(), options, &error);
    INFO (error);
    REQUIRE (json.isObject());
    auto proposal = ai::Proposal::prepare (json, history);
    INFO (proposal.error);
    REQUIRE (proposal.canApply());
    CHECK (sameContent (history.getProject(), before));
    proposal.undoName = "Track creator: " + proposal.summary;
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();

    // A channel per layer, coloured and grouped by family, on an insert of its own named and coloured alike.
    CHECK (project.channels.size() == before.channels.size() + 6);
    for (const auto& layer : song::layersOf (song))
    {
        const auto* c = channelNamed (project, layer.name);
        INFO (layer.name);
        REQUIRE (c != nullptr);
        CHECK (c->group == layer.family);
        CHECK (c->colour == (juce::uint32)song::familyColour (layer.family).substring (1).getHexValue64());
        const auto* insert = project.findInsert (c->targetInsert);
        REQUIRE (insert != nullptr);
        CHECK (insert->name == layer.name);
        REQUIRE (c->instrument.has_value());
        CHECK (c->instrument.value().stockType == (layer.drums ? "spinus.drum-machine" : "spinus.3x-osc"));
    }
    // A bus for each family of two or more channels (FX), which their inserts play into.
    juce::StringArray buses;
    for (const auto& insert : project.mixer.inserts)
        if (std::none_of (project.channels.begin(), project.channels.end(),
                          [&] (const model::Channel& c) { return c.targetInsert == insert.id; }))
            buses.add (insert.name);
    buses.removeString ("Master");
    CHECK (buses == juce::StringArray { "FX" });
    const auto* riserInsert = project.findInsert (channelNamed (project, "Riser")->targetInsert);
    REQUIRE (riserInsert != nullptr);
    const auto* fxBus = project.findInsert (riserInsert->output);
    REQUIRE (fxBus != nullptr);
    CHECK (fxBus->name == "FX");

    // Patterns only where a layer plays: the pad in the chorus, not the verse; one pattern per effect.
    CHECK (patternNamed (project, "Song 1: Verse - Drums") != nullptr);
    CHECK (patternNamed (project, "Song 1: Chorus - Pad") != nullptr);
    CHECK (patternNamed (project, "Song 1: Verse - Pad") == nullptr);
    CHECK (patternNamed (project, "Song 1: Verse - Lead") == nullptr);
    const auto* riser = patternNamed (project, "Song 1: Riser");
    REQUIRE (riser != nullptr);
    CHECK (riser->length == 2 * 3840);
    // Kick, snare and hats on the one drums channel (the Drum Machine's pads).
    const auto* drumsVerse = patternNamed (project, "Song 1: Verse - Drums");
    CHECK (pitchesOf (*drumsVerse, channelNamed (project, "Drums")->id) == std::vector<int> { 60, 61, 63 });

    // A folder track per family, a track per layer under it, the clips where the layer plays.
    const auto* drums = laneNamed (project, "Song 1: Drums");
    const auto* kick = laneNamed (project, "Drums");
    const auto* pad = laneNamed (project, "Pad");
    const auto* riserLane = laneNamed (project, "Riser");
    const auto* impacts = laneNamed (project, "Impacts");
    REQUIRE (drums != nullptr);
    REQUIRE (kick != nullptr);
    REQUIRE (pad != nullptr);
    REQUIRE (riserLane != nullptr);
    REQUIRE (impacts != nullptr);
    CHECK (kick->group == drums->id);
    CHECK (pad->group == laneNamed (project, "Song 1: Synths")->id);
    CHECK (clipStartsOn (project, *kick) == std::vector<model::Ticks> { 0, 7680, 15360 });
    CHECK (clipStartsOn (project, *pad) == std::vector<model::Ticks> { 7680 });
    CHECK (clipStartsOn (project, *riserLane) == std::vector<model::Ticks> { 0 });
    CHECK (clipStartsOn (project, *impacts) == std::vector<model::Ticks> { 7680 });
    CHECK (clipStartsOn (project, *drums).empty());
    CHECK (project.playlist.markers.size() == 3);

    CHECK (history.getUndoDescriptions().size() == 1);
    CHECK (history.getUndoDescriptions()[0].contains ("6 channels"));

    // What it made is found again, and a new bass is written into the bass layer only.
    const auto applied = song::findApplied (song, project, "Song 1");
    REQUIRE (applied.full);
    REQUIRE (applied.isIn (project));
    CHECK (applied.layerPatterns.at ("drums").size() == 2);
    CHECK (applied.layerPatterns.at ("pad").size() == 1);
    CHECK (applied.layerChannels.at ("drums") == channelNamed (project, "Drums")->id);
    const auto after = history.getProject();
    const auto redo = song::makeRedoProposal (makeFullSong (40), applied, "bass", {}, &error);
    INFO (error);
    REQUIRE (redo.isObject());
    auto bass = ai::Proposal::prepare (redo, history);
    INFO (bass.error);
    REQUIRE (bass.apply (history).wasOk());
    CHECK (pitchesOf (*history.getProject().findPattern (applied.layerPatterns.at ("bass").at ("verse")),
                      applied.layerChannels.at ("bass")) == std::vector<int> { 40 });
    CHECK (*history.getProject().findPattern (applied.layerPatterns.at ("drums").at ("verse")) ==
           *after.findPattern (applied.layerPatterns.at ("drums").at ("verse")));
    CHECK (history.getProject().playlist == after.playlist);

    REQUIRE (history.undo());
    REQUIRE (history.undo());
    CHECK (sameContent (history.getProject(), before));
    CHECK_FALSE (applied.isIn (history.getProject()));
}

TEST_CASE ("Song writer: writing again replaces the song before, in its place, as one undo step", "[song]")
{
    commands::CommandHistory history { model::Project {} };
    const auto before = history.getProject();
    song::Options options;
    options.name = "Song 1";
    REQUIRE (ai::Proposal::prepare (song::makeProposal (makeFullSong(), history.getProject(), options), history)
                 .apply (history)
                 .wasOk());
    const auto first = history.getProject();
    CHECK (song::lastSongName (first) == "Song 1");

    // Replaced: its channels, inserts, bus, patterns, tracks and markers go; the new one takes its place.
    song::Options again;
    again.name = "Song 1";
    again.replace = "Song 1";
    model::Ticks start = 0;
    CHECK_FALSE (song::removeSongSteps (first, "Song 1", start).isEmpty());
    CHECK (start == 0);
    juce::String error;
    const auto json = song::makeProposal (makeFullSong (40), first, again, &error);
    INFO (error);
    auto proposal = ai::Proposal::prepare (json, history);
    INFO (proposal.error);
    REQUIRE (proposal.canApply());
    CHECK (proposal.summary.contains ("replacing Song 1"));
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();
    CHECK (project.channels.size() == first.channels.size());
    CHECK (project.patterns.size() == first.patterns.size());
    CHECK (project.playlist.lanes.size() == first.playlist.lanes.size());
    CHECK (project.playlist.markers.size() == first.playlist.markers.size());
    CHECK (project.mixer.inserts.size() == first.mixer.inserts.size());
    CHECK (clipStartsOn (project, *laneNamed (project, "Drums")) == std::vector<model::Ticks> { 0, 7680, 15360 });
    const auto applied = song::findApplied (makeFullSong (40), project, "Song 1");
    CHECK (pitchesOf (*project.findPattern (applied.layerPatterns.at ("bass").at ("verse")),
                      applied.layerChannels.at ("bass")) == std::vector<int> { 40 });
    CHECK (history.getUndoDescriptions().size() == 2);
    REQUIRE (history.undo());
    CHECK (sameContent (history.getProject(), first)); // Ctrl+Z brings it back
    REQUIRE (history.undo());
    CHECK (sameContent (history.getProject(), before));
}

TEST_CASE ("Song writer: the Sounds page's channels are written into, by the part's name", "[song]")
{
    commands::CommandHistory history { model::Project {} };
    // As the Sounds page adds them: "<part> (Track creator)".
    auto add = ai::Proposal::prepare (juce::JSON::parse (R"json({"summary": "x", "steps": [
            {"command": "channel.add", "parameters": {"name": "Bass (Track creator)",
             "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}},
            {"command": "channel.add", "parameters": {"name": "Drums (Track creator)",
             "instrument": {"kind": "stock", "stockType": "spinus.drum-machine"}}}]})json"),
                                      history);
    REQUIRE (add.apply (history).wasOk());
    const auto written = makeFullSong();
    const auto channels = song::findLayerChannels (history.getProject(), song::layersOf (written));
    REQUIRE (channels.size() == 2);
    song::Options options;
    options.channels = channels;
    auto proposal = ai::Proposal::prepare (song::makeProposal (written, history.getProject(), options), history);
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();
    CHECK (project.channels.size() == 2 + 4); // pad, lead, riser and impacts are new
    const auto* bass = channelNamed (project, "Bass (Track creator)");
    REQUIRE (bass != nullptr);
    CHECK (pitchesOf (*patternNamed (project, "Song 1: Verse - Bass"), bass->id) == std::vector<int> { 45 });
    // Replacing the song keeps them: they are the user's choice.
    model::Ticks start = 0;
    const auto steps = song::removeSongSteps (project, "Song 1", start);
    for (const auto& step : steps)
        if (step["command"].toString() == "channel.remove")
            CHECK ((int)step["parameters"]["channel"] != (int)bass->id.value);
}

TEST_CASE ("Song writer: each part starts at its role's level, from how loud its sound played", "[song]")
{
    const auto song = makeFullSong();
    const auto layers = song::layersOf (song);
    const auto layerNamed = [&] (const juce::String& id)
    { return *std::find_if (layers.begin(), layers.end(), [&] (const song::Layer& l) { return l.id == id; }); };

    // The busiest section's notes, in seconds, the drums on the kit's notes.
    const auto drums = song::layerExcerpt (song, layerNamed ("drums"), { { "kick", 36 }, { "snare", 38 } });
    REQUIRE (drums.size() == 2); // the verse (three hits; the hat has no note on this kit)
    const auto secondsPerBeat = 60.0 / (double)song["tempo"];
    CHECK (drums[0].note == 36);
    CHECK (drums[1].start == Catch::Approx (secondsPerBeat * 960.0 / ((double)song["barTicks"] / 4.0)));
    CHECK (song::layerExcerpt (song, layerNamed ("lead"), {}).size() == 1);
    CHECK (song::layerExcerpt (song, layerNamed ("lead"), {}, 0.1).empty()); // its note starts later

    // A pad that played 12 dB over the drums is brought under them, to its role's level; a quiet bass is up to
    // its own. Nothing is turned up: the loudest is at 0 dB.
    const auto faders = song::startingFaders (layers, { { "drums", -20.0 }, { "bass", -22.0 }, { "pad", -8.0 } });
    REQUIRE (faders.size() == 3);
    const auto drumsLevel = song::roleLevelDb ("drums");
    CHECK (faders.at ("pad") - faders.at ("drums") == Catch::Approx (song::roleLevelDb ("pad") - drumsLevel - 12.0));
    CHECK (faders.at ("bass") - faders.at ("drums") == Catch::Approx (song::roleLevelDb ("bass") - drumsLevel + 2.0));
    double loudest = -100.0;
    for (const auto& [id, db] : faders)
    {
        CHECK ((db <= 0.0 && db >= -30.0));
        loudest = std::max (loudest, db);
    }
    CHECK (loudest == 0.0);
    CHECK (song::roleLevelDb ("pad") < song::roleLevelDb ("bass"));
    CHECK (song::startingFaders (layers, { { "drums", -100.0 } }).empty()); // silent: not measured

    // Apply sets them on the layers' inserts, in the same one undo step.
    commands::CommandHistory history { model::Project {} };
    song::Options options;
    options.layerSounds["pad"] = song::defaultSound (layerNamed ("pad"));
    options.layerSounds["pad"].volumeDb = faders.at ("pad");
    auto proposal = ai::Proposal::prepare (song::makeProposal (song, history.getProject(), options), history);
    REQUIRE (proposal.canApply());
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();
    CHECK (project.findInsert (channelNamed (project, "Pad")->targetInsert)->volumeDb ==
           Catch::Approx (faders.at ("pad")).margin (0.01));
    CHECK (project.findInsert (channelNamed (project, "Bass")->targetInsert)->volumeDb == Catch::Approx (0.0));
}
