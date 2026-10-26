#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "FakeSoundCard.h"
#include "RealtimeRunner.h"
#include "engine/EngineHost.h"
#include "commands/CommandRegistry.h"
#include "session/RecordingTime.h"
#include "session/Session.h"

using namespace spinus;

// The song plays from where it is and the input is recorded until playback stops. A cable from the
// output to the input (the test runner's loopback) records the metronome: each click must be in the file
// where the take says the song was then, so the take lands on the Playlist in time.
TEST_CASE ("Record audio: the input while the song plays, placed where it was played", "[audiorecord][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    test::RealtimeRunner runner (
        host.getEngine(),
        { .outputChannels = 2, .inputChannels = 2, .loopback = true, .maxSeconds = 8.0, .paceInRealTime = true });
    model::Project project;
    project.settings.tempoBpm = 120.0; // a beat every half second
    session.getHistory().reset (project);
    session.setMetronome (true);

    auto& recorder = session.getAudioRecorder();
    std::optional<AudioRecorder::Take> take;
    recorder.onFinished = [&] (const std::vector<AudioRecorder::Take>& t)
    {
        REQUIRE (t.size() == 1);
        take = t[0];
    };
    REQUIRE (recorder.start().wasOk());
    CHECK (recorder.isRecording());
    CHECK (session.isPlaying());
    CHECK (session.getMode() == Session::Mode::song);
    CHECK (recorder.start().failed()); // once at a time

    runner.start();
    runner.runUntil (2.3);
    session.stop(); // Space: the take ends
    for (int i = 0; i < 100 && ! take; ++i)
    {
        runner.runUntil (runner.getSecondsProcessed() + 0.02);
        juce::Thread::sleep (2);
    }
    runner.stop();
    REQUIRE (take.has_value());
    const auto recorded = take.value_or (AudioRecorder::Take {});
    CHECK (! recorder.isRecording());
    CHECK (recorded.channels >= 1); // as the input is: Tracktion's inputs are mono unless set as a stereo pair
    CHECK (recorded.file.getFileName().startsWith ("Recording ")); // 1, or more when another test recorded too

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (recorded.file), true));
    REQUIRE (reader != nullptr);
    CHECK ((int)reader->numChannels == recorded.channels);
    CHECK (reader->bitsPerSample == 24);
    const auto rate = reader->sampleRate;
    CHECK ((double)reader->lengthInSamples / rate > 1.8);

    // The click of the song's second beat (1 s in) is where the take says 1 s was (one block late: the
    // cable's delay).
    std::vector<float> audio ((size_t)reader->lengthInSamples);
    float* channels[] = { audio.data() };
    reader->read (channels, 1, 0, (int)audio.size());
    const auto expected = 1.0 - recorded.startSeconds + 128.0 / rate;
    auto click = (size_t)juce::jmax (0.0, (expected - 0.1) * rate);
    while (click < audio.size() && std::abs (audio[click]) < 0.01f)
        ++click;
    REQUIRE (click < audio.size());
    INFO ("click at " << click / rate << " s in the file, take starts at " << recorded.startSeconds << " s");
    CHECK (std::abs ((double)click / rate - expected) < 0.004);
    recorded.file.deleteFile();
}

TEST_CASE ("Record audio needs an audio input", "[audiorecord]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    CHECK (session.getAudioRecorder().start().getErrorMessage().contains ("no audio input"));
    CHECK (! session.getAudioRecorder().isRecording());
    CHECK (! session.isPlaying());
}

namespace
{
/** A sound card whose input k plays a sine of 200 * (k + 1) Hz. */
void sines (juce::AudioBuffer<float>& block, int inputs, int64_t first)
{
    for (int k = 0; k < inputs; ++k)
        for (int i = 0; i < block.getNumSamples(); ++i)
            block.setSample (k, i,
                             0.5f * (float)std::sin (juce::MathConstants<double>::twoPi * 200.0 * (k + 1) *
                                                     (double)(first + i) / 48000.0));
}

/** A channel's frequency in a WAV file, from its zero crossings in the middle half second. */
double frequencyOf (const juce::File& file, int channel)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
    if (reader == nullptr || channel >= (int)reader->numChannels)
        return 0.0;
    const auto rate = reader->sampleRate;
    const auto length = (int)std::min (reader->lengthInSamples, (juce::int64)(rate * 0.5));
    const auto start = (reader->lengthInSamples - length) / 2;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, length);
    reader->read (&audio, 0, length, start, true, true);
    int crossings = 0;
    for (int i = 1; i < length; ++i)
        crossings += (audio.getSample (channel, i - 1) < 0.0f) != (audio.getSample (channel, i) < 0.0f) ? 1 : 0;
    return crossings / 2.0 / ((double)length / rate);
}

juce::var perform (Session& session, const juce::String& name, const juce::String& json)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
    INFO (error);
    REQUIRE (command != nullptr);
    auto* raw = command.get();
    REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
    return raw->getResult();
}

std::vector<AudioRecorder::Take> record (Session& session, test::RealtimeRunner& runner, double seconds)
{
    auto& recorder = session.getAudioRecorder();
    std::optional<std::vector<AudioRecorder::Take>> takes;
    recorder.onFinished = [&] (const std::vector<AudioRecorder::Take>& t) { takes = t; };
    const auto started = recorder.start();
    INFO (started.getErrorMessage());
    REQUIRE (started.wasOk());
    runner.runUntil (runner.getSecondsProcessed() + seconds);
    session.stop();
    for (int i = 0; i < 100 && ! takes; ++i)
    {
        runner.runUntil (runner.getSecondsProcessed() + 0.02);
        juce::Thread::sleep (2);
    }
    REQUIRE (takes.has_value());
    return takes.value_or (std::vector<AudioRecorder::Take> {});
}
} // namespace

// A sound card with four inputs: the kick mic on input 1, overheads as a stereo pair on 3 + 4, a bass on 2
// (not armed). One Record audio records each armed track from its own input, all in step.
TEST_CASE ("Multi-track recording: each armed track records its own input", "[audiorecord][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    test::RealtimeRunner runner (
        host.getEngine(),
        { .outputChannels = 2, .inputChannels = 4, .maxSeconds = 12.0, .paceInRealTime = true, .inputSource = sines });
    CHECK (recording::activeInputChannels (host.getEngine()) == std::vector<int> { 0, 1, 2, 3 });

    const auto kick = (int)perform (session, "channel.add", R"({"name": "Kick mic", "audioTrack": true})")["channel"];
    const auto overheads =
        (int)perform (session, "channel.add", R"({"name": "Overheads", "audioTrack": true})")["channel"];
    const auto bass = (int)perform (session, "channel.add", R"({"name": "Bass DI", "audioTrack": true})")["channel"];
    CHECK (session.getProject().findChannel (model::ChannelId { (uint32_t)kick })->audioTrack);
    perform (session, "channel.set",
             R"({"channel": )" + juce::String (kick) + R"(, "recordInput": 1, "recordArmed": true})");
    perform (session, "channel.set",
             R"({"channel": )" + juce::String (overheads) +
                 R"(, "recordInput": 3, "recordStereo": true, "recordArmed": true})");
    perform (session, "channel.set", R"({"channel": )" + juce::String (bass) + R"(, "recordInput": 2})");

    runner.start();
    runner.runUntil (0.2);
    // Armed tracks' inputs are metered before recording.
    REQUIRE (session.getAudioRecorder().setMetering (true).wasOk());
    runner.runUntil (0.4);
    CHECK (session.getAudioRecorder().getInputLevel (0) > 0.3f);
    CHECK (session.getAudioRecorder().getInputLevel (40) < 1.0e-6f);

    const auto takes = record (session, runner, 1.5);
    REQUIRE (takes.size() == 2);
    CHECK (takes[0].track == model::ChannelId { (uint32_t)kick });
    CHECK (takes[0].channels == 1);
    CHECK (takes[0].file.getFileName() == "Kick mic 1.wav");
    CHECK (std::abs (frequencyOf (takes[0].file, 0) - 200.0) < 5.0);
    CHECK (takes[1].track == model::ChannelId { (uint32_t)overheads });
    CHECK (takes[1].channels == 2);
    CHECK (std::abs (frequencyOf (takes[1].file, 0) - 600.0) < 5.0);
    CHECK (std::abs (frequencyOf (takes[1].file, 1) - 800.0) < 5.0);
    CHECK (std::abs (takes[0].startSeconds - takes[1].startSeconds) < 1.0e-9); // recorded in step
    CHECK (session.getAudioRecorder().isMetering());

    // An input the sound card doesn't have.
    perform (session, "channel.set", R"({"channel": )" + juce::String (kick) + R"(, "recordInput": 9})");
    CHECK (session.getAudioRecorder().start().getErrorMessage().contains ("no input 9"));
    CHECK (! session.isPlaying());

    // Nothing armed: the input chosen in Options › Audio, for the caller to place.
    for (auto track : { kick, overheads })
        perform (session, "channel.set", R"({"channel": )" + juce::String (track) + R"(, "recordArmed": false})");
    recording::setDefaultInput (host.getEngine(), 1);
    const auto single = record (session, runner, 1.0);
    REQUIRE (single.size() == 1);
    CHECK (! single[0].track.has_value());
    // "Recording 1.wav", or a later number when another test recorded into the same temporary folder.
    CHECK (single[0].file.getFileName().startsWith ("Recording "));
    CHECK (std::abs (frequencyOf (single[0].file, 0) - 400.0) < 5.0);
    runner.stop();
    for (const auto& take : takes)
        take.file.deleteFile();
    single[0].file.deleteFile();
    session.getAudioRecorder().setMetering (false);
}

// A microphone opened through PipeWire joins the system's sound (a webcam's can become its clock, with small
// buffers for every program), so the inputs are on only while something records or meters them. Through
// JUCE's real device handling, on a software sound card with a two-input mic.
TEST_CASE ("The sound card's inputs are on only while something records or meters them", "[audiorecord][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto& engine = host.getEngine();
    test::FakeSoundCard card;
    auto& devices = engine.getDeviceManager();
    devices.deviceManager.addAudioDeviceType (card.createType());
    // A saved setup recording from the mic (as an older Spinus left it).
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", test::FakeSoundCard::typeName);
    saved.setAttribute ("audioOutputDeviceName", "Fake output A");
    saved.setAttribute ("audioInputDeviceName", "Fake mic");
    saved.setAttribute ("audioDeviceInChans", "11");
    saved.setAttribute ("audioDeviceRate", 48000.0);
    saved.setAttribute ("audioDeviceBufferSize", 256);
    engine.getPropertyStorage().setXmlProperty (tracktion::SettingID::audio_device_setup, saved);
    devices.initialise (2, 2);
    REQUIRE (devices.deviceManager.getCurrentAudioDevice() != nullptr);
    using Inputs = std::vector<int>;
    REQUIRE (recording::activeInputChannels (engine) == Inputs { 0, 1 });

    // Nothing records yet: off, the mic still chosen.
    Session session (engine, std::make_unique<test::TestDevices> (engine));
    CHECK (recording::activeInputChannels (engine).empty());
    CHECK (devices.deviceManager.getAudioDeviceSetup().inputDeviceName == "Fake mic");
    CHECK (recording::inputChannelNames (engine) == juce::StringArray { "Mic 1", "Mic 2" });

    // An armed track's input, while it is metered.
    const auto track = (int)perform (session, "channel.add", R"({"name": "Vocal", "audioTrack": true})")["channel"];
    perform (session, "channel.set",
             R"({"channel": )" + juce::String (track) + R"(, "recordInput": 2, "recordArmed": true})");
    auto& recorder = session.getAudioRecorder();
    REQUIRE (recorder.setMetering (true).wasOk());
    CHECK (recording::activeInputChannels (engine) == Inputs { 1 });
    recorder.setMetering (false);
    CHECK (recording::activeInputChannels (engine).empty());

    // Recording: on while it records, off after (what is recorded: the tests above).
    REQUIRE (recorder.start().wasOk());
    CHECK (recording::activeInputChannels (engine) == Inputs { 1 });
    recorder.cancel();
    CHECK (! recorder.isRecording());
    CHECK (recording::activeInputChannels (engine).empty());

    // Another user (Options › Audio open) keeps them on when metering ends.
    {
        recording::InputUse settingsOpen (engine);
        REQUIRE (recording::useInputs (engine, { 0 }).wasOk());
        REQUIRE (recorder.setMetering (true).wasOk());
        recorder.setMetering (false);
        CHECK (recording::activeInputChannels (engine) == Inputs { 0, 1 });
    }
    CHECK (recording::activeInputChannels (engine).empty());
    perform (session, "channel.set", R"({"channel": )" + juce::String (track) + R"(, "recordArmed": false})");
    devices.deviceManager.closeAudioDevice();
}
