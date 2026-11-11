/*  Edge cases from the 0.1.0-rc.1 checklist (Piano Roll and Channel Rack, Playlist, Mixer, Export): notes at the edges
   of a pattern and of the keyboard, stuck notes, swing, export formats, long songs, audio clips at another sample rate,
   track groups, which of mute and solo wins, routing loops, faders all the way down, sidechains, removed send targets,
   delay compensation, and mono, stereo and many-output sources. */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "core/ChildProcess.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "engine/Exporter.h"
#include "model/ModelJson.h"
#include "model/NoteTools.h"
#include "plugins/SandboxList.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"
#include "ui/PianoRoll.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::model;

namespace
{
constexpr double rate = 48000.0;
constexpr Ticks beat = ticksPerQuarterNote, bar = 4 * ticksPerQuarterNote; // 120 BPM: a beat is 0.5 s, a bar 2 s

/** A Session with Spinus's own devices (3x Osc, Spinus Delay...) and the test synth (a CLAP), optionally in its
    own process. Commands are run as the UI runs them. */
struct Song
{
    explicit Song (bool synthSeparately = false)
    {
        root.createDirectory();
        plugins::ClapPluginFormat clap;
        for (const auto& file : { synthFile(), synthFile ("mono-synth"), synthFile ("multi-out-synth") })
        {
            juce::OwnedArray<juce::PluginDescription> found;
            clap.findAllTypesForFile (found, file.getFullPathName());
            for (auto* d : found)
                if (d->name != "Spinus Test Organ")
                    host.getEngine().getPluginManager().knownPluginList.addType (*d);
        }

        if (synthSeparately)
            list.set (plugins::SandboxList::keyOf ("CLAP", synthFile().getFullPathName()), true, "Spinus Test Synth");
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        devices->setRunSeparately (
            [this] (const juce::PluginDescription& d)
            { return list.contains (plugins::SandboxList::keyOf (d.pluginFormatName, d.fileOrIdentifier)); });
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        session->setSandboxList (&list);
    }

    ~Song()
    {
        document = nullptr;
        session = nullptr;
        root.deleteRecursively();
    }

    static juce::File synthFile (const juce::String& name = "test-synth")
    {
        return juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile (name + ".clap");
    }

    juce::var run (const juce::String& name, const juce::var& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, params, error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        const auto result = session->getHistory().perform (std::move (command));
        INFO (name << ": " << result.getErrorMessage());
        REQUIRE (result.wasOk());
        return raw->getResult();
    }
    juce::var run (const juce::String& name, const juce::String& json) { return run (name, juce::JSON::parse (json)); }

    bool tryRun (const juce::String& name, const juce::var& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, params, error);
        return command != nullptr && session->getHistory().perform (std::move (command)).wasOk();
    }

    /** A channel with a stock device ("spinus.3x-osc") or, with an empty type, the test synth (or another of
        the test synths: `synth` is its file's name, `synthName` its name). */
    ChannelId addChannel (const juce::String& stockType = "spinus.3x-osc", InsertId insert = {},
                          const juce::String& synth = "test-synth", const juce::String& synthName = "Spinus Test Synth")
    {
        juce::String instrument = stockType.isNotEmpty()
                                      ? R"({"kind": "stock", "stockType": ")" + stockType + "\"}"
                                      : R"({"kind": "plugin", "plugin": {"format": "CLAP", "name": )" +
                                            juce::JSON::toString (synthName) +
                                            R"(, "manufacturer": "Spinus tests", "fileOrIdentifier": )" +
                                            juce::JSON::toString (synthFile (synth).getFullPathName()) + "}}";
        const auto made =
            run ("channel.add",
                 R"({"name": "Ch", "instrument": )" + instrument +
                     (insert.isValid() ? ", \"insert\": " + juce::String (insert.value) : juce::String()) + "}");
        lastInsert = InsertId { (uint32_t)(int)made["insert"] };
        lastDevice = DeviceId { (uint32_t)(int)made["device"] };
        return ChannelId { (uint32_t)(int)made["channel"] };
    }

    PatternId addPattern (Ticks length = bar)
    {
        return PatternId { (uint32_t)(int)run ("pattern.add", R"({"name": "P", "length": )" + juce::String (length) +
                                                                  "}")["pattern"] };
    }

    void setNotes (PatternId pattern, ChannelId channel, const std::vector<Note>& notes)
    {
        juce::Array<juce::var> json;
        for (auto& n : notes)
            json.add (noteToJson (n));
        auto* p = new juce::DynamicObject();
        p->setProperty ("pattern", (int)pattern.value);
        p->setProperty ("channel", (int)channel.value);
        p->setProperty ("notes", json);
        run ("pattern.setNotes", juce::var (p));
    }

    LaneId addLane() { return LaneId { (uint32_t)(int)run ("lane.add", juce::String ("{}"))["lane"] }; }

    ClipId place (LaneId lane, PatternId pattern, Ticks start, std::optional<Ticks> length = {})
    {
        return ClipId { (uint32_t)(int)run ("clip.addPattern",
                                            R"({"lane": )" + juce::String (lane.value) + R"(, "pattern": )" +
                                                juce::String (pattern.value) + R"(, "start": )" + juce::String (start) +
                                                (length ? ", \"length\": " + juce::String (*length) : juce::String()) +
                                                "}")["clip"] };
    }

    struct Rendered
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 0;
        unsigned int bits = 0;
        double seconds() const { return audio.getNumSamples() / sampleRate; }
        /** RMS of both channels between two times. */
        float rms (double from, double to) const
        {
            const auto a = juce::jlimit (0, audio.getNumSamples(), (int)(from * sampleRate));
            const auto b = juce::jlimit (0, audio.getNumSamples(), (int)(to * sampleRate));
            if (b <= a)
                return 0.0f;
            return juce::jmax (audio.getRMSLevel (0, a, b - a), audio.getRMSLevel (1, a, b - a));
        }
        std::vector<float> left() const
        {
            return { audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples() };
        }
    };

    engine::ExportJob::Result exportTo (const juce::File& file, engine::ExportSettings settings,
                                        bool cancelHalfway = false)
    {
        std::optional<engine::ExportJob::Result> result;
        auto job = session->createExportJob (file, std::move (settings),
                                             [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        bool cancelled = false;
        for (int i = 0; i < 6000 && ! result; ++i)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            if (cancelHalfway && ! cancelled && job->getProgress() > 0.4)
            {
                job->cancel();
                cancelled = true;
            }
        }
        REQUIRE (result.has_value());
        return result.value_or (engine::ExportJob::Result {});
    }

    /** Exports the song (or the settings' pattern) as 32-bit float WAV and reads it back. */
    Rendered render (engine::ExportSettings settings = {}, const juce::String& name = "Render")
    {
        if (settings.format == engine::ExportSettings::Format::wav24)
            settings.format = engine::ExportSettings::Format::wav32Float;
        const auto file = root.getChildFile (name + ".wav");
        const auto result = exportTo (file, settings);
        INFO (result.error);
        REQUIRE (result.ok);
        return read (result.files.front());
    }

    static Rendered read (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        REQUIRE (reader != nullptr);
        Rendered r;
        r.sampleRate = reader->sampleRate;
        r.bits = reader->bitsPerSample;
        r.audio.setSize (2, (int)reader->lengthInSamples);
        reader->read (&r.audio, 0, r.audio.getNumSamples(), 0, true, true);
        return r;
    }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-edge-cases-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false, .pluginHost = juce::File (SPINUS_PLUGIN_HOST) } };
    plugins::SandboxList list { root.getChildFile ("run-separately.json") };
    std::unique_ptr<Session> session;
    std::unique_ptr<ProjectDocument> document;
    InsertId lastInsert;
    DeviceId lastDevice;
};

Note note (Ticks start, Ticks length, int pitch = 60, float velocity = 0.8f)
{
    return { start, length, pitch, velocity, 0.5f, 0.0f, 0.0f, {} };
}

bool allFinite (const juce::AudioBuffer<float>& audio)
{
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            if (! std::isfinite (audio.getSample (c, i)))
                return false;
    return true;
}

/** A realtime player for a Song: plays and records what the sound card would get. */
struct Player
{
    explicit Player (Song& s, double maxSeconds = 12.0)
        : song (s),
          runner (s.host.getEngine(), { .outputChannels = 2, .maxSeconds = maxSeconds, .paceInRealTime = true })
    {
    }
    ~Player() { runner.stop(); }

    float level (double from, double to) const
    {
        const auto sr = runner.getParams().sampleRate;
        return juce::jmax (test::checks::rms (runner.getChannel (0), (int64_t)(from * sr), (int64_t)(to * sr)),
                           test::checks::rms (runner.getChannel (1), (int64_t)(from * sr), (int64_t)(to * sr)));
    }
    double now() const { return runner.getSecondsProcessed(); }
    void runFor (double seconds) { runner.runUntil (now() + seconds); }

    /** Starts playing in time with the audio thread: returns the runner's time where the song starts. */
    double play()
    {
        if (! started)
        {
            runner.start();
            started = true;
            runner.runUntil (0.2); // the device is open and the graph built
        }
        else
            runner.runUntil (now() + 0.3); // edits made since reach the playback graph first, as a person is slower
        double at = 0;
        runner.betweenBlocks (
            [&]
            {
                song.session->play();
                at = now();
            });
        return at;
    }

    Song& song;
    test::RealtimeRunner runner;
    bool started = false;
};
} // namespace

//==============================================================================
// 3. Piano Roll and Channel Rack edges

TEST_CASE ("Notes at tick 0, past the pattern's first bar and at the keyboard's ends play and export",
           "[engine][edges][notes]")
{
    Song song;
    const auto channel = song.addChannel();
    const auto pattern = song.addPattern();
    // At the very start, and in the third bar of a one-bar pattern: the pattern grows to fit it.
    song.setNotes (pattern, channel, { note (0, beat), note (2 * bar + beat, beat) });
    CHECK (song.session->getProject().findPattern (pattern)->length == 3 * bar);
    song.place (song.addLane(), pattern, 0);
    song.session->setMode (Session::Mode::song);

    auto rendered = song.render();
    CHECK (rendered.seconds() == Catch::Approx (6.0 + 2.0).margin (0.05));
    CHECK (rendered.rms (0.0, 0.05) > 0.01f); // the first note from the very first sample
    CHECK (rendered.rms (1.5, 4.4) < 1.0e-4f);
    CHECK (rendered.rms (4.5, 5.0) > 0.01f); // the note in bar 3

    // Pattern export too.
    engine::ExportSettings patternOnly;
    patternOnly.pattern = pattern;
    rendered = song.render (patternOnly, "Pattern");
    CHECK (rendered.rms (0.0, 0.05) > 0.01f);
    CHECK (rendered.rms (4.5, 5.0) > 0.01f);

    // The lowest and the highest key, with the highest velocity, and a tiny and a full-length note: no crash,
    // nothing but numbers, and no louder than full scale.
    song.setNotes (
        pattern, channel,
        { note (0, bar, 0, 1.0f), note (0, bar, 127, 1.0f), note (bar, 1, 127, 1.0f), note (bar, 1, 0, 1.0f) });
    rendered = song.render ({}, "Edges");
    CHECK (allFinite (rendered.audio));
    CHECK (rendered.audio.getMagnitude (0, rendered.audio.getNumSamples()) < 2.0f);
    CHECK (rendered.rms (0.2, 1.8) > 1.0e-3f); // the keyboard's ends still sound
}

TEST_CASE ("A note can't be shorter than a tick; a one-tick note plays", "[engine][edges][notes]")
{
    Song song;
    const auto channel = song.addChannel();
    const auto pattern = song.addPattern();
    auto* p = new juce::DynamicObject();
    p->setProperty ("pattern", (int)pattern.value);
    p->setProperty ("channel", (int)channel.value);
    juce::Array<juce::var> zero;
    zero.add (noteToJson (note (0, 0)));
    p->setProperty ("notes", zero);
    CHECK (! song.tryRun ("pattern.setNotes", juce::var (p)));

    // The Piano Roll: resizing a note to nothing leaves it at least a tick long.
    ui::PianoRoll roll (*song.session);
    roll.setSize (760, 420);
    song.session->setSelectedChannel (channel);
    song.session->setCurrentPattern (pattern);
    roll.handleUpdateNowIfNeeded();
    REQUIRE (roll.addNote (60, 0, 240, 0.8f));
    roll.setSnapChoice (ui::PianoRoll::Snap::none);
    roll.resizeNote (0, 0);
    roll.handleUpdateNowIfNeeded();
    REQUIRE (roll.getNotes().size() == 1);
    CHECK (roll.getNotes()[0].length >= 1);
    roll.selectAll();
    roll.resizeSelection (-1000);
    roll.handleUpdateNowIfNeeded();
    REQUIRE (roll.getNotes().size() == 1);
    CHECK (roll.getNotes()[0].length >= 1);

    song.setNotes (pattern, channel, { note (0, 1) });
    song.place (song.addLane(), pattern, 0);
    song.session->setMode (Session::Mode::song);
    const auto rendered = song.render();
    CHECK (rendered.rms (0.0, 0.2) > 1.0e-3f);
    CHECK (rendered.rms (1.0, 2.0) < 1.0e-4f);
}

TEST_CASE ("Notes on the same key at the same time leave no stuck note", "[engine][edges][notes]")
{
    for (const juce::String instrument : { "spinus.3x-osc", "" })
    {
        INFO ((instrument.isEmpty() ? "test synth (CLAP)" : instrument));
        Song song;
        const auto channel = song.addChannel (instrument);
        const auto pattern = song.addPattern (2 * bar);
        // Same start, different lengths; one inside another; two overlapping; a note again right as one ends.
        song.setNotes (pattern, channel,
                       { note (0, beat), note (0, 2 * beat), note (bar, 3 * beat), note (bar + beat / 2, beat / 2),
                         note (bar + 2 * beat, beat), note (bar + 2 * beat + beat / 2, beat),
                         note (2 * bar - beat, beat) });
        song.place (song.addLane(), pattern, 0);
        song.session->setMode (Session::Mode::song);
        const auto rendered = song.render();
        CHECK (rendered.rms (0.1, 0.9) > 0.01f);
        CHECK (rendered.rms (1.5, 2.0) < 1.0e-4f); // the longer of the two notes at 0 ended at 1 s
        CHECK (rendered.rms (4.5, 6.0) < 1.0e-4f); // after the song: nothing left sounding
    }
}

TEST_CASE ("No stuck notes: a long note across the loop's end, and Stop in the middle of a note",
           "[engine][edges][notes][audio]")
{
    for (int kind = 0; kind < 3; ++kind)
    {
        const bool plugin = kind > 0, separately = kind == 2;
        INFO ((kind == 0 ? "3x Osc" : kind == 1 ? "test synth" : "test synth in its own process"));
        Song song (separately);
        const auto channel = song.addChannel (plugin ? "" : "spinus.3x-osc");
        if (separately)
            CHECK (song.session->runsSeparately (song.lastDevice));
        const auto pattern = song.addPattern (2 * bar);
        song.place (song.addLane(), pattern, 0);
        song.session->setMode (Session::Mode::song);
        Player player (song, 20.0);

        // A note from beat 4 of bar 1 to beat 4 of bar 2, with the song looping bar 1: the loop cuts it each time.
        song.setNotes (pattern, channel, { note (3 * beat, bar) });
        song.session->setLoopRegion (Session::Mode::song, Session::LoopRegion { 0, bar });
        auto start = player.play();
        player.runFor (4.2); // two passes
        CHECK (player.level (start + 1.6, start + 1.95) > 0.01f);
        CHECK (player.level (start + 2.4, start + 3.4) < 1.0e-3f); // the second pass, before the note again
        song.session->stop();
        player.runFor (0.6);
        CHECK (player.level (player.now() - 0.2, player.now()) < 1.0e-4f);
        song.session->setLoopRegion (Session::Mode::song, std::nullopt);

        // The same in pattern mode, with the Piano Roll's loop over the first bar.
        song.session->setMode (Session::Mode::pattern);
        song.session->setCurrentPattern (pattern);
        song.session->setLoopRegion (Session::Mode::pattern, Session::LoopRegion { 0, bar });
        start = player.play();
        player.runFor (4.2);
        CHECK (player.level (start + 1.6, start + 1.95) > 0.01f);
        CHECK (player.level (start + 2.4, start + 3.4) < 1.0e-3f);
        song.session->stop();
        player.runFor (0.6);
        CHECK (player.level (player.now() - 0.2, player.now()) < 1.0e-4f);
        song.session->setLoopRegion (Session::Mode::pattern, std::nullopt);
        song.session->setMode (Session::Mode::song);

        // Stop in the middle of a long note.
        song.setNotes (pattern, channel, { note (0, 2 * bar) });
        start = player.play();
        player.runFor (1.0);
        CHECK (player.level (start + 0.5, start + 0.9) > 0.01f);
        song.session->stop();
        player.runFor (0.8);
        CHECK (player.level (player.now() - 0.3, player.now()) < 1.0e-4f);
    }
}

TEST_CASE ("Copy and paste notes into another pattern and another project", "[ui][edges][notes]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine());
    ui::ChannelRack rack (session);
    ui::PianoRoll roll (session);
    roll.setSize (760, 420);
    REQUIRE (rack.addChannel ({}, "Lead"));
    REQUIRE (roll.addNote (60, 0, 480, 0.8f));
    REQUIRE (roll.addNote (64, 480, 480, 0.6f));
    roll.clearSelection();
    REQUIRE (roll.copySelection());

    juce::String error;
    auto add = commands::CommandRegistry::get().create ("pattern.add", juce::JSON::parse (R"({"name": "B"})"), error);
    auto* raw = add.get();
    REQUIRE (session.getHistory().perform (std::move (add)).wasOk());
    session.setCurrentPattern (PatternId { (uint32_t)(int)raw->getResult()["pattern"] });
    session.dispatchPendingMessages();
    roll.handleUpdateNowIfNeeded();
    REQUIRE (roll.getNotes().empty());
    REQUIRE (roll.paste());
    REQUIRE (roll.getNotes().size() == 2);
    CHECK (roll.getNotes()[1].velocity == Catch::Approx (0.6f));

    // Another project in the same window (File › New): the copied notes are still there to paste.
    session.getHistory().reset (Project {});
    session.dispatchPendingMessages();
    REQUIRE (rack.addChannel ({}, "Keys"));
    session.dispatchPendingMessages();
    roll.handleUpdateNowIfNeeded();
    REQUIRE (roll.paste());
    roll.handleUpdateNowIfNeeded();
    CHECK (roll.getNotes().size() == 2);
}

TEST_CASE ("Velocity, release, pan, fine pitch and volume survive Chop, Quantize and the other note tools",
           "[ui][edges][notes]")
{
    namespace nt = noteTools;
    Note n { 130, 960, 62, 0.33f, 0.2f, -0.7f, 25.0f, {} };
    n.setVolume (0.4f);
    auto same = [&] (const Note& m)
    {
        return m.pitch == n.pitch && juce::approximatelyEqual (m.velocity, n.velocity) &&
               juce::approximatelyEqual (m.releaseVelocity, n.releaseVelocity) &&
               juce::approximatelyEqual (m.pan, n.pan) &&
               juce::approximatelyEqual (m.finePitchCents, n.finePitchCents) && m.expressions == n.expressions;
    };
    for (auto& m : nt::chop ({ n }, 240))
        CHECK (same (m));
    for (auto& m : nt::quantize ({ n }, 240))
        CHECK (same (m));
    for (auto& m : nt::legato ({ n, Note { 2000, 100, 62, 0.33f, 0.2f, -0.7f, 25.0f, n.expressions } }))
        CHECK (same (m));
    for (auto& m : nt::arpeggiate ({ n }, 240))
        CHECK (same (m));
    for (auto& m : nt::strum ({ n }, 40, true))
        CHECK (same (m));
}

TEST_CASE ("Swing plays the same in playback and in the export", "[engine][edges][notes][audio]")
{
    Song song;
    const auto channel = song.addChannel ("");
    const auto pattern = song.addPattern();
    // Steps 1 and 2 (the second one swung), short notes.
    song.setNotes (pattern, channel, { note (0, 60), note (240, 60) });
    song.run ("pattern.setSwing", R"({"pattern": )" + juce::String (pattern.value) + R"(, "swing": 0.8})");
    const auto delay = song.session->getProject().findPattern (pattern)->getSwingDelay (240);
    REQUIRE (delay > 0);
    song.place (song.addLane(), pattern, 0);
    song.session->setMode (Session::Mode::song);

    auto secondOnset = [] (const std::vector<float>& x, int64_t from, double sr)
    {
        const auto first = test::checks::findOnset (x, from, (int64_t)x.size(), 0.01f);
        REQUIRE (first >= 0);
        // The second note: the first sound after 0.08 s of the first note (a step is 0.125 s).
        const auto second = test::checks::findOnset (x, first + (int64_t)(0.1 * sr), (int64_t)x.size(), 0.01f);
        REQUIRE (second >= 0);
        return (double)(second - first) / sr;
    };

    const auto exported = song.render();
    const auto exportGap = secondOnset (exported.left(), 0, exported.sampleRate);
    const auto expected = 0.125 + (double)delay / ticksPerQuarterNote * 0.5;
    CHECK (exportGap == Catch::Approx (expected).margin (0.003));

    Player player (song, 6.0);
    const auto start = player.play();
    player.runFor (1.0);
    song.session->stop();
    const auto playedGap = secondOnset (player.runner.getChannel (0), (int64_t)(start * rate), rate);
    CHECK (playedGap == Catch::Approx (exportGap).margin (0.003));
}

//==============================================================================
// 4. Playlist edges

namespace
{
/** A sine at `frequency`, written at `fileRate` (16-bit mono). `fade`: rising from silence to loud. */
juce::File writeSine (const juce::File& file, double seconds, double frequency, double fileRate, bool fade = false)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * fileRate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i,
                         (fade ? (float)i / (float)audio.getNumSamples() : 1.0f) * 0.5f *
                             (float)std::sin (2.0 * M_PI * frequency * i / fileRate));
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (fileRate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

/** Cycles per second from upward zero crossings between two times. */
double frequencyOf (const std::vector<float>& x, double sampleRate, double from, double to)
{
    int crossings = 0;
    int64_t first = -1, last = -1;
    for (auto i = (size_t)(from * sampleRate) + 1; i < (size_t)(to * sampleRate) && i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (first < 0)
                first = (int64_t)i;
            last = (int64_t)i;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) / ((double)(last - first) / sampleRate) : 0.0;
}

/** Where the sound ends: the last sample louder than `threshold`, in seconds. */
double lastSound (const std::vector<float>& x, double sampleRate, float threshold = 1.0e-3f)
{
    for (auto i = (int64_t)x.size() - 1; i >= 0; --i)
        if (std::abs (x[(size_t)i]) > threshold)
            return (double)i / sampleRate;
    return 0.0;
}
} // namespace

TEST_CASE ("A 20-minute song of 200 clips: editing, drawing and exporting keep up", "[engine][edges][export][.slow]")
{
    Song song;
    const auto channel = song.addChannel();
    const auto pattern = song.addPattern (3 * bar);
    song.setNotes (pattern, channel, { note (0, beat), note (bar, beat, 64), note (2 * bar, beat, 67) });
    const auto lane = song.addLane();
    song.session->setMode (Session::Mode::song); // the Playlist plays: each edit syncs its clips
    const auto started = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 200; ++i)
        song.place (lane, pattern, (Ticks)i * 3 * bar); // 200 clips of 6 s: 20 minutes
    const auto placingMs = juce::Time::getMillisecondCounterHiRes() - started;
    INFO ("placing 200 clips took " << placingMs << " ms");
    CHECK (placingMs < 20000.0);

    // One more edit in a long song is quick (each edit syncs the engine).
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    song.run ("clip.set", R"({"clip": )" + juce::String (song.session->getProject().playlist.clips[100].id.value) +
                              R"(, "muted": true})");
    const auto editMs = juce::Time::getMillisecondCounterHiRes() - t0;
    INFO ("one edit took " << editMs << " ms");
    CHECK (editMs < 250.0);

    // The Playlist draws the whole song quickly, zoomed out and in.
    ui::Playlist playlist (*song.session);
    playlist.setSize (1600, 800);
    for (auto factor : { 0.01, 50.0 })
    {
        playlist.zoom (factor);
        t0 = juce::Time::getMillisecondCounterHiRes();
        auto image = playlist.createComponentSnapshot (playlist.getLocalBounds());
        const auto paintMs = juce::Time::getMillisecondCounterHiRes() - t0;
        INFO ("drawing took " << paintMs << " ms");
        CHECK (paintMs < 200.0);
    }

    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::flac;
    t0 = juce::Time::getMillisecondCounterHiRes();
    const auto result = song.exportTo (song.root.getChildFile ("Long.flac"), settings);
    const auto exportSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    INFO (result.error << " (export took " << exportSeconds << " s)");
    REQUIRE (result.ok);
    CHECK (exportSeconds < 120.0);
    const auto rendered = Song::read (result.files.front());
    CHECK (rendered.seconds() == Catch::Approx (1200.0 + 2.0).margin (0.1));
    CHECK (rendered.rms (1190.0, 1190.5) > 0.01f); // the last clip plays

    // Playback keeps up, from the start and near the end: no block takes longer than the sound card gives it.
    for (const auto from : { 0.0, 1190.0 })
    {
        INFO ("playing from " << from << " s");
        Player player (song, 8.0);
        player.runner.start();
        player.started = true;
        player.runner.runUntil (0.2);
        song.session->getEdit().getTransport().setPosition (tracktion::TimePosition::fromSeconds (from));
        const auto start = player.play();
        player.runFor (3.0);
        song.session->stop();
        CHECK (player.level (start + 0.1, start + 0.4) > 0.01f);
        const auto& times = player.runner.getBlockTimesMs();
        const auto slowest = times.empty() ? 0.0f : *std::max_element (times.begin(), times.end());
        INFO ("slowest block " << slowest << " ms of " << player.runner.getBlockPeriodMs() << " ms");
        CHECK (player.runner.getNumOverrunBlocks() == 0);
    }
}

TEST_CASE ("A reversed, pitched and sped-up audio clip exports as it plays, also after saving and reopening",
           "[engine][edges][audioclip][export][audio]")
{
    Song song;
    ui::Playlist playlist (*song.session);
    playlist.setSize (800, 400);
    const auto file = writeSine (song.root.getChildFile ("in/Rise.wav"), 3.0, 220.0, rate, true);
    REQUIRE (playlist.addAudioFile (0, 0, file));
    const auto clip = song.session->getProject().playlist.clips.back().id;
    song.run ("clip.setAudio",
              R"({"clip": )" + juce::String (clip.value) + R"(, "reversed": true, "pitch": 7, "speed": 1.5})");
    song.session->setMode (Session::Mode::song);

    // Reversed: loud first; a fifth up (330 Hz); 1.5 times as fast: two seconds long.
    auto check = [] (const std::vector<float>& x, double sr, double offset)
    {
        CHECK (frequencyOf (x, sr, offset + 0.2, offset + 0.8) == Catch::Approx (330.0).epsilon (0.02));
        CHECK (test::checks::rms (x, (int64_t)((offset + 0.1) * sr), (int64_t)((offset + 0.4) * sr)) >
               3.0f * test::checks::rms (x, (int64_t)((offset + 1.5) * sr), (int64_t)((offset + 1.8) * sr)));
        CHECK (test::checks::rms (x, (int64_t)((offset + 2.2) * sr), (int64_t)((offset + 2.8) * sr)) < 1.0e-3f);
    };
    const auto exported = song.render();
    check (exported.left(), exported.sampleRate, 0.0);

    Player player (song, 8.0);
    player.runner.start();
    player.started = true;
    player.runner.runUntil (0.2);
    // A reversed clip plays from a reversed copy Tracktion writes in the background: wait for it.
    for (int i = 0; i < 200 && song.session->getEdit().engine.getRenderManager().getNumJobs() > 0; ++i)
        player.runFor (0.05);
    const auto start = player.play();
    player.runFor (3.0);
    song.session->stop();
    check (player.runner.getChannel (0), rate, start);
    CHECK (
        std::abs (juce::Decibels::gainToDecibels (test::checks::rms (
                      player.runner.getChannel (0), (int64_t)((start + 0.2) * rate), (int64_t)((start + 1.2) * rate))) -
                  juce::Decibels::gainToDecibels (exported.rms (0.2, 1.2))) < 1.0);

    // Saved and opened again: the same.
    song.document = std::make_unique<ProjectDocument> (*song.session, song.root.getChildFile ("settings"));
    REQUIRE (song.document->saveAs (song.root.getChildFile ("Song")).wasOk());
    song.document->newProject();
    REQUIRE (song.document->open (song.root.getChildFile ("Song")).wasOk());
    const auto& reopened = std::get<AudioClip> (song.session->getProject().playlist.clips.back().content);
    CHECK (reopened.reversed);
    CHECK (reopened.pitchSemitones == Catch::Approx (7.0));
    CHECK (reopened.speed == Catch::Approx (1.5));
    song.session->setMode (Session::Mode::song);
    const auto again = song.render ({}, "Again");
    check (again.left(), again.sampleRate, 0.0);
    CHECK (std::abs (juce::Decibels::gainToDecibels (again.rms (0.2, 1.2)) -
                     juce::Decibels::gainToDecibels (exported.rms (0.2, 1.2))) < 0.5);
}

TEST_CASE ("An audio clip at another sample rate keeps its pitch and length",
           "[engine][edges][audioclip][export][audio]")
{
    for (const auto fileRate : { 44100.0, 96000.0 })
    {
        INFO ("file at " << fileRate);
        Song song;
        ui::Playlist playlist (*song.session);
        playlist.setSize (800, 400);
        REQUIRE (
            playlist.addAudioFile (0, 0, writeSine (song.root.getChildFile ("in/Tone.wav"), 1.0, 441.0, fileRate)));
        CHECK (song.session->getProject().playlist.clips.back().length == 2 * beat); // one second at 120 BPM
        song.session->setMode (Session::Mode::song);

        for (const auto exportRate : { 44100.0, 48000.0 })
        {
            engine::ExportSettings settings;
            settings.sampleRate = exportRate;
            const auto r = song.render (settings, "At" + juce::String ((int)exportRate));
            CHECK (r.sampleRate == Catch::Approx (exportRate));
            CHECK (frequencyOf (r.left(), r.sampleRate, 0.1, 0.9) == Catch::Approx (441.0).epsilon (0.005));
            CHECK (lastSound (r.left(), r.sampleRate, 0.01f) == Catch::Approx (1.0).margin (0.01));
        }

        // Played on a 48 kHz sound card.
        Player player (song, 4.0);
        const auto start = player.play();
        player.runFor (1.5);
        song.session->stop();
        const auto& out = player.runner.getChannel (0);
        CHECK (frequencyOf (out, rate, start + 0.1, start + 0.9) == Catch::Approx (441.0).epsilon (0.005));
        const std::vector<float> played (out.begin() + (int64_t)(start * rate),
                                         out.begin() + (int64_t)((start + 1.5) * rate));
        CHECK (lastSound (played, rate, 0.01f) == Catch::Approx (1.0).margin (0.01));
    }
}

TEST_CASE ("Folded and muted track groups stay silent, also after saving and reopening", "[engine][edges][groups]")
{
    Song song;
    const auto channel = song.addChannel();
    const auto pattern = song.addPattern();
    song.setNotes (pattern, channel, { note (0, bar - beat) });
    const auto group = song.addLane(), inner = song.addLane(), deeper = song.addLane();
    song.run ("lane.set",
              R"({"lane": )" + juce::String (inner.value) + R"(, "group": )" + juce::String (group.value) + "}");
    song.run ("lane.set",
              R"({"lane": )" + juce::String (deeper.value) + R"(, "group": )" + juce::String (inner.value) + "}");
    song.place (inner, pattern, 0);
    song.place (deeper, pattern, bar);
    song.session->setMode (Session::Mode::song);
    CHECK (song.render().rms (0.2, 3.0) > 0.01f);

    song.run ("lane.set", R"({"lane": )" + juce::String (group.value) + R"(, "muted": true, "collapsed": true})");
    CHECK (song.render ({}, "Muted").rms (0.0, 4.0) < 1.0e-5f);

    song.document = std::make_unique<ProjectDocument> (*song.session, song.root.getChildFile ("settings"));
    REQUIRE (song.document->saveAs (song.root.getChildFile ("Groups")).wasOk());
    song.document->newProject();
    REQUIRE (song.document->open (song.root.getChildFile ("Groups")).wasOk());
    const auto& project = song.session->getProject();
    REQUIRE (project.findLane (group) != nullptr);
    CHECK (project.findLane (group)->muted);
    CHECK (project.findLane (group)->collapsed);
    CHECK (project.isLaneMuted (deeper));
    CHECK (project.isLaneHidden (deeper));
    song.session->setMode (Session::Mode::song);
    CHECK (song.render ({}, "Reopened").rms (0.0, 4.0) < 1.0e-5f);

    // Soloing a track inside the muted group doesn't bring it back: the group's mute wins.
    song.run ("lane.set", R"({"lane": )" + juce::String (deeper.value) + R"(, "solo": true})");
    CHECK (song.render ({}, "Soloed").rms (0.0, 4.0) < 1.0e-5f);
}

TEST_CASE ("Mute wins over solo the same way on clips, tracks, channels and mixer inserts", "[engine][edges][solo]")
{
    Song song;
    const auto a = song.addChannel();
    const auto insertA = song.lastInsert;
    const auto b = song.addChannel();
    const auto insertB = song.lastInsert;
    const auto pa = song.addPattern(), pb = song.addPattern();
    song.setNotes (pa, a, { note (0, 2 * beat, 60) });        // A in the first second
    song.setNotes (pb, b, { note (2 * beat, 2 * beat, 72) }); // B in the second
    const auto laneA = song.addLane(), laneB = song.addLane();
    const auto clipA = song.place (laneA, pa, 0);
    song.place (laneB, pb, 0);
    song.session->setMode (Session::Mode::song);

    struct Heard
    {
        bool a, b;
    };
    auto heard = [&] (const char* name)
    {
        const auto r = song.render ({}, name);
        return Heard { r.rms (0.1, 0.9) > 0.005f, r.rms (1.35, 1.9) > 0.005f }; // after A's release
    };
    auto set = [&] (const char* command, const char* key, uint32_t id, const juce::String& json)
    { song.run (command, "{\"" + juce::String (key) + "\": " + juce::String (id) + ", " + json + "}"); };

    auto both = heard ("Both");
    CHECK ((both.a && both.b));

    // Solo alone: only the soloed one, wherever it is soloed.
    for (auto [command, key, id] :
         { std::tuple { "lane.set", "lane", laneA.value }, std::tuple { "channel.set", "channel", a.value },
           std::tuple { "insert.set", "insert", insertA.value } })
    {
        INFO (command);
        set (command, key, id, R"("solo": true)");
        auto h = heard ("Solo");
        CHECK (h.a);
        CHECK (! h.b);
        // Muted as well: muted (and the other stays silent, as the solo is still on).
        set (command, key, id, R"("muted": true)");
        h = heard ("SoloMute");
        CHECK (! h.a);
        CHECK (! h.b);
        set (command, key, id, R"("muted": false, "solo": false)");
    }

    // A muted clip on a soloed track is silent.
    set ("lane.set", "lane", laneA.value, R"("solo": true)");
    set ("clip.set", "clip", clipA.value, R"("muted": true)");
    auto h = heard ("MutedClip");
    CHECK (! h.a);
    CHECK (! h.b);
    set ("clip.set", "clip", clipA.value, R"("muted": false)");
    set ("lane.set", "lane", laneA.value, R"("solo": false)");

    // A soloed channel playing into a muted insert: silent (the insert's mute wins).
    set ("channel.set", "channel", a.value, R"("solo": true)");
    set ("insert.set", "insert", insertA.value, R"("muted": true)");
    h = heard ("SoloIntoMuted");
    CHECK (! h.a);
    CHECK (! h.b);
    juce::ignoreUnused (b, insertB);
}

//==============================================================================
// 7. Export

TEST_CASE ("Exports at 44.1, 48 and 96 kHz, 16 and 24 bit have the right pitch and length", "[engine][edges][export]")
{
    Song song;
    ui::Playlist playlist (*song.session);
    playlist.setSize (800, 400);
    REQUIRE (playlist.addAudioFile (0, 0, writeSine (song.root.getChildFile ("in/A.wav"), 2.0, 440.0, rate)));
    song.session->setMode (Session::Mode::song);

    using Format = engine::ExportSettings::Format;
    for (const auto exportRate : { 44100.0, 48000.0, 96000.0 })
        for (auto [format, bits] :
             { std::pair { Format::wav16, 16u }, std::pair { Format::wav24, 24u }, std::pair { Format::flac, 24u } })
        {
            INFO (exportRate << " Hz, " << bits << " bit, " << engine::ExportSettings::extensionFor (format));
            engine::ExportSettings settings;
            settings.sampleRate = exportRate;
            settings.format = format;
            const auto result = song.exportTo (song.root.getChildFile ("out/" + juce::String ((int)exportRate) + "-" +
                                                                       juce::String ((int)bits) +
                                                                       engine::ExportSettings::extensionFor (format)),
                                               settings);
            INFO (result.error);
            REQUIRE (result.ok);
            const auto r = Song::read (result.files.front());
            CHECK (r.sampleRate == Catch::Approx (exportRate));
            CHECK (r.bits == bits);
            CHECK (r.seconds() == Catch::Approx (2.0 + 2.0).margin (0.01));
            CHECK (frequencyOf (r.left(), r.sampleRate, 0.1, 1.9) == Catch::Approx (440.0).epsilon (0.002));
            CHECK (lastSound (r.left(), r.sampleRate, 0.01f) == Catch::Approx (2.0).margin (0.01));
        }
}

TEST_CASE ("WAV, FLAC and MP3 mixes and their stems are as long as each other", "[engine][edges][export]")
{
    Song song;
    const auto one = song.addChannel();
    const auto two = song.addChannel();
    const auto pattern = song.addPattern();
    song.setNotes (pattern, one, { note (0, bar) });
    song.setNotes (pattern, two, { note (beat, beat, 67) });
    song.place (song.addLane(), pattern, 0);
    song.session->setMode (Session::Mode::song);

    using Format = engine::ExportSettings::Format;
    std::vector<Format> formats { Format::wav24, Format::flac };
    spinus::core::ChildProcess probe;
    const bool canDecodeMp3 =
        probe.start (juce::StringArray { "ffmpeg", "-version" }) && probe.waitForProcessToFinish (5000);
    if (engine::ExportJob::isMp3Available() && canDecodeMp3)
        formats.push_back (Format::mp3);
    for (auto format : formats)
    {
        INFO (engine::ExportSettings::extensionFor (format));
        engine::ExportSettings settings;
        settings.format = format;
        settings.stems = true;
        const auto folder = song.root.getChildFile ("formats" + engine::ExportSettings::extensionFor (format));
        const auto result = song.exportTo (folder.getChildFile ("My Song"), settings);
        INFO (result.error);
        REQUIRE (result.ok);
        REQUIRE (result.files.size() == 3);
        CHECK (result.files[0].getFileName() == "My Song" + engine::ExportSettings::extensionFor (format));
        for (auto& f : result.files)
        {
            INFO (f.getFullPathName());
            auto readable = f;
            if (format == Format::mp3)
            {
                readable = f.withFileExtension (".decoded.wav");
                spinus::core::ChildProcess ffmpeg;
                REQUIRE (ffmpeg.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i",
                                                           f.getFullPathName(), readable.getFullPathName() }));
                ffmpeg.waitForProcessToFinish (30000);
            }
            // MP3 adds a little silence of its own at the start (the encoder's delay).
            CHECK (Song::read (readable).seconds() ==
                   Catch::Approx (4.0).margin (format == Format::mp3 ? 0.06 : 0.001));
        }
    }
}

TEST_CASE ("Cancelling an export halfway leaves no file that looks finished", "[engine][edges][export]")
{
    Song song;
    const auto channel = song.addChannel();
    const auto pattern = song.addPattern();
    song.setNotes (pattern, channel, { note (0, bar) });
    const auto lane = song.addLane();
    for (int i = 0; i < 60; ++i)
        song.place (lane, pattern, i * bar); // two minutes
    song.session->setMode (Session::Mode::song);

    using Format = engine::ExportSettings::Format;
    for (auto format : { Format::wav24, Format::flac, Format::opus })
    {
        INFO (engine::ExportSettings::extensionFor (format));
        const auto folder = song.root.getChildFile ("cancel" + engine::ExportSettings::extensionFor (format));
        REQUIRE (folder.createDirectory());
        engine::ExportSettings settings;
        settings.format = format;
        settings.stems = true;
        const auto result = song.exportTo (folder.getChildFile ("Song"), settings, true);
        CHECK (result.cancelled);
        CHECK (! result.ok);
        CHECK (folder.findChildFiles (juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no).isEmpty());
    }
}

//==============================================================================
// 5. Mixer and routing

namespace
{
juce::String json (std::initializer_list<std::pair<const char*, juce::var>> values)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : values)
        o->setProperty (key, value);
    return juce::JSON::toString (juce::var (o), true);
}

int idOf (InsertId id) { return (int)id.value; }

/** RMS of one channel of a render between two times. */
float channelRms (const Song::Rendered& r, int channel, double from, double to)
{
    const auto a = juce::jlimit (0, r.audio.getNumSamples(), (int)(from * r.sampleRate));
    const auto b = juce::jlimit (0, r.audio.getNumSamples(), (int)(to * r.sampleRate));
    return b > a ? r.audio.getRMSLevel (channel, a, b - a) : 0.0f;
}

/** The first sample louder than `threshold`, or -1. */
int64_t firstSound (const float* x, int64_t from, int64_t to, float threshold = 1.0e-3f)
{
    for (auto i = from; i < to; ++i)
        if (std::abs (x[i]) > threshold)
            return i;
    return -1;
}

/** The stem file of an insert in an export's files (named "NN Name"). */
juce::File stemOf (const engine::ExportJob::Result& result, const juce::String& insertName)
{
    for (auto& f : result.files)
        if (f.getParentDirectory().getFileName().endsWith (" stems") &&
            f.getFileNameWithoutExtension().fromFirstOccurrenceOf (" ", false, false) == insertName)
            return f;
    return {};
}
} // namespace

TEST_CASE ("A routing loop is refused with a message, by output, send or sidechain, also in a project file",
           "[engine][edges][routing]")
{
    Song song;
    song.addChannel();
    const auto a = song.lastInsert;
    song.addChannel();
    const auto b = song.lastInsert;
    song.run ("insert.setSend", json ({ { "insert", idOf (a) }, { "target", idOf (b) }, { "levelDb", -6.0 } }));

    auto refused = [&] (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        REQUIRE (command != nullptr);
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto result = song.session->getHistory().perform (std::move (command));
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < 1000.0);
        INFO (name << ": " << result.getErrorMessage());
        CHECK (result.failed());
        CHECK (result.getErrorMessage().contains ("feedback loop"));
    };
    const auto before = song.session->getProject();
    refused ("insert.setSend", json ({ { "insert", idOf (b) }, { "target", idOf (a) }, { "levelDb", -6.0 } }));
    refused ("insert.setOutput", json ({ { "insert", idOf (b) }, { "target", idOf (a) } }));
    refused ("insert.setSidechain", json ({ { "insert", idOf (a) }, { "source", idOf (b) } }));
    CHECK (song.session->getProject() == before);

    // The Mixer says why, and the strip's + Send menu greys the loop out.
    ui::Mixer mixer (*song.session);
    CHECK (! mixer.setSend (b, a, -6.0));
    CHECK (mixer.getLastError().contains ("feedback loop"));
    for (const auto& target : mixer.getSendTargets (b))
        if (target.insert == a)
        {
            CHECK (! target.allowed);
            CHECK (target.text.contains ("loop"));
        }

    // A project file edited by hand (or by another program) with sends both ways: an output loop is refused when
    // the file is read; a loop of sends is not (an owner decision), but it plays and exports rather than hangs.
    auto outputLoop = song.session->getProject();
    outputLoop.findInsert (a)->output = b;
    outputLoop.findInsert (b)->output = a;
    Project read;
    const auto parsed = fromJsonText (toJsonText (outputLoop), read);
    CHECK (parsed.failed());
    CHECK (parsed.getErrorMessage().contains ("routing cycle"));

    const auto pattern = song.addPattern();
    song.setNotes (pattern, song.session->getProject().channels[0].id, { note (0, beat) });
    auto data = song.session->captureProjectData();
    data.project.findInsert (b)->sends.push_back ({ a, -6.0f });
    REQUIRE (fromJsonText (toJsonText (data.project), read).wasOk());
    song.session->loadProject (data);
    song.session->setMode (Session::Mode::pattern);
    song.session->setCurrentPattern (pattern);
    engine::ExportSettings settings;
    settings.pattern = pattern;
    CHECK (song.render (settings, "Loop").rms (0.05, 0.4) > 0.01f);
    Player player (song, 4.0);
    const auto start = player.play();
    player.runFor (1.0);
    song.session->stop();
    CHECK (player.level (start + 0.05, start + 0.4) > 0.01f);
}

TEST_CASE ("A fader all the way down is silent: insert, channel and Master, with a send after it",
           "[engine][edges][fader][audio]")
{
    Song song;
    const auto channel = song.addChannel();
    const auto insert = song.lastInsert;
    const auto pattern = song.addPattern();
    song.setNotes (pattern, channel, { note (0, bar - beat, 60, 1.0f) });
    song.place (song.addLane(), pattern, 0);
    // A reverb bus the insert sends to: the send comes after the fader, so it goes quiet too.
    const auto bus = InsertId { (uint32_t)(int)song.run ("insert.add", json ({ { "name", "Bus" } }))["insert"] };
    song.run ("insert.setSend", json ({ { "insert", idOf (insert) }, { "target", idOf (bus) }, { "levelDb", 0.0 } }));
    song.session->setMode (Session::Mode::song);
    REQUIRE (song.render().rms (0.1, 1.4) > 0.01f);

    auto peak = [] (const Song::Rendered& r)
    {
        return juce::jmax (r.audio.getMagnitude (0, 0, r.audio.getNumSamples()),
                           r.audio.getMagnitude (1, 0, r.audio.getNumSamples()));
    };

    for (auto [command, key, id] : { std::tuple { "insert.set", "insert", idOf (insert) },
                                     std::tuple { "channel.set", "channel", (int)channel.value },
                                     std::tuple { "insert.set", "insert", idOf (masterInsertId) } })
    {
        INFO (command << " " << id);
        song.run (command, json ({ { key, id }, { "volumeDb", -100.0 } }));
        const auto r = song.render ({}, "Down");
        CHECK (peak (r) <= 0.0f);

        // In playback too.
        Player player (song, 4.0);
        const auto start = player.play();
        player.runFor (1.5);
        song.session->stop();
        float loudest = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (auto i = (size_t)(start * rate); i < (size_t)((start + 1.5) * rate); ++i)
                loudest = juce::jmax (loudest, std::abs (player.runner.getChannel (c)[i]));
        CHECK (loudest <= 0.0f);
        song.run (command, json ({ { key, id }, { "volumeDb", 0.0 } }));
    }
}

TEST_CASE ("A sidechain Ducker and Compressor still duck after saving and reopening, and after the source moves",
           "[engine][edges][sidechain][audio]")
{
    Song song;
    const auto kick = song.addChannel();
    const auto kickInsert = song.lastInsert;
    const auto bass = song.addChannel();
    const auto bassInsert = song.lastInsert;
    song.run ("insert.set", json ({ { "insert", idOf (kickInsert) }, { "name", "Kick" } }));
    song.run ("insert.set", json ({ { "insert", idOf (bassInsert) }, { "name", "Bass" } }));
    const auto pattern = song.addPattern (2 * bar);
    std::vector<Note> hits;
    hits.reserve (8);
    for (int i = 0; i < 8; ++i)
        hits.push_back (note (i * beat, beat / 8, 84, 1.0f));
    song.setNotes (pattern, kick, hits);
    song.setNotes (pattern, bass, { note (0, 2 * bar, 36, 0.8f) });
    song.place (song.addLane(), pattern, 0);
    song.session->setMode (Session::Mode::song);

    // How far the bass dips just after each kick, against just before the next one (dB): from its stem.
    auto dip = [&] (const juce::String& name)
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav32Float;
        settings.stems = true;
        const auto result = song.exportTo (song.root.getChildFile (name + "/Mix"), settings);
        INFO (result.error);
        REQUIRE (result.ok);
        const auto stem = stemOf (result, "Bass");
        REQUIRE (stem.existsAsFile());
        const auto r = Song::read (stem);
        float after = 0.0f, before = 0.0f;
        for (int i = 1; i < 7; ++i)
        {
            const auto t = i * 0.5;
            after += r.rms (t + 0.03, t + 0.08);
            before += r.rms (t + 0.40, t + 0.48);
        }
        return juce::Decibels::gainToDecibels (after / juce::jmax (before, 1.0e-9f));
    };
    const auto none = dip ("None");
    INFO ("without a sidechain: " << none << " dB");
    CHECK (std::abs (none) < 1.5f);

    for (auto type : { "spinus.ducker", "spinus.compressor" })
    {
        INFO (type);
        const auto device = (uint32_t)(int)song.run (
            "insert.addEffect", json ({ { "insert", idOf (bassInsert) },
                                        { "device", juce::JSON::parse (R"({"kind": "stock", "stockType": ")" +
                                                                       juce::String (type) + "\"}") } }))["device"];
        if (juce::String (type) == "spinus.compressor") // follows the sidechain, hard and fast
            song.run (
                "device.setParameters",
                R"({"device": )" + juce::String (device) +
                    R"(, "text": {"sidechain": "Sidechain", "threshold": "-40", "ratio": "20", "attack": "0.1", "release": "100"}})");
        song.run ("insert.setSidechain", json ({ { "insert", idOf (bassInsert) }, { "source", idOf (kickInsert) } }));
        const auto ducked = dip ("Ducked");
        INFO ("ducked: " << ducked << " dB");
        CHECK (ducked < -4.0f);

        // Saved and opened again.
        song.document = std::make_unique<ProjectDocument> (*song.session, song.root.getChildFile ("settings"));
        const auto folder = song.root.getChildFile (juce::String ("Sidechain ") + type);
        REQUIRE (song.document->saveAs (folder).wasOk());
        song.document->newProject();
        REQUIRE (song.document->open (folder).wasOk());
        song.session->setMode (Session::Mode::song);
        CHECK (song.session->getProject().findInsert (bassInsert)->sidechainSources == std::vector { kickInsert });
        const auto reopened = dip ("Reopened");
        INFO ("reopened: " << reopened << " dB");
        CHECK (reopened == Catch::Approx (ducked).margin (1.0));

        // The kick's insert moved to the end of the mixer, and back.
        song.run ("insert.move", json ({ { "insert", idOf (kickInsert) }, { "index", 1 } }));
        CHECK (song.session->getProject().mixer.inserts.back().id == kickInsert);
        const auto moved = dip ("Moved");
        INFO ("moved: " << moved << " dB");
        CHECK (moved == Catch::Approx (ducked).margin (1.0));
        song.run ("insert.move", json ({ { "insert", idOf (kickInsert) }, { "index", 0 } }));

        // The Ducker moved to after another effect on the bass's insert: still follows the kick.
        song.run ("insert.addEffect",
                  json ({ { "insert", idOf (bassInsert) },
                          { "device", juce::JSON::parse (R"({"kind": "stock", "stockType": "spinus.filter"})") },
                          { "index", 0 } }));
        const auto behind = dip ("Behind");
        INFO ("behind another effect: " << behind << " dB");
        CHECK (behind < -4.0f);

        // Off again for the next device.
        while (! song.session->getProject().findInsert (bassInsert)->effects.empty())
            song.run (
                "insert.removeEffect",
                json ({ { "device",
                          (int)song.session->getProject().findInsert (bassInsert)->effects[0].device.id.value } }));
        song.run ("insert.setSidechain", json ({ { "insert", idOf (bassInsert) } }));
    }
}

TEST_CASE ("Removing an insert that others send to removes the sends; Undo brings them back, heard",
           "[engine][edges][routing][audio]")
{
    Song song;
    const auto one = song.addChannel();
    const auto a = song.lastInsert;
    const auto two = song.addChannel();
    const auto b = song.lastInsert;
    const auto pattern = song.addPattern();
    song.setNotes (pattern, one, { note (0, beat, 60) });
    song.setNotes (pattern, two, { note (2 * beat, beat, 67) });
    song.place (song.addLane(), pattern, 0);
    const auto reverb = InsertId { (uint32_t)(int)song.run ("insert.add", json ({ { "name", "Reverb" } }))["insert"] };
    song.run ("insert.set", json ({ { "insert", idOf (a) }, { "name", "A" } }));
    song.run ("insert.setSend", json ({ { "insert", idOf (a) }, { "target", idOf (reverb) }, { "levelDb", -3.0 } }));
    song.run ("insert.setSend", json ({ { "insert", idOf (b) }, { "target", idOf (reverb) }, { "levelDb", -9.0 } }));
    song.session->setMode (Session::Mode::song);

    auto sendsIn = [] (const juce::String& description)
    {
        int n = 0;
        for (auto& line : juce::StringArray::fromLines (description))
            if (line.contains ("sends=[") && ! line.contains ("sends=[]"))
                ++n;
        return n;
    };
    auto returnStem = [&] (const juce::String& name)
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav32Float;
        settings.stems = true;
        const auto result = song.exportTo (song.root.getChildFile (name + "/Mix"), settings);
        INFO (result.error);
        REQUIRE (result.ok);
        return std::pair { Song::read (result.files.front()), stemOf (result, "Reverb") };
    };

    const auto [withSends, reverbStem] = returnStem ("Sends");
    REQUIRE (reverbStem.existsAsFile());
    const auto wet = Song::read (reverbStem);
    CHECK (wet.rms (0.05, 0.45) > 0.005f); // A's send
    CHECK (wet.rms (1.05, 1.45) > 0.002f); // B's
    CHECK (sendsIn (song.session->getAdapter().describe()) == 2);

    song.run ("insert.remove", json ({ { "insert", idOf (reverb) } }));
    const auto& project = song.session->getProject();
    CHECK (project.findInsert (reverb) == nullptr);
    CHECK (project.findInsert (a)->sends.empty());
    CHECK (project.findInsert (b)->sends.empty());
    CHECK (sendsIn (song.session->getAdapter().describe()) == 0);
    const auto [without, gone] = returnStem ("Removed");
    CHECK (gone == juce::File());
    CHECK (without.rms (0.05, 0.45) > 0.005f); // the dry sound still plays
    CHECK (without.rms (0.05, 0.45) < withSends.rms (0.05, 0.45));

    song.session->getHistory().undo();
    {
        const auto& back = song.session->getProject();
        REQUIRE (back.findInsert (reverb) != nullptr);
        REQUIRE (back.findInsert (a)->sends.size() == 1);
        REQUIRE (back.findInsert (b)->sends.size() == 1);
        CHECK (back.findInsert (a)->sends[0].target == reverb);
        CHECK (back.findInsert (a)->sends[0].levelDb == Catch::Approx (-3.0f));
        CHECK (back.findInsert (b)->sends[0].levelDb == Catch::Approx (-9.0f));
    }
    CHECK (sendsIn (song.session->getAdapter().describe()) == 2);
    const auto [again, againStem] = returnStem ("Undone");
    REQUIRE (againStem.existsAsFile());
    const auto wetAgain = Song::read (againStem);
    CHECK (wetAgain.rms (0.05, 0.45) == Catch::Approx (wet.rms (0.05, 0.45)).epsilon (0.01));
    CHECK (wetAgain.rms (1.05, 1.45) == Catch::Approx (wet.rms (1.05, 1.45)).epsilon (0.01));
    CHECK (again.rms (0.05, 1.5) == Catch::Approx (withSends.rms (0.05, 1.5)).epsilon (0.01));

    song.session->getHistory().redo();
    CHECK (song.session->getProject().findInsert (a)->sends.empty());
    CHECK (sendsIn (song.session->getAdapter().describe()) == 0);
}

TEST_CASE ("Delay compensation: a drum behind a look-ahead Limiter stays in time, in playback and export",
           "[engine][edges][latency][audio]")
{
    Song song;
    const auto late = song.addChannel();
    const auto lateInsert = song.lastInsert;
    const auto other = song.addChannel();
    const auto otherInsert = song.lastInsert;
    const auto pattern = song.addPattern();
    // The same hit on both, on every beat: one on the left, the other on the right.
    std::vector<Note> hits;
    hits.reserve (4);
    for (int i = 0; i < 4; ++i)
        hits.push_back (note (i * beat, beat / 4, 60, 0.5f));
    song.setNotes (pattern, late, hits);
    song.setNotes (pattern, other, hits);
    song.place (song.addLane(), pattern, 0);
    song.run ("insert.set", json ({ { "insert", idOf (lateInsert) }, { "pan", -1.0 } }));
    song.run ("insert.set", json ({ { "insert", idOf (otherInsert) }, { "pan", 1.0 } }));
    const auto limiter = (int)song.run (
        "insert.addEffect",
        json ({ { "insert", idOf (lateInsert) },
                { "device", juce::JSON::parse (R"({"kind": "stock", "stockType": "spinus.limiter"})") } }))["device"];
    song.run ("device.setParameters", R"({"device": )" + juce::String (limiter) + R"(, "values": {"lookahead": 1.0}})");
    song.session->setMode (Session::Mode::song);

    // Each hit's start on the left against the right, in samples.
    auto offsets = [] (const float* left, const float* right, double sampleRate, double start)
    {
        std::vector<int64_t> result;
        for (int i = 0; i < 4; ++i)
        {
            const auto from = std::max<int64_t> (0, (int64_t)((start + i * 0.5 - 0.05) * sampleRate));
            const auto to = (int64_t)((start + i * 0.5 + 0.1) * sampleRate);
            const auto l = firstSound (left, from, to), r = firstSound (right, from, to);
            result.push_back (l < 0 || r < 0 ? 100000 : l - r);
        }
        return result;
    };

    const auto exported = song.render();
    for (auto offset : offsets (exported.audio.getReadPointer (0), exported.audio.getReadPointer (1), rate, 0.0))
    {
        INFO ("exported " << offset << " samples apart");
        CHECK (std::abs (offset) <= 2);
    }

    Player player (song, 6.0);
    const auto start = player.play();
    player.runFor (2.2);
    song.session->stop();
    // Playback starts a little late by the compensation: the hits are still together.
    for (auto offset : offsets (player.runner.getChannel (0).data(), player.runner.getChannel (1).data(), rate, start))
    {
        INFO ("played " << offset << " samples apart");
        CHECK (std::abs (offset) <= 2);
    }

    // The test means something: the Limiter (prepared by playing) holds the sound back by more than 4 ms.
    auto* plugin = song.session->getAdapter().getDevicePlugin (DeviceId { (uint32_t)limiter });
    REQUIRE (plugin != nullptr);
    INFO ("the Limiter's latency: " << plugin->getLatencySeconds() * 1000.0 << " ms");
    CHECK (plugin->getLatencySeconds() > 0.004);
}

TEST_CASE ("Mono, stereo and many-output sources reach the mixer at the right level and pan",
           "[engine][edges][levels][audio]")
{
    // Audio clips: a mono file and a stereo one with the same sine on both sides.
    {
        Song song;
        ui::Playlist playlist (*song.session);
        playlist.setSize (800, 400);
        const auto mono = writeSine (song.root.getChildFile ("in/Mono.wav"), 1.0, 440.0, rate);
        auto stereo = song.root.getChildFile ("in/Stereo.wav");
        {
            juce::AudioBuffer<float> audio (2, (int)rate);
            for (int i = 0; i < audio.getNumSamples(); ++i)
                for (int c = 0; c < 2; ++c)
                    audio.setSample (c, i, 0.5f * (float)std::sin (2.0 * M_PI * 440.0 * i / rate));
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (stereo);
            auto writer = juce::WavAudioFormat().createWriterFor (
                stream,
                juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (16));
            REQUIRE (writer != nullptr);
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        }
        REQUIRE (playlist.addAudioFile (0, 0, mono));
        const auto monoChannel = std::get<AudioClip> (song.session->getProject().playlist.clips.back().content).channel;
        REQUIRE (playlist.addAudioFile (0, 4 * beat, stereo)); // two seconds later
        song.session->setMode (Session::Mode::song);

        const auto r = song.render();
        const auto expected = 0.5f / std::sqrt (2.0f); // the files' own level: a sine at 0.5
        INFO ("mono L " << channelRms (r, 0, 0.1, 0.9) << " R " << channelRms (r, 1, 0.1, 0.9) << ", stereo L "
                        << channelRms (r, 0, 2.1, 2.9) << " R " << channelRms (r, 1, 2.1, 2.9));
        for (int c = 0; c < 2; ++c)
        {
            CHECK (channelRms (r, c, 0.1, 0.9) == Catch::Approx (expected).epsilon (0.05));
            CHECK (channelRms (r, c, 2.1, 2.9) == Catch::Approx (expected).epsilon (0.05));
        }

        // Panned hard left on its insert: all on the left, at the same level as both sides had.
        const auto insert = song.session->getProject().findChannel (monoChannel)->targetInsert;
        song.run ("insert.set", json ({ { "insert", idOf (insert) }, { "pan", -1.0 } }));
        const auto left = song.render ({}, "Left");
        CHECK (channelRms (left, 1, 0.1, 0.9) < 1.0e-4f);
        CHECK (channelRms (left, 0, 0.1, 0.9) > 0.5f * expected);
    }

    // Instruments: the stereo test synth, its mono twin and its twin with three stereo outputs play the same note
    // at the same level on both sides (a plugin's extra outputs are not mixed in).
    {
        Song song;
        const auto pattern = song.addPattern();
        std::vector<std::pair<juce::String, ChannelId>> synths;
        for (auto [file, name] :
             { std::pair { "test-synth", "Spinus Test Synth" }, std::pair { "mono-synth", "Spinus Mono Synth" },
               std::pair { "multi-out-synth", "Spinus Multi-Out Synth" } })
            synths.emplace_back (name, song.addChannel ({}, {}, file, name));
        const auto lane = song.addLane();
        song.place (lane, pattern, 0);
        song.session->setMode (Session::Mode::song);
        float reference = 0.0f;
        for (auto& [name, channel] : synths)
        {
            INFO (name);
            for (auto& [other, otherChannel] : synths)
                song.setNotes (pattern, otherChannel,
                               otherChannel == channel ? std::vector { note (0, bar - beat, 57) }
                                                       : std::vector<Note> {});
            const auto r = song.render ({}, name.removeCharacters (" "));
            const auto l = channelRms (r, 0, 0.2, 1.2), rr = channelRms (r, 1, 0.2, 1.2);
            INFO ("L " << l << " R " << rr);
            CHECK (l > 0.01f);
            CHECK (l == Catch::Approx (rr).epsilon (0.02));
            if (reference <= 0.0f)
                reference = l;
            CHECK (l == Catch::Approx (reference).epsilon (0.05));
        }
    }
}
