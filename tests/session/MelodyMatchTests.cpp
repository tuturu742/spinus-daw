#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "session/MelodyMatch.h"

using namespace spinus;
using namespace spinus::model;
using Catch::Approx;

TEST_CASE ("A melody over a vocal clip: the channel's notes while the clip plays, in seconds into its file",
           "[tune][session]")
{
    Project p;
    p.settings.tempoBpm = 120.0; // 960 ticks = half a second
    Channel lead;
    lead.id = p.allocateId<ChannelId>();
    lead.name = "Melody";
    Channel other;
    other.id = p.allocateId<ChannelId>();
    p.channels = { lead, other };
    Lane lane { p.allocateId<LaneId>(), "Lane" };
    p.playlist.lanes = { lane };

    Pattern tune;
    tune.id = p.allocateId<PatternId>();
    tune.notes[lead.id] = { { 0, 960, 60 }, { 1920, 480, 62 } };
    tune.notes[other.id] = { { 0, 960, 40 } };
    Pattern before; // a note that ends where the vocal starts
    before.id = p.allocateId<PatternId>();
    before.notes[lead.id] = { { 3000, 840, 65 } };
    p.patterns = { tune, before };

    AudioClip vocal;
    vocal.file = "vocal.wav";
    vocal.onTempoChange = "time";
    const auto vocalId = p.allocateId<ClipId>();
    p.playlist.clips.push_back ({ vocalId, lane.id, 3840, 7680, false, vocal });
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane.id, 3840, 3840, false, PatternClip { tune.id, 0 } });
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane.id, 0, 3840, false, PatternClip { before.id, 0 } });
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane.id, 7680, 3840, true, PatternClip { tune.id, 0 } });

    const auto melody = MelodyMatch::melodyFor (p, vocalId, lead.id);
    REQUIRE (melody.size() == 2); // not the muted clip's, nor the one before the vocal
    CHECK (melody[0].start == Approx (0.0));
    CHECK (melody[0].end == Approx (0.5));
    CHECK (melody[0].pitch == 60);
    CHECK (melody[1].start == Approx (1.0));
    CHECK (melody[1].end == Approx (1.25));

    // Its offset counts: the clip starting a second into its file.
    std::get<AudioClip> (p.playlist.clips[0].content).offset = 1920;
    CHECK (MelodyMatch::melodyFor (p, vocalId, lead.id)[0].start == Approx (1.0));

    CHECK (MelodyMatch::melodyFor (p, vocalId, ChannelId {}).empty());
    CHECK (MelodyMatch::melodyFor (p, p.playlist.clips[1].id, lead.id).empty()); // not an audio clip
}

TEST_CASE ("Matching sung notes to a melody: pitch in the octave sung, timing, and what is left alone",
           "[tune][session]")
{
    const std::vector<MelodyMatch::Note> melody { { 0.0, 0.5, 60 }, { 1.0, 1.25, 62 } };
    const std::vector<TuneNote> sung { { 0.05, 0.45, 59.6f }, { 1.1, 1.3, 74.2f }, { 2.0, 2.3, 64.0f } };

    auto result = MelodyMatch::match (sung, melody, true, {}, 3.0);
    CHECK (result.matched == 2);
    CHECK (result.timed == 2);
    CHECK (result.unmatched == std::vector<int> { 2 });
    const auto& n = result.notes;
    CHECK (n[0].pitch + n[0].shift == Approx (60.0f));
    CHECK (n[1].pitch + n[1].shift == Approx (74.0f)); // the melody's D, an octave up as sung
    CHECK (n[0].outputStart() == Approx (0.0));
    CHECK (n[0].outputEnd() == Approx (0.5));
    CHECK (n[1].outputStart() == Approx (1.0));
    CHECK (n[1].outputEnd() == Approx (1.25));
    CHECK (n[2] == sung[2]);

    // Pitch only.
    result = MelodyMatch::match (sung, melody, false, {}, 3.0);
    CHECK (result.notes[0].pitch + result.notes[0].shift == Approx (60.0f));
    CHECK (! result.notes[0].isMovedInTime());
    CHECK (result.timed == 0);

    // Only the notes asked for.
    result = MelodyMatch::match (sung, melody, true, { 1 }, 3.0);
    CHECK (result.matched == 1);
    CHECK (result.notes[0] == sung[0]);
    CHECK (result.notes[1].outputStart() == Approx (1.0));

    // Two syllables on one melody note: both on its pitch, neither moved.
    result = MelodyMatch::match ({ { 0.0, 0.2, 60.3f }, { 0.25, 0.5, 59.8f } }, melody, true, {}, 3.0);
    CHECK (result.matched == 2);
    CHECK (result.timed == 0);
    for (auto& note : result.notes)
    {
        CHECK (note.pitch + note.shift == Approx (60.0f));
        CHECK (! note.isMovedInTime());
    }

    // Placed one after another, never overlapping: the first made longer up to where the second now starts.
    result = MelodyMatch::match ({ { 0.0, 0.4, 60.0f }, { 0.45, 0.8, 62.0f } }, { { 0.0, 0.6, 60 }, { 0.6, 0.9, 62 } },
                                 true, {}, 3.0);
    CHECK (result.timed == 2);
    CHECK (result.notes[0].outputEnd() == Approx (0.6));
    CHECK (result.notes[1].outputStart() == Approx (0.6));
    CHECK (result.notes[1].outputEnd() == Approx (0.9));

    // A melody note far longer than the sung note: at most 4 times as long.
    result = MelodyMatch::match ({ { 0.0, 0.1, 60.0f } }, { { 0.0, 2.0, 60 } }, true, {}, 3.0);
    CHECK (result.notes[0].stretch == Approx (4.0));
}
