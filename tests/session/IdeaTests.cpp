#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "RealtimeRunner.h"
#include "engine/EngineHost.h"
#include "model/Scales.h"
#include "session/IdeaToNotes.h"
#include "session/Session.h"

using namespace spinus;
using namespace spinus::model;

namespace
{
/** A one-bar pattern (2 s at 120 BPM) in a project, optionally in C major. */
Project oneBar (bool inCMajor)
{
    Project p;
    p.settings.tempoBpm = 120.0;
    if (inCMajor)
        p.settings.key = Key { 0, "major" };
    Pattern pattern;
    pattern.id = p.allocateId<PatternId>();
    pattern.length = 4 * ticksPerQuarterNote;
    p.patterns = { pattern };
    return p;
}

juce::var parse (const char* json) { return juce::JSON::parse (json); }
} // namespace

TEST_CASE ("Keys: which notes are in them, and the nearest one", "[idea][model]")
{
    const Key aMinor { 9, "minor" };
    CHECK (isInKey (aMinor, 69));
    CHECK (isInKey (aMinor, 72));
    CHECK (! isInKey (aMinor, 70));
    CHECK (nearestNoteInKey (aMinor, 69.6f) == 69);
    CHECK (nearestNoteInKey (aMinor, 70.4f) == 71);
    CHECK (nearestNoteInKey (Key { 0, "chromatic" }, 61.2f) == 61);
    CHECK (isInKey (Key { 0, "something new" }, 64)); // unknown scales count as major
}

TEST_CASE ("A sung take becomes notes in time with the pattern, on the grid and in the key", "[idea]")
{
    const auto p = oneBar (true);
    const IdeaRecorder::Take take { {}, 1.0, 2.0, 120.0 }; // the pattern starts 1 s into the file

    // In the file: before the pattern (dropped), on beat 1 (a little late), beat 2 (C# -> D in C major),
    // a note sung over the next one's start, and one after the pattern's end (dropped).
    const auto heard = parse (R"({"notes": [
        {"start": 0.5, "end": 0.9, "pitch": 64.0},
        {"start": 1.02, "end": 1.47, "pitch": 60.3},
        {"start": 1.51, "end": 2.2, "pitch": 61.4},
        {"start": 2.0, "end": 2.4, "pitch": 67.1},
        {"start": 3.1, "end": 3.5, "pitch": 65.0}]})");

    const auto notes = IdeaToNotes {}.melody (heard, take, p, p.patterns[0].id);
    REQUIRE (notes.size() == 3);
    CHECK (notes[0].start == 0);
    CHECK (notes[0].length == 960);
    CHECK (notes[0].pitch == 60);
    CHECK (notes[1].start == 960);
    CHECK (notes[1].pitch == 62);
    CHECK (notes[1].length == 960); // cut where the next note starts
    CHECK (notes[2].start == 1920);
    CHECK (notes[2].pitch == 67);

    // Without a key (or with snapping off), the nearest semitone.
    IdeaToNotes free;
    free.snapToKey = false;
    CHECK (free.melody (heard, take, p, p.patterns[0].id)[1].pitch == 61);
    CHECK (IdeaToNotes {}.melody (heard, take, oneBar (false), p.patterns[0].id)[1].pitch == 61);
}

TEST_CASE ("Beatboxed hits become Drum Machine steps", "[idea]")
{
    const auto p = oneBar (false);
    const IdeaRecorder::Take take { {}, 0.5, 2.0, 120.0 };
    const auto heard = parse (R"({"hits": [
        {"time": 0.51, "sound": "kick", "strength": 1.0},
        {"time": 0.76, "sound": "hat", "strength": 0.2},
        {"time": 1.0, "sound": "snare", "strength": 0.8},
        {"time": 1.01, "sound": "snare", "strength": 0.8},
        {"time": 3.0, "sound": "kick", "strength": 1.0}]})");

    const auto notes = IdeaToNotes {}.beats (heard, take, p, p.patterns[0].id);
    REQUIRE (notes.size() == 3);
    CHECK ((notes[0].start == 0 && notes[0].pitch == 60));
    CHECK ((notes[1].start == 480 && notes[1].pitch == 63)); // half a beat later
    CHECK (notes[1].velocity < notes[0].velocity);
    CHECK ((notes[2].start == 960 && notes[2].pitch == 61));
}

TEST_CASE ("Record idea: a count-in, then the pattern's second time through, found in the take", "[idea][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    test::RealtimeRunner runner (
        host.getEngine(),
        { .outputChannels = 2, .inputChannels = 1, .loopback = true, .maxSeconds = 8.0, .paceInRealTime = true });
    session.getHistory().reset (oneBar (false));
    session.setCurrentPattern (session.getProject().patterns[0].id);

    auto& recorder = session.getIdeaRecorder();
    std::optional<IdeaRecorder::Take> take;
    recorder.onFinished = [&] (const IdeaRecorder::Take& t) { take = t; };
    REQUIRE (recorder.startWithPattern().wasOk());
    CHECK (session.isMetronomeOn());
    CHECK (recorder.getState() == IdeaRecorder::State::countIn);

    runner.start();
    bool sawCountIn = false;
    for (int i = 0; i < 400 && ! take; ++i)
    {
        runner.runUntil (runner.getSecondsProcessed() + 0.02);
        sawCountIn |= recorder.getState() == IdeaRecorder::State::countIn && recorder.getBeatsToGo() > 0;
        juce::Thread::sleep (2);
    }
    runner.stop();
    REQUIRE (take.has_value());
    const auto recorded = take.value_or (IdeaRecorder::Take {});
    CHECK (sawCountIn);
    CHECK (! session.isMetronomeOn()); // as it was
    CHECK (! session.isPlaying());
    CHECK (juce::approximatelyEqual (recorded.patternSeconds, 2.0));

    // The take heard the metronome through the cable: its click on the pattern's first beat is where
    // the take says the pattern starts (one block late: the cable's delay).
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (recorded.file), true));
    REQUIRE (reader != nullptr);
    std::vector<float> audio ((size_t)reader->lengthInSamples);
    float* channels[] = { audio.data() };
    reader->read (channels, 1, 0, (int)audio.size());
    const auto rate = reader->sampleRate;
    const auto from = (size_t)juce::jmax (0.0, (recorded.patternStart - 0.1) * rate);
    size_t click = from;
    while (click < audio.size() && std::abs (audio[click]) < 0.01f)
        ++click;
    REQUIRE (click < audio.size());
    const auto expected = recorded.patternStart + 128.0 / rate;
    INFO ("click at " << click / rate << " s, pattern start " << recorded.patternStart << " s");
    CHECK (std::abs ((double)click / rate - expected) < 0.004);
    recorded.file.deleteFile();
}

TEST_CASE ("Record idea needs an audio input and a pattern", "[idea]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    CHECK (session.getIdeaRecorder().startWithPattern().getErrorMessage().contains ("pattern"));
    session.getHistory().reset (oneBar (false));
    session.setCurrentPattern (session.getProject().patterns[0].id);
    CHECK (session.getIdeaRecorder().startWithPattern().getErrorMessage().contains ("no audio input"));
    CHECK (session.getIdeaRecorder().getState() == IdeaRecorder::State::idle);
}
