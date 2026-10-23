#pragma once

#include "model/Model.h"

namespace spinus::test
{

using namespace spinus::model;

/** A small project that uses every kind of model object. tests/model/files/example-v1.json is this
    project as written by schema version 1; keep them in sync. */
inline Project makeExample()
{
    Project p;
    p.settings.tempoBpm = 140.0;
    p.settings.timeSignature = { 3, 4 };
    p.settings.key = Key { 9, "minor" };
    p.settings.title = "Example";
    p.settings.author = "Spinus tests";

    Insert drumsBus;
    drumsBus.id = p.allocateId<InsertId>();
    drumsBus.name = "Drums";
    drumsBus.effects.push_back (
        { Device { p.allocateId<DeviceId>(), Device::Kind::stock, "bundled.lsp.compressor-stereo", {}, false }, 1.0f });

    Insert reverb;
    reverb.id = p.allocateId<InsertId>();
    reverb.name = "Reverb";
    Device plugin { p.allocateId<DeviceId>(),
                    Device::Kind::plugin,
                    {},
                    { "CLAP", "Dragonfly Hall Reverb", "Michael Willis",
                      "/usr/lib/spinus/plugins/DragonflyHallReverb.clap", 1234 },
                    false };
    reverb.effects.push_back ({ plugin, 0.5f });
    drumsBus.sends.push_back ({ reverb.id, -12.0f });

    Insert bass;
    bass.id = p.allocateId<InsertId>();
    bass.name = "Bass";
    bass.sidechainSources.push_back (drumsBus.id);
    p.mixer.inserts = { drumsBus, reverb, bass };

    Channel kick;
    kick.id = p.allocateId<ChannelId>();
    kick.name = "Kick";
    kick.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.sampler", {}, false };
    kick.targetInsert = drumsBus.id;

    Channel synth;
    synth.id = p.allocateId<ChannelId>();
    synth.name = "Surge";
    synth.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "bundled.surge-xt", {}, false };
    synth.volumeDb = -3.0f;
    synth.pan = 0.25f;
    synth.targetInsert = bass.id;
    p.channels = { kick, synth };

    Pattern pattern;
    pattern.id = p.allocateId<PatternId>();
    pattern.name = "Beat";
    for (int step = 0; step < 4; ++step)
        pattern.notes[kick.id].push_back ({ step * ticksPerQuarterNote, 240, 36, 1.0f, 0.5f, 0.0f, 0.0f, {} });
    pattern.notes[synth.id].push_back (
        { 0, 2 * ticksPerQuarterNote, 45, 0.75f, 0.5f, -0.5f, 12.5f, { { "brightness", 0.3f } } });
    p.patterns = { pattern };

    Lane lane1 { p.allocateId<LaneId>(), "Track 1", 0xff3d4450, false };
    Lane lane2 { p.allocateId<LaneId>(), "Track 2", 0xff3d4450, true };
    p.playlist.lanes = { lane1, lane2 };

    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), lane1.id, 0, pattern.length, false, PatternClip { pattern.id, 0 } });
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), lane1.id, pattern.length, pattern.length, false, PatternClip { pattern.id, 960 } });
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane2.id, 0, 7680, false,
                                  AudioClip { "vocals.wav", {}, 0, -2.0f, 0.0f, "tempo-sync", 96, 480 } });

    AutomationClip automation;
    automation.target = { ParameterTarget::Kind::deviceParameter, {}, {}, plugin.id, "size" };
    automation.points = { { 0, 0.0f, 0.0f }, { 3840, 1.0f, 0.5f } };
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane2.id, 0, 3840, false, automation });

    // The clip launcher: a pattern looped in one scene, audio played once in the other.
    const LauncherTrack jam { p.allocateId<LauncherTrackId>(), "Jam", 0xff3d4450 };
    const Scene verse { p.allocateId<SceneId>(), "Verse" }, chorus { p.allocateId<SceneId>(), "Chorus" };
    p.launcher.tracks = { jam };
    p.launcher.scenes = { verse, chorus };
    p.launcher.clips.push_back (
        { p.allocateId<ClipId>(), jam.id, verse.id, pattern.length, true, PatternClip { pattern.id, 0 } });
    p.launcher.clips.push_back ({ p.allocateId<ClipId>(), jam.id, chorus.id, 7680, false,
                                  AudioClip { "vocals.wav", {}, 0, -2.0f, 0.0f, "off", 0, 0 } });
    p.launcher.quantize = LaunchQuantize::beat;
    return p;
}

} // namespace spinus::test
