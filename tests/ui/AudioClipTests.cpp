#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/ChildProcess.h"
#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "engine/BundledDevices.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "session/ProjectDocument.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
constexpr double rate = 48000.0;

juce::File writeTone (const juce::File& file, double seconds, double frequency = 220.0)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.5f * (float)std::sin (2.0 * M_PI * frequency * i / rate));
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

struct Fixture
{
    Fixture() { root.createDirectory(); }
    ~Fixture()
    {
        document = nullptr;
        playlist = nullptr;
        session = nullptr;
        root.deleteRecursively();
    }

    void makeSession()
    {
        session = std::make_unique<Session> (host.getEngine());
        playlist = std::make_unique<Playlist> (*session);
        playlist->setSize (800, 400);
    }

    const model::Clip& lastClip() const { return session->getProject().playlist.clips.back(); }
    const model::AudioClip& lastAudio() const { return std::get<model::AudioClip> (lastClip().content); }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-audio-clips") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<Playlist> playlist;
    std::unique_ptr<ProjectDocument> document;
};
} // namespace

TEST_CASE ("An audio file becomes an audio clip on its own channel", "[ui][playlist][audioclip]")
{
    Fixture f;
    f.makeSession();
    const auto file = writeTone (f.root.getChildFile ("samples/Vocal Hook.wav"), 2.0);

    REQUIRE (f.playlist->addAudioFile (1, 3840, file));
    const auto& p = f.session->getProject();
    CHECK (p.playlist.lanes.size() == 2);
    CHECK (f.lastClip().start == 3840);
    CHECK (f.lastClip().length == 3840);                  // 2 s at 120 BPM = 4 beats
    CHECK (f.lastAudio().file == file.getFullPathName()); // untitled: the file where it is
    const auto* channel = p.findChannel (f.lastAudio().channel);
    REQUIRE (channel != nullptr);
    CHECK (channel->name == "Vocal Hook");
    CHECK (f.session->getAdapter().describe().contains ("audio clip"));

    // Not audio: nothing is added.
    f.root.getChildFile ("notes.txt").replaceWithText ("hi");
    CHECK (! f.playlist->addAudioFile (0, 0, f.root.getChildFile ("notes.txt")));
    CHECK (! f.playlist->isInterestedInFileDrag ({ f.root.getChildFile ("notes.txt").getFullPathName() }));
    CHECK (f.playlist->isInterestedInFileDrag ({ file.getFullPathName() }));

    // Dropping two files puts them on consecutive tracks.
    const auto second = writeTone (f.root.getChildFile ("Bass.wav"), 1.0);
    const auto clips = p.playlist.clips.size();
    f.playlist->filesDropped ({ file.getFullPathName(), second.getFullPathName() }, Playlist::headerWidth + 10,
                              Playlist::rulerHeight + 5);
    REQUIRE (p.playlist.clips.size() == clips + 2);
    CHECK (p.playlist.clips[clips].lane != p.playlist.clips[clips + 1].lane);
    CHECK (p.playlist.clips[clips].start == p.playlist.clips[clips + 1].start);

    // Painting audio clips (with their waveforms) works.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    CHECK (f.playlist->createComponentSnapshot (f.playlist->getLocalBounds()).isValid());
}

TEST_CASE ("Audio clips play in song mode, and not in pattern mode or when muted", "[ui][playlist][audioclip][audio]")
{
    Fixture f;
    f.makeSession();
    const auto file = writeTone (f.root.getChildFile ("tone.wav"), 3.0);
    REQUIRE (f.playlist->addAudioFile (0, 0, file));

    // Paced like a real device: muting rebuilds the playback graph on the message thread.
    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 8.0, .paceInRealTime = true });
    const auto sr = runner.getParams().sampleRate;
    auto level = [&] (double from, double to)
    { return test::checks::rms (runner.getChannel (0), (int64_t)(from * sr), (int64_t)(to * sr)); };

    f.session->setMode (Session::Mode::song);
    f.session->play();
    runner.start();
    runner.runUntil (1.0);
    f.session->stop();
    CHECK (level (0.2, 0.9) > 0.1f);

    // Pattern mode: the playlist is silent.
    f.session->setMode (Session::Mode::pattern);
    f.session->play();
    runner.runUntil (2.0);
    f.session->stop();
    CHECK (level (1.2, 1.9) < 1.0e-4f);

    // Muted clip.
    f.session->setMode (Session::Mode::song);
    juce::String error;
    auto mute = commands::CommandRegistry::get().create (
        "clip.set",
        juce::JSON::parse ("{\"clip\": " + juce::String ((int)f.lastClip().id.value) + ", \"muted\": true}"), error);
    REQUIRE (mute != nullptr);
    REQUIRE (f.session->getHistory().perform (std::move (mute)).wasOk());
    f.session->play();
    runner.runUntil (3.0);
    f.session->stop();
    runner.stop();
    CHECK (level (2.2, 2.9) < 1.0e-4f);
}

TEST_CASE ("Saving copies audio files into the project, which opens elsewhere", "[ui][playlist][audioclip]")
{
    Fixture f;
    f.makeSession();
    f.document = std::make_unique<ProjectDocument> (*f.session, f.root.getChildFile ("settings"));
    const auto original = writeTone (f.root.getChildFile ("elsewhere/Pad.wav"), 1.0);
    REQUIRE (f.playlist->addAudioFile (0, 0, original));
    CHECK (juce::File::isAbsolutePath (f.lastAudio().file));

    // Untitled -> saved: the file is copied into audio/ and the clip points at the copy.
    REQUIRE (f.document->saveAs (f.root.getChildFile ("Song")).wasOk());
    CHECK (f.lastAudio().file == "Pad.wav");
    CHECK (f.root.getChildFile ("Song/audio/Pad.wav").existsAsFile());
    CHECK (! f.document->hasUnsavedEdits());
    CHECK (f.session->getAdapter().getMissingAudioClips().empty());

    // A saved project copies dropped files at once.
    const auto drums = writeTone (f.root.getChildFile ("elsewhere/Drums.wav"), 0.5);
    REQUIRE (f.playlist->addAudioFile (1, 0, drums));
    CHECK (f.lastAudio().file == "Drums.wav");
    REQUIRE (f.document->save().wasOk());

    // The originals can go: the project still has its audio, also after moving it.
    f.root.getChildFile ("elsewhere").deleteRecursively();
    f.document->newProject(); // closed before moving it
    REQUIRE (f.root.getChildFile ("Song").moveFileTo (f.root.getChildFile ("Moved")));
    REQUIRE (f.document->open (f.root.getChildFile ("Moved")).wasOk());
    CHECK (f.session->getProject().playlist.clips.size() == 2);
    CHECK (f.session->getAdapter().getMissingAudioClips().empty());
    CHECK (f.session->getAdapter().describe().contains ("file=Pad.wav"));

    // A missing file is reported (and the clip is kept).
    f.root.getChildFile ("Moved/audio/Drums.wav").deleteFile();
    f.document->newProject();
    REQUIRE (f.document->open (f.root.getChildFile ("Moved")).wasOk());
    CHECK (f.session->getAdapter().getMissingAudioClips().size() == 1);
    CHECK (f.session->getProject().playlist.clips.size() == 2);
}

TEST_CASE ("A saved project's audio clips are in its exports", "[ui][playlist][audioclip][export]")
{
    Fixture f;
    f.makeSession();
    f.document = std::make_unique<ProjectDocument> (*f.session, f.root.getChildFile ("settings"));
    REQUIRE (f.playlist->addAudioFile (0, 0, writeTone (f.root.getChildFile ("elsewhere/Pad.wav"), 1.0)));
    REQUIRE (f.document->saveAs (f.root.getChildFile ("Song")).wasOk());
    REQUIRE (f.lastAudio().file == "Pad.wav"); // inside the project now
    f.session->setMode (Session::Mode::song);

    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    std::optional<engine::ExportJob::Result> result;
    const auto file = f.root.getChildFile ("Song.wav");
    auto job = f.session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 600 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (result.has_value());
    const auto done = result.value_or (engine::ExportJob::Result {});
    INFO (done.error);
    REQUIRE (done.ok);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio (1, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
    CHECK (audio.getRMSLevel (0, (int)(0.1 * reader->sampleRate), (int)(0.5 * reader->sampleRate)) > 0.1f);
}

TEST_CASE ("Compressed audio files arrive as FLAC and play", "[ui][playlist][audioclip][audio]")
{
    Fixture f;
    f.makeSession();
    f.document = std::make_unique<ProjectDocument> (*f.session, f.root.getChildFile ("settings"));
    REQUIRE (f.document->saveAs (f.root.getChildFile ("Song")).wasOk());

    // An Ogg Vorbis file (MP3 and Opus go the same way): decoded into the project's audio/ folder.
    const auto wav = writeTone (f.root.getChildFile ("elsewhere/Tone.wav"), 2.0);
    const auto ogg = f.root.getChildFile ("elsewhere/Tone.ogg");
    {
        std::unique_ptr<juce::AudioFormatReader> reader (
            juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (wav), true));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (ogg);
        auto writer = juce::OggVorbisAudioFormat().createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withQualityOptionIndex (5));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples));
    }
    REQUIRE (f.playlist->addAudioFile (0, 0, ogg));
    CHECK (f.lastAudio().file == "Tone.flac");
    CHECK (f.root.getChildFile ("Song/audio/Tone.flac").existsAsFile());
    CHECK (f.lastClip().length == 3840); // 2 s at 120 BPM

    // The same file again uses the same decoded copy.
    REQUIRE (f.playlist->addAudioFile (1, 0, ogg));
    CHECK (f.lastAudio().file == "Tone.flac");

    // With ffmpeg, formats Spinus cannot read itself (M4A here) work too.
    spinus::core::ChildProcess ffmpeg;
    const auto m4a = f.root.getChildFile ("elsewhere/Tone.m4a");
    if (ffmpeg.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", wav.getFullPathName(),
                                          m4a.getFullPathName() }) &&
        ffmpeg.waitForProcessToFinish (30000) && m4a.existsAsFile())
    {
        REQUIRE (f.playlist->isInterestedInFileDrag ({ m4a.getFullPathName() }));
        REQUIRE (f.playlist->addAudioFile (2, 0, m4a));
        CHECK (f.lastAudio().file == "Tone(2).flac"); // a different file with the same name
    }

    // It plays.
    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = true });
    const auto sr = runner.getParams().sampleRate;
    f.session->setMode (Session::Mode::song);
    f.session->play();
    runner.start();
    runner.runUntil (1.0);
    f.session->stop();
    runner.stop();
    CHECK (test::checks::rms (runner.getChannel (0), (int64_t)(0.2 * sr), (int64_t)(0.9 * sr)) > 0.1f);
}

namespace
{
/** Exports the song as 48 kHz float and returns its left channel. */
std::vector<float> exportSong (Fixture& f, const juce::String& name)
{
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    settings.sampleRate = rate;
    settings.tailSeconds = 0.5;
    std::optional<engine::ExportJob::Result> result;
    const auto file = f.root.getChildFile (name + ".wav");
    auto job = f.session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (result.has_value());
    const auto done = result.value_or (engine::ExportJob::Result {});
    INFO (done.error);
    REQUIRE (done.ok);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return { audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples() };
}

float rmsBetween (const std::vector<float>& x, double from, double to)
{
    return test::checks::rms (x, (int64_t)(from * rate), (int64_t)(to * rate));
}

/** Cycles per second from upward zero crossings, between two times. */
double frequencyBetween (const std::vector<float>& x, double from, double to)
{
    int crossings = 0;
    for (auto i = (size_t)(from * rate) + 1; i < (size_t)(to * rate) && i < x.size(); ++i)
        crossings += x[i - 1] < 0.0f && x[i] >= 0.0f ? 1 : 0;
    return crossings / (to - from);
}
} // namespace

TEST_CASE ("Audio clips play reversed, pitched and stretched", "[ui][playlist][audioclip][export]")
{
    Fixture f;
    f.makeSession();
    // Two seconds of 220 Hz that fades in: quiet at the start, loud at the end.
    const auto file = f.root.getChildFile ("Rise.wav");
    {
        juce::AudioBuffer<float> audio (1, (int)(2.0 * rate));
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (
                0, i, 0.5f * (float)i / (float)audio.getNumSamples() * (float)std::sin (2.0 * M_PI * 220.0 * i / rate));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    REQUIRE (f.playlist->addAudioFile (0, 0, file));
    f.session->setMode (Session::Mode::song);
    const auto clip = f.lastClip().id;
    auto set = [&] (const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (
            "clip.setAudio", juce::JSON::parse (R"({"clip": )" + juce::String (clip.value) + ", " + json + "}"), error);
        INFO (error);
        REQUIRE (command != nullptr);
        REQUIRE (f.session->getHistory().perform (std::move (command)).wasOk());
    };

    const auto plain = exportSong (f, "Plain");
    CHECK (rmsBetween (plain, 1.6, 1.9) > rmsBetween (plain, 0.1, 0.4) * 3.0f);

    // Backwards: loud first.
    set (R"("reversed": true)");
    CHECK (f.session->getHistory().getUndoDescriptions().contains ("Reverse clip"));
    const auto reversed = exportSong (f, "Reversed");
    CHECK (rmsBetween (reversed, 0.1, 0.4) > rmsBetween (reversed, 1.6, 1.9) * 3.0f);
    set (R"("reversed": false)");

    // An octave up, as long as before (Rubber Band).
    set (R"("pitch": 12)");
    CHECK (f.lastAudio().stretchMode == "stretch");
    const auto higher = exportSong (f, "Higher");
    CHECK (frequencyBetween (higher, 1.0, 1.8) == Catch::Approx (440.0).epsilon (0.03));
    CHECK (rmsBetween (higher, 1.6, 1.9) > 0.05f); // still sounding near the end

    // Twice as fast at the same pitch: over in one second.
    set (R"("pitch": 0, "speed": 2)");
    const auto faster = exportSong (f, "Faster");
    CHECK (frequencyBetween (faster, 0.5, 0.9) == Catch::Approx (220.0).epsilon (0.03));
    CHECK (rmsBetween (faster, 1.2, 1.9) < 0.01f);

    // Resampled, like a record: an octave up is twice as fast too.
    set (R"("speed": 1, "pitch": 12, "stretch": "resample")");
    const auto record = exportSong (f, "Record");
    CHECK (frequencyBetween (record, 0.5, 0.9) == Catch::Approx (440.0).epsilon (0.03));
    CHECK (rmsBetween (record, 1.2, 1.9) < 0.01f);

    // Saved and loaded with the project.
    const auto json = model::toJsonText (f.session->getProject());
    CHECK (json.contains ("\"resample\""));
    CHECK (json.contains ("\"pitchSemitones\": 12"));
}

TEST_CASE ("A cut audio clip plays the part of its file it shows", "[ui][playlist][audioclip][export]")
{
    Fixture f;
    f.makeSession();
    // One second of 220 Hz, then one second of 440 Hz.
    const auto file = f.root.getChildFile ("Low then high.wav");
    {
        juce::AudioBuffer<float> audio (1, (int)(2.0 * rate));
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (0, i, 0.5f * (float)std::sin (2.0 * M_PI * (i < (int)rate ? 220.0 : 440.0) * i / rate));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    REQUIRE (f.playlist->addAudioFile (0, 0, file));
    f.session->setMode (Session::Mode::song);
    const auto first = f.lastClip().id;
    auto run = [&] (const char* name, const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
        INFO (error);
        REQUIRE (command != nullptr);
        REQUIRE (f.session->getHistory().perform (std::move (command)).wasOk());
    };

    // Cut in two at one second (120 BPM: 1920 ticks), and the second half moved to three seconds.
    run ("clip.split", R"({"clip": )" + juce::String (first.value) + R"(, "at": 1920})");
    const auto second = f.lastClip().id;
    REQUIRE (second != first);
    run ("clip.set", R"({"clip": )" + juce::String (second.value) + R"(, "start": 5760})");
    const auto cut = exportSong (f, "Cut");
    CHECK (frequencyBetween (cut, 0.2, 0.8) == Catch::Approx (220.0).epsilon (0.03));
    CHECK (rmsBetween (cut, 1.2, 2.8) < 0.01f);
    CHECK (frequencyBetween (cut, 3.2, 3.8) == Catch::Approx (440.0).epsilon (0.03));
}

TEST_CASE ("A pitched audio clip plays pitched while the song plays", "[ui][playlist][audioclip][audio]")
{
    Fixture f;
    f.makeSession();
    REQUIRE (f.playlist->addAudioFile (0, 0, writeTone (f.root.getChildFile ("Tone.wav"), 2.0)));
    f.session->setMode (Session::Mode::song);
    juce::String error;
    auto command = commands::CommandRegistry::get().create (
        "clip.setAudio",
        juce::JSON::parse (R"({"clip": )" + juce::String (f.lastClip().id.value) + R"(, "pitch": 12})"), error);
    REQUIRE (command != nullptr);
    REQUIRE (f.session->getHistory().perform (std::move (command)).wasOk());

    // Pitched at once: stretched as it plays, with no copy to wait for.
    auto& engine = f.host.getEngine();

    // In real time: the file is read ahead on a thread of its own, as when playing.
    test::RealtimeRunner runner (engine, { .outputChannels = 2, .maxSeconds = 2.0, .paceInRealTime = true });
    f.session->getEdit().dispatchPendingUpdatesSynchronously();
    f.session->play();
    runner.start();
    runner.runUntil (1.5);
    runner.stop();
    f.session->stop();
    CHECK (frequencyBetween (runner.getChannel (0), 0.5, 1.4) == Catch::Approx (440.0).epsilon (0.03));
}

TEST_CASE ("Stretching an audio clip keeps its audio: faster is shorter", "[ui][playlist][audioclip]")
{
    Fixture f;
    f.makeSession();
    REQUIRE (f.playlist->addAudioFile (0, 0, writeTone (f.root.getChildFile ("Loop.wav"), 2.0)));
    const auto id = f.lastClip().id;
    REQUIRE (f.lastClip().length == 3840);

    // Alt+drag of the end to half the length: twice as fast, in Rubber Band's stretch (pitch kept).
    REQUIRE (f.playlist->stretchClips ({ id }, -1920));
    CHECK (f.lastClip().length == 1920);
    CHECK (f.lastAudio().speed == Catch::Approx (2.0));
    CHECK (f.lastAudio().stretchMode == "stretch");
    CHECK (f.session->getHistory().getUndoDescriptions().contains ("Stretch clip"));

    // Like a record: an octave up plays twice as fast again, so the clip halves with "fit".
    REQUIRE (f.playlist->setAudioPlayback (id, { { "stretch", "resample" }, { "pitch", 12 }, { "fit", true } }));
    CHECK (f.lastClip().length == 960);
    // Back to as recorded: the whole file again.
    REQUIRE (f.playlist->setAudioPlayback (id, { { "stretch", "off" }, { "fit", true } }));
    CHECK (f.lastClip().length == 3840);

    // One undo step each.
    REQUIRE (f.session->getHistory().undo());
    CHECK (f.lastClip().length == 960);
    CHECK (f.lastAudio().stretchMode == "resample");

    // Not an audio clip, or out of range: refused.
    juce::String error;
    CHECK (commands::CommandRegistry::get().create (
               "clip.setAudio", juce::JSON::parse (R"({"clip": 999999, "speed": 9})"), error) == nullptr);
}

TEST_CASE ("Recording into a track stacks takes; Alt+click chooses which plays where",
           "[ui][playlist][audioclip][takes]")
{
    Fixture f;
    f.makeSession();
    // Take 1: two seconds (four beats) on a new track.
    REQUIRE (f.playlist->addRecordedTake (writeTone (f.root.getChildFile ("Recording 1.wav"), 2.0), 0.0));
    const auto& project = f.session->getProject();
    const auto channel = f.lastAudio().channel;
    const auto first = f.lastClip();
    REQUIRE (first.length == 3840);

    // Take 2, from 1 s to 3 s, into the same track: on "Take 2" under the first take's track; the first take
    // is cut at 1 s and its second half muted.
    REQUIRE (f.playlist->addRecordedTake (writeTone (f.root.getChildFile ("Recording 2.wav"), 2.0), 1.0, channel));
    CHECK (f.session->getHistory().getUndoDescriptions().contains ("Record take 2"));
    const auto take2 = f.lastClip();
    CHECK (std::get<model::AudioClip> (take2.content).channel == channel);
    CHECK (take2.start == 1920);
    const auto* lane = project.findLane (take2.lane);
    REQUIRE (lane != nullptr);
    CHECK (lane->name == "Take 2");
    CHECK (lane->group == first.lane);
    auto clipAt = [&] (model::LaneId l, model::Ticks start) -> const model::Clip*
    {
        for (const auto& c : project.playlist.clips)
            if (c.lane == l && c.start == start)
                return &c;
        return nullptr;
    };
    REQUIRE (clipAt (first.lane, 0) != nullptr);
    CHECK (! clipAt (first.lane, 0)->muted);
    CHECK (clipAt (first.lane, 0)->length == 1920);
    REQUIRE (clipAt (first.lane, 1920) != nullptr);
    CHECK (clipAt (first.lane, 1920)->muted);
    CHECK (std::get<model::AudioClip> (clipAt (first.lane, 1920)->content).offset == 1920);
    CHECK (f.playlist->hasOtherTakes (clipAt (first.lane, 1920)->id));

    // Comping: the first take's second half again; take 2 is cut at 2 s and muted up to there.
    REQUIRE (f.playlist->useTake (clipAt (first.lane, 1920)->id));
    CHECK (! clipAt (first.lane, 1920)->muted);
    REQUIRE (clipAt (take2.lane, 1920) != nullptr);
    CHECK (clipAt (take2.lane, 1920)->muted);
    REQUIRE (clipAt (take2.lane, 3840) != nullptr);
    CHECK (! clipAt (take2.lane, 3840)->muted); // after the first take ends, take 2 still plays
    CHECK (f.session->getHistory().getUndoDescriptions().contains ("Use take"));
    REQUIRE (f.session->getHistory().undo());
    CHECK (clipAt (first.lane, 1920)->muted);

    // Into the track where it has no audio yet: on the first take's track, no new take.
    const auto lanes = project.playlist.lanes.size();
    REQUIRE (f.playlist->addRecordedTake (writeTone (f.root.getChildFile ("Recording 3.wav"), 1.0), 6.0, channel));
    CHECK (project.playlist.lanes.size() == lanes);
    CHECK (f.lastClip().lane == first.lane);
}
