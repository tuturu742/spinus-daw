#pragma once

#include "../model/ExampleProject.h"

#include <utility>
#include <vector>

namespace spinus::test
{

/** One valid example call per command; the generic undo/redo test runs all of them. Every registered
    command must appear here (checked in CommandTests.cpp). */
inline const std::vector<std::pair<juce::String, juce::String>>& exampleCalls()
{
    static const std::vector<std::pair<juce::String, juce::String>> calls {
        { "project.setTempo", R"({"bpm": 128})" },
        { "project.setKey", R"({"tonic": 2, "scale": "dorian"})" },
        { "channel.add", R"({"name": "Hats", "instrument": {"kind": "stock", "stockType": "spinus.sampler"}})" },
        { "channel.remove", R"({"channel": 9})" },
        { "channel.replaceInstrument",
          R"({"channel": 9, "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})" },
        { "channel.set",
          R"({"channel": 7, "name": "Big kick", "volumeDb": -6, "muted": true, "colour": "#ff112233"})" },
        { "channel.route", R"({"channel": 7, "insert": 6})" },
        { "channel.move", R"({"channel": 9, "index": 0})" },
        { "channel.freeze", R"({"channel": 7})" },
        { "insert.add", R"({"name": "Vocals", "index": 0})" },
        { "insert.addBus", R"({"name": "Low end", "inserts": [2, 6]})" },
        { "insert.remove", R"({"insert": 2})" },
        { "insert.set", R"({"insert": 1, "volumeDb": -1.5, "stereoWidth": 0.5})" },
        { "insert.move", R"({"insert": 6, "index": 0})" },
        { "insert.freeze", R"({"insert": 6, "frozen": true})" },
        { "insert.setOutput", R"({"insert": 6, "target": 4})" }, // 6 follows 2 as a sidechain: not into 2
        { "insert.setSend", R"({"insert": 6, "target": 4, "levelDb": -9})" },
        { "insert.setSidechain", R"({"insert": 4, "source": 6})" },
        { "insert.setNotesFrom", R"({"insert": 6, "channel": 9})" },
        { "insert.removeSend", R"({"insert": 2, "target": 4})" },
        { "insert.addEffect",
          R"({"insert": 6, "device": {"kind": "stock", "stockType": "bundled.lsp.compressor-stereo"}, "index": 0})" },
        { "insert.removeEffect", R"({"device": 3})" },
        { "insert.replaceEffect", R"({"device": 5, "effect": {"kind": "stock", "stockType": "spinus.filter"}})" },
        { "insert.moveEffect", R"({"device": 3, "insert": 6, "index": 0})" },
        { "insert.setEffect", R"({"device": 5, "bypassed": true})" },
        { "device.setParameters", R"({"device": 3, "values": {"ratio": 0.5}})" },
        { "device.loadPreset", R"({"device": 8, "preset": "Reese Bass"})" }, // the kick made a 3x Osc below
        { "pattern.add", R"({"name": "Fill", "length": 3840})" },
        { "pattern.remove", R"({"pattern": 11})" },
        { "lane.add", R"({"name": "Drums"})" },
        { "lane.remove", R"({"lane": 12})" },
        { "lane.set", R"({"lane": 12, "name": "Beat", "muted": true})" },
        { "lane.set", R"({"lane": 12, "solo": true})" },
        { "lane.move", R"({"lane": 13, "index": 0})" },
        { "clip.addPattern", R"({"lane": 13, "pattern": 11, "start": 7680})" },
        { "clip.set", R"({"clip": 14, "lane": 13, "start": 1920, "length": 3840})" },
        { "clip.remove", R"({"clip": 15})" },
        { "clip.split", R"({"clip": 16, "at": 1000})" },
        { "clip.makeUnique", R"({"clip": 15})" },
        { "clip.add", R"({"clip": {"type": "pattern", "pattern": 11, "lane": 13, "start": 7680, "length": 3840,
              "offset": 960, "muted": true}})" },
        { "marker.add", R"({"at": 7680, "name": "Bridge"})" },
        { "marker.set", R"({"marker": 23, "name": "Drop", "at": 1920})" },
        { "marker.remove", R"({"marker": 23})" },
        { "clip.addAudio", R"({"lane": 13, "file": "drums/loop.wav", "start": 3840, "length": 7680})" },
        { "clip.addAudio",
          R"({"lane": 12, "file": "/home/me/vox.flac", "start": 0, "length": 960, "channel": 7, "gainDb": -3})" },
        { "clip.addTake", R"({"channel": 7, "file": "take2.wav", "start": 0, "length": 960})" },
        { "clip.useTake", R"({"clip": 24})" },
        { "clip.setAudio", R"({"clip": 16, "gainDb": -6, "fadeIn": 240})" },
        { "clip.setTune",
          R"({"clip": 16, "notes": [{"start": 0.5, "end": 1.0, "pitch": 60.4, "shift": -0.4, "drift": 0.2},
              {"start": 1.2, "end": 1.8, "pitch": 63.1, "vibrato": 0.5}], "description": "Tune notes"})" },
        { "clip.addAutomation",
          R"({"start": 0, "length": 3840, "laneName": "Drums volume", "target": {"kind": "insertVolume", "insert": 2},
              "points": [{"time": 0, "value": 0.2}, {"time": 3840, "value": 0.9, "curve": 0.3}]})" },
        { "clip.addAutomation",
          R"({"lane": 13, "start": 960, "length": 1920, "target": {"kind": "deviceParameter", "device": 5, "parameterId": "size"},
              "points": [{"time": 0, "value": 1}]})" },
        { "clip.setPoints", R"({"clip": 17, "points": [{"time": 960, "value": 0.25}, {"time": 0, "value": 0.5}]})" },
        { "pattern.setLength", R"({"pattern": 11, "length": 7680})" },
        { "pattern.setTimeSignature", R"({"pattern": 11, "numerator": 7, "denominator": 8})" },
        { "pattern.setTripletSteps", R"({"pattern": 11, "triplets": true})" },
        { "pattern.set", R"({"pattern": 11, "name": "Chorus drums", "colour": "#ffd65c5c"})" },
        { "pattern.clone", R"({"pattern": 11})" },
        { "pattern.setSwing", R"({"pattern": 11, "swing": 0.6})" },
        { "project.setTimeSignature", R"({"numerator": 6, "denominator": 8})" },
        { "pattern.setNotes",
          R"({"pattern": 11, "channel": 9, "notes": [{"start": 0, "length": 480, "pitch": 48, "velocity": 0.6}]})" },
        // The example's launcher: track 18, scenes 19 and 20, pattern clip 21 (18/19), audio clip 22 (18/20).
        { "launcher.addTrack", R"({"name": "Bass jam"})" },
        { "launcher.removeTrack", R"({"track": 18})" },
        { "launcher.setTrack", R"({"track": 18, "name": "Keys", "colour": "#ff336699"})" },
        { "launcher.addScene", R"({"name": "Bridge", "index": 1})" },
        { "launcher.removeScene", R"({"scene": 20})" },
        { "launcher.setScene", R"({"scene": 19, "name": "Intro"})" },
        { "launcher.addPattern", R"({"track": 18, "scene": 20, "pattern": 11, "length": 3840})" },
        { "launcher.addAudio", R"({"track": 18, "scene": 19, "file": "loops/pad.wav", "length": 7680})" },
        { "launcher.addAudio", R"({"track": 18, "scene": 19, "file": "pad.wav", "length": 960, "channel": 7})" },
        { "launcher.setClip", R"({"clip": 21, "scene": 20, "loop": false})" },
        { "launcher.removeClip", R"({"clip": 22})" },
        { "launcher.setQuantize", R"({"quantize": "none"})" },
    };
    return calls;
}

/** The example project an example call runs on: makeExample() with what some calls need. */
inline Project exampleProjectFor (const juce::String& commandName)
{
    auto project = makeExample();
    project.playlist.markers = { { project.allocateId<MarkerId>(), 3840, "Chorus" } }; // ID 23
    // ID 24: a muted second take of the vocals (clip 16), on lane 12, for clip.useTake.
    project.playlist.clips.push_back ({ project.allocateId<ClipId>(), LaneId { 12 }, 0, 960, true,
                                        std::get<AudioClip> (project.findClip (ClipId { 16 })->content) });
    if (commandName == "device.loadPreset")
        project.channels[0].instrument.value().stockType = "spinus.3x-osc"; // only 3x Osc has built-in presets
    return project;
}

} // namespace spinus::test
