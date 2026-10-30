#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "core/ChildProcess.h"
#include "RealtimeRunner.h"
#include "TestDevices.h"
#include "engine/EngineHost.h"
#include "engine/DeviceFactory.h"
#include "engine/Exporter.h"
#include "engine/OutputCapturePlugin.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/LiveExport.h"
#include "session/Session.h"
#include "ui/ExportDialog.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::engine;
using namespace spinus::model;

namespace
{
/** Two tone channels on inserts "Lead" and "Pad" (Pad 12 dB quieter), one bar in the playlist. */
Project makeSong()
{
    Project p;
    p.settings.tempoBpm = 120.0; // one 4/4 bar = 2 seconds

    Insert lead, pad, unused;
    lead.id = p.allocateId<InsertId>();
    lead.name = "Lead";
    pad.id = p.allocateId<InsertId>();
    pad.name = "Pad";
    unused.id = p.allocateId<InsertId>();
    unused.name = "Unused";
    p.mixer.inserts = { lead, pad, unused };

    auto tone = [&] (const char* name, InsertId insert, float volumeDb)
    {
        Channel c;
        c.id = p.allocateId<ChannelId>();
        c.name = name;
        c.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "test.tone", {}, false };
        c.volumeDb = volumeDb;
        c.targetInsert = insert;
        return c;
    };
    p.channels = { tone ("One", lead.id, 0.0f), tone ("Two", pad.id, -12.0f) };

    Pattern pattern;
    pattern.id = p.allocateId<PatternId>();
    pattern.length = 4 * ticksPerQuarterNote;
    p.patterns = { pattern };

    Lane lane { p.allocateId<LaneId>(), "Track 1", 0xff3d4450, false };
    p.playlist.lanes = { lane };
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), lane.id, 0, pattern.length, false, PatternClip { pattern.id, 0 } });
    return p;
}

struct Fixture
{
    Fixture() : devices (host.getEngine()) {}

    ExportJob::Result run (const Project& project, const juce::File& destination, ExportSettings settings,
                           bool cancelAtOnce = false)
    {
        std::optional<ExportJob::Result> result;
        ExportJob job (host.getEngine(), devices, project, {}, destination, std::move (settings),
                       [&] (const ExportJob::Result& r) { result = r; });
        job.start();
        if (cancelAtOnce)
            job.cancel();

        for (int i = 0; i < 600 && ! result; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        REQUIRE (result.has_value());
        return result ? *result : ExportJob::Result {};
    }

    EngineHost host { { .openAudioDevice = false } };
    test::TestDevices devices;
    juce::TemporaryFile folder;
};

struct Audio
{
    double seconds = 0, sampleRate = 0;
    unsigned int bits = 0;
    bool isFloat = false;
    float rmsDb = -100.0f;
};

Audio read (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);

    juce::AudioBuffer<float> buffer ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
    // RMS over the first second (the tone plays throughout the bar).
    const auto rms = buffer.getRMSLevel (0, 0, (int)reader->sampleRate);
    return { (double)reader->lengthInSamples / reader->sampleRate, reader->sampleRate, reader->bitsPerSample,
             reader->usesFloatingPointData, juce::Decibels::gainToDecibels (rms, -100.0f) };
}
} // namespace

TEST_CASE ("Exporting the song writes a WAV of the song plus the tail", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    for (auto [format, bits, isFloat] : { std::tuple { ExportSettings::Format::wav16, 16u, false },
                                          std::tuple { ExportSettings::Format::wav24, 24u, false },
                                          std::tuple { ExportSettings::Format::wav32Float, 32u, true } })
    {
        INFO ("bits " << bits);
        ExportSettings settings;
        settings.format = format;
        settings.sampleRate = 44100.0;
        const auto result = f.run (makeSong(), folder.getChildFile ("Song"), settings);
        INFO (result.error);
        REQUIRE (result.ok);
        REQUIRE (result.files.size() == 1);
        CHECK (result.files[0] == folder.getChildFile ("Song.wav"));

        const auto audio = read (result.files[0]);
        CHECK (std::abs (audio.seconds - 4.0) < 0.05); // 2 s bar + 2 s tail
        CHECK (juce::approximatelyEqual (audio.sampleRate, 44100.0));
        CHECK (audio.bits == bits);
        CHECK (audio.isFloat == isFloat);
        CHECK (audio.rmsDb > -30.0f);
    }
}

TEST_CASE ("Stems: one file per insert with sound, each with only its own sound", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    auto song = makeSong();
    CHECK (ExportJob::getStemInserts (song) == std::vector { song.mixer.inserts[0].id, song.mixer.inserts[1].id });

    ExportSettings settings;
    settings.stems = true;
    const auto result = f.run (song, folder.getChildFile ("Song.wav"), settings);
    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.files.size() == 3);
    CHECK (result.files[1] == folder.getChildFile ("Song stems/01 Lead.wav"));
    CHECK (result.files[2] == folder.getChildFile ("Song stems/02 Pad.wav"));

    const auto mix = read (result.files[0]), lead = read (result.files[1]), pad = read (result.files[2]);
    CHECK (std::abs ((lead.rmsDb - pad.rmsDb) - 12.0f) < 0.5f); // Pad's channel is 12 dB down
    CHECK (mix.rmsDb > lead.rmsDb);                             // both tones (same pitch, so they add)

    // A stem includes what is routed into its insert: route Pad into Lead.
    song.mixer.inserts[1].output = song.mixer.inserts[0].id;
    const auto routed = f.run (song, folder.getChildFile ("Routed.wav"), settings);
    REQUIRE (routed.ok);
    REQUIRE (routed.files.size() == 3);
    CHECK (read (routed.files[1]).rmsDb > lead.rmsDb + 0.5f);           // Lead's stem: Lead and Pad
    CHECK (std::abs (read (routed.files[2]).rmsDb - pad.rmsDb) < 0.1f); // Pad's stem: as before
}

TEST_CASE ("Which inserts get a stem, and which stems are silent", "[engine][export]")
{
    // Lead ducks to Pad (a Ducker on Lead following Pad's insert); a third insert feeds Pad.
    auto song = makeSong();
    const auto lead = song.mixer.inserts[0].id, pad = song.mixer.inserts[1].id, unused = song.mixer.inserts[2].id;
    model::Insert feeder;
    feeder.id = { 900 };
    feeder.name = "Feeder";
    feeder.output = pad;
    song.mixer.inserts.push_back (feeder);
    CHECK (ExportJob::getStemInserts (song) == std::vector { lead, pad });

    // An insert with no channel but a sidechain from one gets a stem (as its effects may make sound).
    song.mixer.inserts[2].sidechainSources = { pad };
    CHECK (ExportJob::getStemInserts (song) == std::vector { lead, pad, unused });

    // A muted insert's stem is silent, and so is one that plays into it: neither is heard.
    CHECK (ExportJob::getSilentStems (song).empty());
    song.findInsert (pad)->muted = true;
    CHECK (ExportJob::getSilentStems (song) == std::set { pad, feeder.id });
}

TEST_CASE ("Exporting a pattern, and an empty playlist", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    auto song = makeSong();
    song.playlist.clips.clear();
    const auto empty = f.run (song, folder.getChildFile ("Empty.wav"), {});
    CHECK (! empty.ok);
    CHECK (empty.error.contains ("playlist is empty"));
    CHECK (! folder.getChildFile ("Empty.wav").exists());

    ExportSettings settings;
    settings.pattern = song.patterns[0].id;
    settings.tailSeconds = 0.0;
    const auto pattern = f.run (song, folder.getChildFile ("Pattern.wav"), settings);
    INFO (pattern.error);
    REQUIRE (pattern.ok);
    CHECK (std::abs (read (pattern.files[0]).seconds - 2.0) < 0.05);
}

TEST_CASE ("A cancelled export leaves no files", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    ExportSettings settings;
    settings.stems = true;
    const auto result = f.run (makeSong(), folder.getChildFile ("Song.wav"), settings, true);
    CHECK (! result.ok);
    CHECK (result.cancelled);
    CHECK (folder.findChildFiles (juce::File::findFiles, true, "*.wav").isEmpty());
}

TEST_CASE ("MP3 export uses lame", "[engine][export]")
{
    if (! ExportJob::isMp3Available())
        SKIP ("lame is not installed");

    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    auto song = makeSong();
    song.settings.title = "Test song";
    ExportSettings settings;
    settings.format = ExportSettings::Format::mp3;
    settings.mp3Kbps = 192;
    const auto result = f.run (song, folder.getChildFile ("Song.wav"), settings);
    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.files[0] == folder.getChildFile ("Song.mp3"));
    // 4 s at 192 kbit/s is about 96 kB.
    CHECK (result.files[0].getSize() > 60000);
    CHECK (result.files[0].getSize() < 140000);
}

TEST_CASE ("FLAC, Opus and AAC exports sound like the WAV", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    // The WAV export, to compare with.
    const auto wav = f.run (makeSong(), folder.getChildFile ("Reference.wav"), {});
    REQUIRE (wav.ok);
    std::unique_ptr<juce::AudioFormatReader> wavReader (
        juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (wav.files[0]), true));
    REQUIRE (wavReader != nullptr);
    const auto wavSeconds = (double)wavReader->lengthInSamples / wavReader->sampleRate;

    auto formats = std::vector<ExportSettings::Format> { ExportSettings::Format::flac, ExportSettings::Format::opus };
    if (ExportJob::isAacAvailable())
        formats.push_back (ExportSettings::Format::aac);
    for (auto format : formats)
    {
        ExportSettings settings;
        settings.format = format;
        const auto result = f.run (makeSong(), folder.getChildFile ("Song.wav"), settings);
        INFO (ExportSettings::extensionFor (format) << ": " << result.error);
        REQUIRE (result.ok);
        const auto file = result.files[0];
        CHECK (file.getFileExtension() == ExportSettings::extensionFor (format));

        // Read back (AAC through ffmpeg): as long as the WAV, and not silent.
        auto readable = file;
        if (format == ExportSettings::Format::aac)
        {
            readable = folder.getChildFile ("aac.wav");
            spinus::core::ChildProcess ffmpeg;
            REQUIRE (ffmpeg.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i",
                                                       file.getFullPathName(), readable.getFullPathName() }));
            ffmpeg.waitForProcessToFinish (30000);
        }
        juce::AudioFormatManager formatsToRead;
        formatsToRead.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formatsToRead.createReaderFor (readable));
        REQUIRE (reader != nullptr);
        const auto seconds = (double)reader->lengthInSamples / reader->sampleRate;
        CHECK (std::abs (seconds - wavSeconds) < 0.1);
        juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->sampleRate);
        reader->read (&audio, 0, audio.getNumSamples(), (juce::int64)reader->sampleRate, true, true);
        CHECK (audio.getRMSLevel (0, 0, audio.getNumSamples()) > 0.01f);
    }
}

TEST_CASE ("The Export dialog exports and says where the files went", "[ui][export]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    session.getHistory().reset (makeSong());

    juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory());

    ui::ExportDialog dialog (session);
    dialog.setFormat (ExportSettings::Format::wav16);
    dialog.setStems (true);
    CHECK (dialog.getSettings().format == ExportSettings::Format::wav16);
    CHECK (! dialog.getSettings().pattern.has_value());

    dialog.startExport (folder.getFile().getChildFile ("My song.wav"));
    CHECK (dialog.isExporting());
    for (int i = 0; i < 600 && dialog.isExporting(); ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    CHECK (! dialog.isExporting());
    CHECK (dialog.getStatusText() == "Saved My song.wav and 2 stems in \"My song stems\".");
    CHECK (folder.getFile().getChildFile ("My song stems/02 Pad.wav").existsAsFile());

    dialog.setExportPattern (true);
    CHECK (dialog.getSettings().pattern == session.getCurrentPattern());

    // Playing while exporting: the song, as one file.
    dialog.setPlayWhileExporting (true);
    CHECK (dialog.isPlayingWhileExporting());
    CHECK (! dialog.getSettings().pattern.has_value());
    CHECK (! dialog.getSettings().stems);
}

TEST_CASE ("An export plays Spinus Tune's rendering of a tuned clip", "[engine][export]")
{
    Fixture f;
    // An audio clip of silence; its "rendering" is a loud tone.
    auto writeFile = [&] (const juce::String& name, float level)
    {
        const auto file = f.folder.getFile().getChildFile (name);
        file.getParentDirectory().createDirectory();
        juce::AudioBuffer<float> audio (1, 48000);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (0, i, level * (float)std::sin (2.0 * M_PI * 440.0 * i / 48000.0));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        return file;
    };
    const auto recording = writeFile ("recording.wav", 0.0f);
    const auto rendering = writeFile ("rendering.wav", 0.5f);

    Project p;
    p.settings.tempoBpm = 120.0;
    Channel voice;
    voice.id = p.allocateId<ChannelId>();
    p.channels = { voice };
    Lane lane { p.allocateId<LaneId>(), "Vocal" };
    p.playlist.lanes = { lane };
    AudioClip audio;
    audio.file = recording.getFullPathName();
    audio.channel = voice.id;
    const auto clip = p.allocateId<ClipId>();
    p.playlist.clips.push_back ({ clip, lane.id, 0, 1920, false, audio });

    ExportSettings settings;
    settings.format = ExportSettings::Format::wav32Float;
    settings.tailSeconds = 0.0;
    CHECK (read (f.run (p, f.folder.getFile().getChildFile ("plain.wav"), settings).files[0]).rmsDb < -90.0f);
    settings.tunedAudio[clip] = rendering;
    CHECK (read (f.run (p, f.folder.getFile().getChildFile ("tuned.wav"), settings).files[0]).rmsDb > -12.0f);
}

TEST_CASE ("Exporting while playing writes what played: the song and its tail", "[session][export]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    // The Master 6 dB down: the file has the Master's fader in it (and the test tones stay below clipping).
    auto song = makeSong();
    song.mixer.master.volumeDb = -6.0f;
    session.getHistory().reset (song);
    session.setMode (Session::Mode::pattern);
    const Session::LoopRegion loop { 0, 480 };
    session.setLoopRegion (Session::Mode::song, loop);
    juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory());

    // The same song exported offline, to compare with.
    ExportSettings settings;
    settings.format = ExportSettings::Format::wav24;
    settings.sampleRate = 48000.0;
    settings.tailSeconds = 0.5;
    std::optional<ExportJob::Result> offline;
    auto job = session.createExportJob (folder.getFile().getChildFile ("Offline"), settings,
                                        [&] (const ExportJob::Result& r) { offline = r; });
    job->start();
    for (int i = 0; i < 600 && ! offline; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    const auto rendered = offline.value_or (ExportJob::Result {});
    REQUIRE (rendered.ok);
    job = nullptr;

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 15.0, .paceInRealTime = true });
    session.getEdit().dispatchPendingUpdatesSynchronously();
    runner.start();

    std::optional<LiveExport::Result> result;
    LiveExport live (session, folder.getFile().getChildFile ("Live"), settings,
                     [&] (const LiveExport::Result& r) { result = r; });
    live.start();
    CHECK (session.isPlaying());
    CHECK (session.getMode() == Session::Mode::song);
    CHECK (! session.getLoopRegion (Session::Mode::song).has_value()); // the whole song, once
    while (! result && runner.getSecondsProcessed() < 12.0)
        runner.runUntil (runner.getSecondsProcessed() + 0.05);
    runner.stop();

    REQUIRE (result.has_value());
    const auto exported = result.value_or (LiveExport::Result {});
    INFO (exported.error);
    REQUIRE (exported.ok);
    CHECK (live.getFile() == folder.getFile().getChildFile ("Live.wav"));
    const auto played = read (live.getFile());
    CHECK (std::abs (played.seconds - 2.5) < 0.001); // the 2 s bar and the 0.5 s tail, to the sample
    CHECK (juce::approximatelyEqual (played.sampleRate, 48000.0));
    CHECK (played.bits == 24u);
    // What the file has is what the sound card got (the test tone's level is set for offline renders only,
    // so this compares with playback, not with the offline file): the steady tone, 0.5 to 1.5 s into the song.
    auto rmsDb = [] (const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        for (auto i = from; i < to && i < x.size(); ++i)
            sum += (double)x[i] * x[i];
        return 10.0 * std::log10 (sum / (double)(to - from));
    };
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::FileInputStream (live.getFile()), true));
    REQUIRE (reader != nullptr);
    std::vector<float> file ((size_t)reader->lengthInSamples);
    juce::AudioBuffer<float> buffer (2, (int)reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
    std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + buffer.getNumSamples(), file.begin());
    // The sound card started before playback did: line the two up first.
    const auto& card = runner.getChannel (0);
    size_t offset = 0;
    double closest = 1.0e30;
    for (size_t candidate = 0; candidate < 48000 && candidate + 30256 < card.size(); ++candidate)
    {
        double error = 0.0;
        for (size_t i = 30000; i < 30256; ++i)
            error += ((double)card[candidate + i] - file[i]) * ((double)card[candidate + i] - file[i]);
        if (error < closest)
        {
            closest = error;
            offset = candidate;
        }
    }
    const std::vector<float> heard (card.begin() + (std::ptrdiff_t)offset, card.end());
    const auto fileDb = rmsDb (file, 24000, 72000), cardDb = rmsDb (heard, 24000, 72000);
    INFO ("file " << fileDb << " dB, sound card " << cardDb << " dB");
    CHECK (std::abs (fileDb - cardDb) < 0.2);
    CHECK (fileDb > -20.0);
    CHECK (fileDb < -6.0);

    // Playback is as it was, and the capture is gone from the Master.
    CHECK (! session.isPlaying());
    CHECK (session.getMode() == Session::Mode::pattern);
    CHECK (session.getLoopRegion (Session::Mode::song) == loop);
    CHECK (session.getEdit().getMasterPluginList().getPluginsOfType<engine::OutputCapturePlugin>().isEmpty());
}

TEST_CASE ("Stopping playback stops an export while playing, and leaves no file", "[session][export]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    session.getHistory().reset (makeSong());
    juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory());

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 6.0, .paceInRealTime = true });
    session.getEdit().dispatchPendingUpdatesSynchronously();
    runner.start();

    std::optional<LiveExport::Result> result;
    ExportSettings settings;
    settings.format = ExportSettings::Format::wav16;
    LiveExport live (session, folder.getFile().getChildFile ("Live"), settings,
                     [&] (const LiveExport::Result& r) { result = r; });
    live.start();
    runner.runUntil (0.5);
    session.stop();
    while (! result && runner.getSecondsProcessed() < 3.0)
        runner.runUntil (runner.getSecondsProcessed() + 0.05);
    runner.stop();

    REQUIRE (result.has_value());
    const auto stopped = result.value_or (LiveExport::Result {});
    CHECK (stopped.cancelled);
    CHECK (stopped.error.contains ("stopped before the end"));
    CHECK (! live.getFile().existsAsFile());

    // An empty playlist can't be exported this way either.
    session.getHistory().reset (Project {});
    std::optional<LiveExport::Result> empty;
    LiveExport nothing (session, folder.getFile().getChildFile ("Empty"), settings,
                        [&] (const LiveExport::Result& r) { empty = r; });
    nothing.start();
    REQUIRE (empty.has_value());
    CHECK (empty.value_or (LiveExport::Result {}).error.contains ("playlist is empty"));
}

TEST_CASE ("Export dialog screenshot for the manual", "[.screenshot][export]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    session.getHistory().reset (makeSong());
    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    ui::ExportDialog dialog (session);
    dialog.setLookAndFeel (&lookAndFeel);
    dialog.setFormat (ExportSettings::Format::wav24);
    const auto image = dialog.createComponentSnapshot (dialog.getLocalBounds(), true, 1.0f);
    dialog.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/export.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

namespace
{
/** A plugin with latency: it plays its input 480 samples late and says so (as a look-ahead limiter does). */
class LatePlugin final : public tracktion::Plugin
{
public:
    explicit LatePlugin (tracktion::PluginCreationInfo info) : tracktion::Plugin (std::move (info)) {}
    ~LatePlugin() override { notifyListenersOfDeletion(); }

    static const char* xmlTypeName;
    static const char* getPluginName() { return "Late"; }
    static juce::ValueTree create()
    {
        juce::ValueTree v (tracktion::IDs::PLUGIN);
        v.setProperty (tracktion::IDs::type, xmlTypeName, nullptr);
        return v;
    }
    static constexpr int lateBy = 480;

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    double getLatencySeconds() override { return lateBy / sampleRate; }
    int getNumOutputChannelsGivenInputs (int) override { return 2; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    void initialise (const tracktion::PluginInitialisationInfo& info) override
    {
        sampleRate = info.sampleRate;
        line.setSize (2, lateBy);
        line.clear();
        at = 0;
    }
    void deinitialise() override {}
    void applyToBuffer (const tracktion::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr)
            return;
        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            for (int ch = 0; ch < 2 && ch < fc.destBuffer->getNumChannels(); ++ch)
            {
                auto* sample = fc.destBuffer->getWritePointer (ch, fc.bufferStartSample + i);
                std::swap (*sample, *line.getWritePointer (ch, at));
            }
            at = (at + 1) % lateBy;
        }
    }
    void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

private:
    double sampleRate = 48000.0;
    juce::AudioBuffer<float> line;
    int at = 0;
};
const char* LatePlugin::xmlTypeName = "testLate";

juce::AudioBuffer<float> readAll (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio;
}

Insert& addInsert (Project& p, const juce::String& name, InsertId output = masterInsertId)
{
    Insert insert;
    insert.id = p.allocateId<InsertId>();
    insert.name = name;
    insert.output = output;
    p.mixer.inserts.push_back (insert);
    return p.mixer.inserts.back();
}

Channel& addChannel (Project& p, const char* stockType, InsertId insert, float volumeDb = 0.0f)
{
    Channel c;
    c.id = p.allocateId<ChannelId>();
    c.name = stockType;
    c.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, stockType, {}, false };
    c.volumeDb = volumeDb;
    c.targetInsert = insert;
    p.channels.push_back (c);
    return p.channels.back();
}
} // namespace

TEST_CASE ("Stems add up to the mix: a send return's stem is its wet sound, in time with the mix", "[engine][export]")
{
    Fixture f;
    f.host.getEngine().getPluginManager().createBuiltInType<LatePlugin>();
    f.devices.registerStockDevice ("test.late", [] (tracktion::Edit& edit, const Device&)
                                   { return edit.getPluginCache().createNewPlugin (LatePlugin::xmlTypeName, {}); });
    f.devices.registerStockDevice ("test.noise",
                                   [] (tracktion::Edit& edit, const Device&)
                                   {
                                       auto p = edit.getPluginCache().createNewPlugin (
                                           tracktion::ToneGeneratorPlugin::xmlTypeName, {});
                                       auto& tone = dynamic_cast<tracktion::ToneGeneratorPlugin&> (*p);
                                       tone.oscTypeParam->setParameter (5.0f, juce::dontSendNotification); // noise
                                       tone.levelParam->setParameter (0.1f, juce::dontSendNotification);
                                       return p;
                                   });
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    // Lead plays into a bus and sends to a return (as the senders of the showcase song's reverbs play into
    // buses); Late sends there too, through a plugin with latency; Pad goes straight out.
    auto song = makeSong();
    song.mixer.inserts.clear();
    song.channels.clear();
    const auto bus = addInsert (song, "Bus").id;
    const auto ret = addInsert (song, "Return").id;
    auto& lead = addInsert (song, "Lead", bus);
    lead.sends = { { ret, -6.0f } };
    const auto leadId = lead.id;
    auto& late = addInsert (song, "Late");
    late.sends = { { ret, -3.0f } };
    late.effects.push_back ({ Device { song.allocateId<DeviceId>(), Device::Kind::stock, "test.late", {}, false } });
    const auto lateId = late.id;
    const auto pad = addInsert (song, "Pad").id;
    addChannel (song, "test.tone", leadId);
    addChannel (song, "test.noise", lateId);
    addChannel (song, "test.tone", pad, -12.0f);

    ExportSettings settings;
    settings.format = ExportSettings::Format::wav32Float;
    settings.stems = true;
    const auto result = f.run (song, folder.getChildFile ("Song.wav"), settings);
    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.files.size() == 6); // the mix, then Bus, Return, Lead, Late, Pad

    std::map<juce::String, juce::AudioBuffer<float>> stems;
    for (size_t i = 1; i < result.files.size(); ++i)
        stems[result.files[i].getFileNameWithoutExtension().fromFirstOccurrenceOf (" ", false, false)] =
            readAll (result.files[i]);
    const auto mix = readAll (result.files[0]);
    REQUIRE (stems.size() == 5);

    for (auto& [name, audio] : stems)
    {
        INFO (name);
        CHECK (audio.getNumSamples() == mix.getNumSamples());
        CHECK (audio.getRMSLevel (0, 0, 48000) > 0.001f); // none silent: the return neither
    }

    // The return's stem is the sends' sound: Lead 6 dB down plus Late 3 dB down, nothing dry.
    const auto& wet = stems["Return"];
    float worstReturn = 0.0f, worstSum = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < mix.getNumSamples(); ++i)
        {
            const auto sends = stems["Lead"].getSample (ch, i) * juce::Decibels::decibelsToGain (-6.0f) +
                               stems["Late"].getSample (ch, i) * juce::Decibels::decibelsToGain (-3.0f);
            worstReturn = std::max (worstReturn, std::abs (wet.getSample (ch, i) - sends));

            // What goes to the master adds up to the mix (Lead is in the bus's stem). Not in the first block: there
            // Tracktion's mix of a song with latency is off (the stems are right: as without the latency).
            if (i < 512)
                continue;
            const auto sum = stems["Bus"].getSample (ch, i) + wet.getSample (ch, i) + stems["Late"].getSample (ch, i) +
                             stems["Pad"].getSample (ch, i);
            worstSum = std::max (worstSum, std::abs (mix.getSample (ch, i) - sum));
        }
    CHECK (worstReturn < 1.0e-4f);
    CHECK (worstSum < 1.0e-4f);
}

TEST_CASE ("A muted master exports silence; the stems, which skip the master, do not change", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    ExportSettings settings;
    settings.stems = true;
    auto song = makeSong();
    const auto heard = f.run (song, folder.getChildFile ("Heard.wav"), settings);
    REQUIRE (heard.ok);
    song.mixer.master.muted = true;
    const auto muted = f.run (song, folder.getChildFile ("Muted.wav"), settings);
    REQUIRE (muted.ok);
    REQUIRE (muted.files.size() == 3);

    CHECK (read (heard.files[0]).rmsDb > -30.0f);
    CHECK (read (muted.files[0]).rmsDb <= -100.0f);
    CHECK (std::abs (read (muted.files[1]).rmsDb - read (heard.files[1]).rmsDb) < 0.01f);
    CHECK (std::abs (read (muted.files[2]).rmsDb - read (heard.files[2]).rmsDb) < 0.01f);
}

TEST_CASE ("Stems of muted and soloed inserts: a mute silences a stem, a solo changes only the mix", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    ExportSettings settings;
    settings.stems = true;
    auto song = makeSong();
    const auto plain = f.run (song, folder.getChildFile ("Plain.wav"), settings);
    REQUIRE (plain.ok);

    // Pad soloed: the mix is Pad alone; the stems are the whole song's, as without the solo.
    song.mixer.inserts[1].solo = true;
    const auto soloed = f.run (song, folder.getChildFile ("Soloed.wav"), settings);
    INFO (soloed.error);
    REQUIRE (soloed.ok);
    REQUIRE (soloed.files.size() == 3);
    CHECK (std::abs (read (soloed.files[0]).rmsDb - read (plain.files[2]).rmsDb) < 0.1f); // the mix: Pad's sound
    CHECK (std::abs (read (soloed.files[1]).rmsDb - read (plain.files[1]).rmsDb) < 0.01f);
    CHECK (std::abs (read (soloed.files[2]).rmsDb - read (plain.files[2]).rmsDb) < 0.01f);
    CHECK (folder.findChildFiles (juce::File::findFiles, true, "*").size() == 6); // no scratch files left

    // Pad muted: its stem is there, silent.
    song.mixer.inserts[1].solo = false;
    song.mixer.inserts[1].muted = true;
    const auto muted = f.run (song, folder.getChildFile ("Muted.wav"), settings);
    REQUIRE (muted.ok);
    REQUIRE (muted.files.size() == 3);
    CHECK (read (muted.files[1]).rmsDb > -30.0f);
    CHECK (read (muted.files[2]).rmsDb <= -100.0f);
}

TEST_CASE ("Stems in other formats are encoded from their own capture", "[engine][export]")
{
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    ExportSettings settings;
    settings.stems = true;
    settings.format = ExportSettings::Format::flac;
    const auto result = f.run (makeSong(), folder.getChildFile ("Song"), settings);
    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.files.size() == 3);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<float> levels;
    for (auto& file : result.files)
    {
        CHECK (file.getFileExtension() == ".flac");
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        REQUIRE (reader != nullptr);
        juce::AudioBuffer<float> audio ((int)reader->numChannels, 48000);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
        levels.push_back (juce::Decibels::gainToDecibels (audio.getRMSLevel (0, 0, audio.getNumSamples())));
    }
    CHECK (std::abs ((levels[1] - levels[2]) - 12.0f) < 0.5f); // Lead, and Pad 12 dB down
    // Only the files asked for: the WAVs they were encoded from are gone.
    CHECK (folder.findChildFiles (juce::File::findFiles, true, "*").size() == 3);
}

TEST_CASE ("Stems come from the mix's render: 16 stems take about as long as the mix", "[engine][export]")
{
    // One render per stem took 15 times as long as the mix for the showcase song's 38 stems (189 s, the mix 13 s).
    Fixture f;
    const auto folder = f.folder.getFile();
    REQUIRE (folder.createDirectory());

    auto song = makeSong();
    song.mixer.inserts.clear();
    song.channels.clear();
    for (int i = 0; i < 16; ++i)
        addChannel (song, "test.tone", addInsert (song, "Insert " + juce::String (i + 1)).id, -24.0f);

    ExportSettings settings;
    const auto timed = [&] (bool stems)
    {
        settings.stems = stems;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        const auto result = f.run (song, folder.getChildFile (stems ? "Stems.wav" : "Mix.wav"), settings);
        REQUIRE (result.ok);
        CHECK (result.files.size() == (stems ? 17u : 1u));
        return (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
    };
    const auto mixSeconds = timed (false);
    const auto stemSeconds = timed (true);
    INFO ("mix " << mixSeconds << " s, mix and 16 stems " << stemSeconds << " s");
    CHECK (stemSeconds < 3.0 * mixSeconds + 0.5);
}

TEST_CASE ("Exporting while playing with the master muted writes silence, as was heard", "[session][export]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    auto song = makeSong();
    song.mixer.master.muted = true;
    session.getHistory().reset (song);
    juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory());

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 8.0, .paceInRealTime = true });
    session.getEdit().dispatchPendingUpdatesSynchronously();
    runner.start();

    std::optional<LiveExport::Result> result;
    ExportSettings settings;
    settings.format = ExportSettings::Format::wav24;
    settings.tailSeconds = 0.2;
    LiveExport live (session, folder.getFile().getChildFile ("Live"), settings,
                     [&] (const LiveExport::Result& r) { result = r; });
    live.start();
    while (! result && runner.getSecondsProcessed() < 6.0)
        runner.runUntil (runner.getSecondsProcessed() + 0.05);
    runner.stop();

    REQUIRE (result.has_value());
    REQUIRE (result.value_or (LiveExport::Result {}).ok);
    CHECK (read (live.getFile()).rmsDb <= -100.0f);
    const auto& card = runner.getChannel (0);
    double sum = 0.0;
    for (auto x : card)
        sum += (double)x * x;
    CHECK (sum < 1.0e-6);
}

TEST_CASE ("An export waits for an instrument that loads its sounds in the background", "[engine][export]")
{
    // The difficult synth stays silent for its first 3 seconds, as big sample libraries do while they load. An
    // export makes a fresh one and renders faster than real time: without waiting, the song's first note is lost.
    EngineHost host ({ .openAudioDevice = false });
    plugins::ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    const auto file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("difficult-synth.clap");
    clap.findAllTypesForFile (found, file.getFullPathName());
    const juce::PluginDescription* synth = nullptr;
    for (auto* d : found)
        if (d->name == "Spinus Difficult Synth")
            synth = d;
    REQUIRE (synth != nullptr);
    host.getEngine().getPluginManager().knownPluginList.addType (*synth);
    Session session (host.getEngine(), std::make_unique<engine::DefaultDeviceFactory>());
    auto run = [&] (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
        return raw->getResult();
    };
    const auto channel = run (
        "channel.add",
        R"({"name": "Difficult", "instrument": {"kind": "plugin", "plugin": {"format": "CLAP", "name": "Spinus Difficult Synth", "manufacturer": )" +
            juce::JSON::toString (synth->manufacturerName) + R"(, "fileOrIdentifier": )" +
            juce::JSON::toString (file.getFullPathName()) + "}}}");
    const auto pattern = run ("pattern.add", R"({"name": "P", "length": 3840})");
    run ("pattern.setNotes", R"({"pattern": )" + pattern["pattern"].toString() + R"(, "channel": )" +
                                 channel["channel"].toString() +
                                 R"(, "notes": [{"start": 0, "length": 3600, "pitch": 60, "velocity": 0.9}]})");
    const auto lane = run ("lane.add", "{}");
    run ("clip.addPattern", R"({"lane": )" + lane["lane"].toString() + R"(, "pattern": )" +
                                pattern["pattern"].toString() + R"(, "start": 0})");
    session.getEdit().dispatchPendingUpdatesSynchronously();

    juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory());
    ExportSettings settings;
    settings.format = ExportSettings::Format::wav24;
    settings.sampleRate = 48000.0;
    settings.tailSeconds = 0.5;
    std::optional<ExportJob::Result> result;
    auto job = session.createExportJob (folder.getFile().getChildFile ("song.wav"), settings,
                                        [&] (const ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    job = nullptr;
    REQUIRE (result.has_value());
    const auto exported = result.value_or (ExportJob::Result {});
    REQUIRE (exported.ok);
    REQUIRE (! exported.files.empty());
    CHECK (read (exported.files.at (0)).rmsDb > -40.0f); // the note, from its first beat
}
