#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/Session.h"

using namespace spinus;
using LaunchState = engine::EngineAdapter::LaunchState;

namespace
{
constexpr double rate = 48000.0;

juce::File writeTone (const juce::File& file, double seconds, double frequency)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.3f * (float)std::sin (2.0 * M_PI * frequency * i / rate));
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

/** How strongly a frequency sounds in part of a recording (Goertzel), as an amplitude. */
float toneLevel (const std::vector<float>& x, int64_t begin, int64_t end, double frequency)
{
    end = std::min<int64_t> (end, (int64_t)x.size());
    if (end <= begin)
        return 0.0f;
    const auto coefficient = 2.0 * std::cos (2.0 * M_PI * frequency / rate);
    double s1 = 0.0, s2 = 0.0;
    for (auto n = begin; n < end; ++n)
    {
        const auto s = (double)x[(size_t)n] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const auto power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return (float)(2.0 * std::sqrt (std::max (0.0, power)) / (double)(end - begin));
}

struct Fixture
{
    Fixture()
    {
        root.createDirectory();
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
    }
    ~Fixture()
    {
        session = nullptr;
        root.deleteRecursively();
    }

    juce::var run (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        const auto result = session->getHistory().perform (std::move (command));
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
        return raw->getResult();
    }

    juce::String tone (const juce::String& name, double frequency)
    {
        return juce::JSON::toString (writeTone (root.getChildFile (name + ".wav"), 12.0, frequency).getFullPathName());
    }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-launcher") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
};

model::ClipId clipOf (const juce::var& result) { return model::ClipId { (uint32_t)(int)result["clip"] }; }
} // namespace

TEST_CASE ("Launcher clips sit in Tracktion's clip slots of the channels they play on", "[engine][launcher]")
{
    Fixture f;
    auto& adapter = f.session->getAdapter();
    const auto track = f.run ("launcher.addTrack", "{}")["track"].toString();
    const auto scene = f.run ("launcher.addScene", "{}")["scene"].toString();
    const auto scene2 = f.run ("launcher.addScene", "{}")["scene"].toString();

    // A pattern with notes for two channels: a MIDI clip in a slot of each channel's track.
    const auto lead = f.run ("channel.add", R"({"name": "Lead"})")["channel"].toString();
    const auto bass = f.run ("channel.add", R"({"name": "Bass"})")["channel"].toString();
    f.run ("channel.add", R"({"name": "Unused"})");
    const auto pattern = f.run ("pattern.add", R"({"name": "Riff", "length": 3840})")["pattern"].toString();
    for (auto& channel : { lead, bass })
        f.run ("pattern.setNotes", R"({"pattern": )" + pattern + R"(, "channel": )" + channel +
                                       R"(, "notes": [{"start": 0, "length": 480, "pitch": 60}]})");
    const auto riff = clipOf (f.run ("launcher.addPattern", R"({"track": )" + track + R"(, "scene": )" + scene +
                                                                R"(, "pattern": )" + pattern + "}"));
    auto clips = adapter.getLauncherClips (riff);
    REQUIRE (clips.size() == 2);
    for (auto* clip : clips)
    {
        auto* midi = dynamic_cast<tracktion::MidiClip*> (clip);
        REQUIRE (midi != nullptr);
        CHECK (midi->getSequence().getNumNotes() == 1);
        CHECK (midi->isLooping());
        CHECK (dynamic_cast<tracktion::ClipSlot*> (clip->getParent()) != nullptr);
    }

    // Audio: a wave clip on its own channel's track. A missing file has nothing to play.
    const auto audio =
        clipOf (f.run ("launcher.addAudio", R"({"track": )" + track + R"(, "scene": )" + scene2 + R"(, "file": )" +
                                                f.tone ("pad", 330.0) + R"(, "length": 3840, "loop": false})"));
    REQUIRE (adapter.getLauncherClips (audio).size() == 1);
    CHECK (dynamic_cast<tracktion::WaveAudioClip*> (adapter.getLauncherClips (audio)[0]) != nullptr);
    CHECK (! adapter.getLauncherClips (audio)[0]->isLooping());
    const auto slotOf = [&] (model::ClipId id)
    { return dynamic_cast<tracktion::ClipSlot*> (adapter.getLauncherClips (id)[0]->getParent())->getIndex(); };
    const auto audioSlot = slotOf (audio);

    // Removing a clip takes its Tracktion clips; the other keeps its slot (a playing clip never moves).
    f.run ("launcher.removeClip", R"({"clip": )" + juce::String (riff.value) + "}");
    CHECK (adapter.getLauncherClips (riff).empty());
    CHECK (slotOf (audio) == audioSlot);

    // An edit to the notes rewrites the clip in place.
    f.session->getHistory().undo();
    const auto* before = adapter.getLauncherClips (riff)[0];
    f.run ("pattern.setNotes",
           R"({"pattern": )" + pattern + R"(, "channel": )" + lead +
               R"(, "notes": [{"start": 0, "length": 480, "pitch": 60}, {"start": 960, "length": 480, "pitch": 64}]})");
    bool rewrittenInPlace = false;
    for (auto* clip : adapter.getLauncherClips (riff))
        rewrittenInPlace = rewrittenInPlace || (clip == before);
    CHECK (rewrittenInPlace);

    // Nothing to play.
    const auto missing = clipOf (f.run ("launcher.addAudio", R"({"track": )" + track + R"(, "scene": )" + scene2 +
                                                                 R"(, "file": "/nowhere/gone.wav", "length": 960})"));
    CHECK (adapter.getLauncherClips (missing).empty());
    CHECK (! f.session->launchClip (missing));
}

TEST_CASE ("Launching: on the next bar, one clip per launcher track, channels leave the Playlist",
           "[engine][launcher][audio]")
{
    Fixture f;
    auto& session = *f.session;
    f.run ("project.setTempo", R"({"bpm": 240})"); // a bar is one second

    // The Playlist plays a 220 Hz tone through channel A for the whole test.
    const auto lane = f.run ("lane.add", "{}")["lane"].toString();
    const auto playlistTone = f.run ("clip.addAudio", R"({"lane": )" + lane + R"(, "file": )" + f.tone ("a", 220.0) +
                                                          R"(, "start": 0, "length": 46080, "name": "A"})");
    const auto channelA = playlistTone["channel"].toString();

    // Launcher: track 1 has 440 Hz (its own channel) in scene 1 and 880 Hz through channel A in scene 2;
    // track 2 has 660 Hz in scene 1.
    const auto track1 = f.run ("launcher.addTrack", "{}")["track"].toString();
    const auto track2 = f.run ("launcher.addTrack", "{}")["track"].toString();
    const auto scene1 = f.run ("launcher.addScene", "{}")["scene"];
    const auto scene2 = f.run ("launcher.addScene", "{}")["scene"].toString();
    auto addTone = [&] (const juce::String& track, const juce::String& scene, const juce::String& name, double hz,
                        const juce::String& extra)
    {
        return clipOf (f.run ("launcher.addAudio", R"({"track": )" + track + R"(, "scene": )" + scene +
                                                       R"(, "file": )" + f.tone (name, hz) + R"(, "length": 3840)" +
                                                       extra + "}"));
    };
    const auto b = addTone (track1, scene1.toString(), "b", 440.0, "");
    const auto a2 = addTone (track1, scene2, "a2", 880.0, R"(, "channel": )" + channelA);
    const auto c = addTone (track2, scene1.toString(), "c", 660.0, "");

    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 20.0, .paceInRealTime = true });
    const auto sr = runner.getParams().sampleRate;
    runner.start();
    runner.runUntil (0.2);
    session.setMode (Session::Mode::song);
    session.play();
    const auto t0 = runner.getSecondsProcessed(); // bar 1 starts about here
    auto level = [&] (double from, double to, double hz)
    { return toneLevel (runner.getChannel (0), (int64_t)((t0 + from) * sr), (int64_t)((t0 + to) * sr), hz); };
    constexpr float on = 0.05f, off = 0.005f;

    // Bars are a second long from t0. Launches happen away from a bar line (a busy machine can be late),
    // and each check counts from the bar the launch fell before.
    auto now = [&] { return runner.getSecondsProcessed() - t0; };
    auto awayFromBarLine = [&]
    {
        while (now() - std::floor (now()) > 0.75 || now() - std::floor (now()) < 0.1)
            runner.runUntil (t0 + now() + 0.05);
        return now();
    };

    // Launched during a bar: starts on the next.
    runner.runUntil (t0 + 0.3);
    auto at = awayFromBarLine();
    REQUIRE (session.launchClip (b));
    CHECK (session.getAdapter().getLaunchState (b) == LaunchState::starting);
    const auto bBar = std::ceil (at);
    runner.runUntil (t0 + bBar + 0.9);
    CHECK (session.getAdapter().getLaunchState (b) == LaunchState::playing);
    CHECK (session.getAdapter().getLaunchProgress (b).value_or (-1.0) > 0.0);
    CHECK (level (at + 0.1, bBar - 0.1, 440.0) < off);
    CHECK (level (bBar + 0.15, bBar + 0.85, 440.0) > on);
    CHECK (level (bBar + 0.15, bBar + 0.85, 220.0) > on); // channel A still plays the Playlist

    // Another clip on the same launcher track: the first stops as it starts, on the next bar. It plays
    // through channel A, so A leaves the Playlist.
    runner.runUntil (t0 + bBar + 1.3);
    at = awayFromBarLine();
    REQUIRE (session.launchClip (a2));
    const auto a2Bar = std::ceil (at);
    runner.runUntil (t0 + a2Bar + 0.9);
    CHECK (session.getAdapter().getLaunchState (b) == LaunchState::stopped);
    CHECK (session.getAdapter().getLaunchState (a2) == LaunchState::playing);
    CHECK (level (at + 0.05, a2Bar - 0.1, 440.0) > on);
    CHECK (level (a2Bar + 0.2, a2Bar + 0.85, 440.0) < off);
    CHECK (level (a2Bar + 0.2, a2Bar + 0.85, 880.0) > on);
    CHECK (level (a2Bar + 0.2, a2Bar + 0.85, 220.0) < off);
    CHECK (session.getAdapter().isLauncherActive());

    // Back to the Playlist: at once.
    session.backToPlaylist();
    at = now();
    runner.runUntil (t0 + at + 1.0);
    CHECK (level (at + 0.3, at + 0.95, 880.0) < off);
    CHECK (level (at + 0.3, at + 0.95, 220.0) > on);
    CHECK (! session.getAdapter().isLauncherActive());

    // A scene launches each launcher track's clip in it, together on the next bar.
    at = awayFromBarLine();
    session.launchScene (model::SceneId { (uint32_t)(int)scene1 });
    const auto sceneBar = std::ceil (at);
    runner.runUntil (t0 + sceneBar + 0.9);
    CHECK (level (sceneBar + 0.2, sceneBar + 0.85, 440.0) > on);
    CHECK (level (sceneBar + 0.2, sceneBar + 0.85, 660.0) > on);

    // Stopping playback stops them; launching while stopped starts playback, at once without quantising.
    session.stop();
    CHECK (! session.isPlaying());
    runner.runUntil (t0 + now() + 0.2);
    CHECK (session.getAdapter().getLaunchState (c) == LaunchState::stopped);
    f.run ("launcher.setQuantize", R"({"quantize": "none"})");
    const auto restart = now();
    REQUIRE (session.launchClip (c));
    CHECK (session.isPlaying());
    runner.runUntil (t0 + restart + 0.6);
    runner.stop();
    CHECK (level (restart + 0.15, restart + 0.55, 660.0) > on);
}

TEST_CASE ("A launched pattern plays its channels' instruments, looped", "[engine][launcher][audio]")
{
    Fixture f;
    auto& session = *f.session;
    f.run ("project.setTempo", R"({"bpm": 240})");
    const auto synth =
        f.run ("channel.add",
               R"({"name": "Synth", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})")["channel"]
            .toString();
    // One short note at the start of a one-bar pattern: it sounds again on every pass.
    const auto pattern = f.run ("pattern.add", R"({"name": "Blip", "length": 3840})")["pattern"].toString();
    f.run ("pattern.setNotes", R"({"pattern": )" + pattern + R"(, "channel": )" + synth +
                                   R"(, "notes": [{"start": 0, "length": 480, "pitch": 69, "velocity": 1}]})");
    const auto track = f.run ("launcher.addTrack", "{}")["track"].toString();
    const auto scene = f.run ("launcher.addScene", "{}")["scene"].toString();
    const auto clip = clipOf (f.run ("launcher.addPattern", R"({"track": )" + track + R"(, "scene": )" + scene +
                                                                R"(, "pattern": )" + pattern + "}"));

    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 6.0, .paceInRealTime = true });
    const auto sr = runner.getParams().sampleRate;
    runner.start();
    runner.runUntil (0.3);
    const auto t0 = runner.getSecondsProcessed();
    REQUIRE (session.launchClip (clip)); // stopped: starts playback, and the clip at once
    runner.runUntil (t0 + 2.6);
    runner.stop();
    auto rms = [&] (double from, double to)
    { return test::checks::rms (runner.getChannel (0), (int64_t)((t0 + from) * sr), (int64_t)((t0 + to) * sr)); };
    // The note is an eighth of a second long (a quarter at 240 BPM): sound, then quiet, then sound again.
    CHECK (rms (0.0, 0.2) > 0.01f);
    CHECK (rms (0.5, 0.9) < rms (0.0, 0.2) * 0.1f);
    CHECK (rms (1.0, 1.2) > 0.01f);
    CHECK (rms (2.0, 2.2) > 0.01f);
}

TEST_CASE ("Record to Playlist writes what was launched as Playlist clips, one per pass", "[engine][launcher][audio]")
{
    Fixture f;
    auto& session = *f.session;
    f.run ("project.setTempo", R"({"bpm": 240})"); // a bar is one second, 3840 ticks
    const auto track = f.run ("launcher.addTrack", R"({"name": "Jam"})")["track"].toString();
    const auto scene1 = f.run ("launcher.addScene", "{}")["scene"].toString();
    const auto scene2 = f.run ("launcher.addScene", "{}")["scene"].toString();
    const auto loop =
        clipOf (f.run ("launcher.addAudio", R"({"track": )" + track + R"(, "scene": )" + scene1 + R"(, "file": )" +
                                                f.tone ("loop", 440.0) + R"(, "length": 3840, "name": "Loop"})"));
    const auto once = clipOf (f.run ("launcher.addAudio", R"({"track": )" + track + R"(, "scene": )" + scene2 +
                                                              R"(, "file": )" + f.tone ("hit", 880.0) +
                                                              R"(, "length": 1920, "loop": false, "name": "Hit"})"));

    session.setMode (Session::Mode::pattern);
    session.setLauncherRecording (true);
    CHECK (session.getMode() == Session::Mode::song); // recording needs song positions

    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 8.0, .paceInRealTime = true });
    runner.start();
    runner.runUntil (0.2);
    session.play();
    const auto t0 = runner.getSecondsProcessed();

    // The loop from bar 2 for two passes, then the one-shot from bar 4 (half a bar), then stop in bar 5.
    runner.runUntil (t0 + 0.3);
    REQUIRE (session.launchClip (loop));
    runner.runUntil (t0 + 2.3);
    REQUIRE (session.launchClip (once));
    runner.runUntil (t0 + 4.4);
    session.stop();
    runner.stop();

    const auto& project = session.getProject();
    CHECK (session.getLastLauncherRecorded() == 3);
    CHECK (session.getHistory().getUndoDescriptions().strings.getLast() == "Record the launcher to the Playlist");
    REQUIRE (project.playlist.lanes.size() == 1);
    CHECK (project.playlist.lanes[0].name == "Launcher: Jam");
    REQUIRE (project.playlist.clips.size() == 3);
    auto near = [] (model::Ticks a, model::Ticks b) { return std::abs (a - b) <= 2; };
    const auto& first = project.playlist.clips[0];
    const auto& second = project.playlist.clips[1];
    const auto& hit = project.playlist.clips[2];
    CHECK (near (first.start, 3840));
    CHECK (near (first.length, 3840));
    CHECK (near (second.start, 7680));
    CHECK (near (second.length, 3840));
    CHECK (near (hit.start, 11520));
    CHECK (near (hit.length, 1920)); // once: its own length, not until the stop
    CHECK (std::get<model::AudioClip> (hit.content).file.endsWith ("hit.wav"));

    // Nothing launched: nothing written.
    session.setLauncherRecording (false);
    CHECK (session.getLastLauncherRecorded() == 0);
    session.getHistory().undo();
    CHECK (project.playlist.clips.empty());
}
