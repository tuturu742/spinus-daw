/*  Edge cases from the 0.1.0-rc.1 checklist (Piano Roll and Channel Rack, Playlist, Export): notes at the edges of
    a pattern and of the keyboard, stuck notes, swing, export formats, long songs, audio clips at another
    sample rate, track groups, and which of mute and solo wins. */

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
        juce::OwnedArray<juce::PluginDescription> found;
        clap.findAllTypesForFile (found, synthFile().getFullPathName());
        for (auto* d : found)
            if (d->name == "Spinus Test Synth")
                host.getEngine().getPluginManager().knownPluginList.addType (*d);

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

    static juce::File synthFile() { return juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap"); }

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

    /** A channel with a stock device ("spinus.3x-osc") or, with an empty type, the test synth. */
    ChannelId addChannel (const juce::String& stockType = "spinus.3x-osc", InsertId insert = {})
    {
        juce::String instrument =
            stockType.isNotEmpty()
                ? R"({"kind": "stock", "stockType": ")" + stockType + "\"}"
                : R"({"kind": "plugin", "plugin": {"format": "CLAP", "name": "Spinus Test Synth", "manufacturer": "Spinus tests", "fileOrIdentifier": )" +
                      juce::JSON::toString (synthFile().getFullPathName()) + "}}";
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

    song.session->setMode (Session::Mode::song);
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
