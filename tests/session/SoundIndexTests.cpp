#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/threeosc/ThreeOscPresets.h"
#include "session/SoundDescriptors.h"
#include "session/ProgramFolder.h"
#include "session/SoundIndex.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>
#include <random>

using namespace spinus;

namespace
{
constexpr double pi = 3.14159265358979323846;

struct TempFolder
{
    TempFolder() { folder.createDirectory(); }
    ~TempFolder() { folder.deleteRecursively(); }
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-sound-index-" + juce::Uuid().toString());
};

bool writeWav (const juce::File& file, const std::function<double (double)>& f, double seconds, double rate = 44100.0)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::AudioBuffer<float> b (1, (int)(seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i, (float)f (i / rate));
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

void writeKick (const juce::File& file)
{
    double phase = 0.0;
    writeWav (
        file,
        [&] (double t)
        {
            phase += (50.0 + 100.0 * std::exp (-t / 0.03)) / 44100.0;
            return 0.9 * std::sin (2 * pi * phase) * std::exp (-t / 0.15);
        },
        0.8);
}

void writeHat (const juce::File& file, unsigned seed = 7)
{
    std::mt19937 random (seed); // the same noise every time
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    juce::IIRFilter hp1, hp2;
    hp1.setCoefficients (juce::IIRCoefficients::makeHighPass (44100.0, 7000.0));
    hp2.setCoefficients (juce::IIRCoefficients::makeHighPass (44100.0, 7000.0));
    writeWav (
        file,
        [&] (double t)
        {
            const auto v = hp2.processSingleSampleRaw (hp1.processSingleSampleRaw ((float)u (random)));
            return 1.5 * v * std::exp (-t / 0.03);
        },
        0.3);
}

void writeBass (const juce::File& file)
{
    writeWav (
        file, [] (double t) { return 0.5 * (2.0 * (55.0 * t - std::floor (55.0 * t)) - 1.0) * (t < 1.0 ? 1.0 : 0.0); },
        1.2);
}

SoundSource testSynth (const juce::String& file = "test-synth.clap", const juce::String& name = "Spinus Test Synth")
{
    SoundSource s;
#if defined(SPINUS_BROKEN_PLUGINS_DIR)
    s.file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile (file).getFullPathName();
#endif
    s.key = "plugin-" + file;
    s.name = name;
    s.format = "CLAP";
    s.pluginName = name;
    s.badge = "CLAP";
    s.fingerprint = "1";
    return s;
}

SoundSource threeOsc()
{
    SoundSource s;
    s.key = "stock-spinus.3x-osc";
    s.name = "3x Osc";
    s.format = "Spinus";
    s.file = "spinus:3x-osc";
    s.stockType = "spinus.3x-osc";
    s.badge = "Spinus";
    s.fingerprint = "Spinus test";
    return s;
}

// Short limits only where a test waits for a hang; a Debug + AddressSanitizer build needs well over 4 s
// to render some presets.
SoundIndexer::Options helperOptions (int timeoutMs = 60000)
{
    SoundIndexer::Options o;
#if defined(SPINUS_PLUGIN_SCANNER)
    o.helper = juce::File (SPINUS_PLUGIN_SCANNER);
#endif
    o.soundTimeoutMs = timeoutMs;
    o.loadTimeoutMs = timeoutMs;
    return o;
}

const juce::var* findSound (const SoundIndex::Instrument& i, const juce::String& name)
{
    for (const auto& s : i.sounds)
        if (s["name"].toString() == name)
            return &s;
    return nullptr;
}
} // namespace

//==============================================================================
TEST_CASE ("Sound index: the format on disk, with versions", "[sounds][index]")
{
    TempFolder temp;
    SoundIndex index (temp.folder);
    SoundIndex::Instrument i;
    i.source = threeOsc();
    i.complete = true;
    i.problemPresets.add ("program:3:Broken");
    auto* sound = new juce::DynamicObject();
    sound->setProperty ("id", "program:1:Bass");
    sound->setProperty ("version", "v1");
    sound->setProperty ("name", "Bass");
    sound->setProperty ("role", "bass");
    i.sounds.add (juce::var (sound));
    index.instruments[i.source.key] = i;
    auto* info = new juce::DynamicObject();
    info->setProperty ("kind", "one-shot");
    info->setProperty ("class", "kick");
    index.samples.push_back ({ "/x/kick.wav", 1234, 5678, juce::var (info) });
    index.sampleFolders.add ("/x");
    index.updated = juce::Time (2026, 9, 26, 12, 0);
    REQUIRE (index.saveInstrument (index.instruments.begin()->second));
    REQUIRE (index.saveSamples());
    REQUIRE (index.saveIndex());

    const auto json = juce::JSON::parse (temp.folder.getChildFile ("index.json"));
    CHECK (json["kind"].toString() == "spinus.sound-index");
    CHECK ((int)json["version"] == SoundIndex::version);
    CHECK ((int)json["descriptorsVersion"] == sounds::descriptorsVersion);
    CHECK (temp.folder.getChildFile ("instruments/stock-spinus.3x-osc.json").existsAsFile());

    SoundIndex again (temp.folder);
    again.load();
    REQUIRE (again.instruments.size() == 1);
    const auto& back = again.instruments.begin()->second;
    CHECK (back.source.name == "3x Osc");
    CHECK (back.source.format == "Spinus");
    CHECK (back.complete);
    CHECK (back.problemPresets == juce::StringArray ("program:3:Broken"));
    REQUIRE (back.sounds.size() == 1);
    CHECK (back.sounds[0]["role"].toString() == "bass");
    REQUIRE (again.samples.size() == 1);
    CHECK (again.samples[0].size == 1234);
    CHECK (again.samples[0].modified == 5678);
    CHECK (again.samples[0].info["class"].toString() == "kick");
    CHECK (again.sampleFolders == juce::StringArray ("/x"));
    const auto summary = again.summarize();
    CHECK (summary.instruments == 1);
    CHECK (summary.presets == 1);
    CHECK (summary.samples == 1);
    CHECK (summary.problems == 1);
    CHECK (again.getDiskUse() > 0);

    // Another version is ignored: everything is measured again.
    auto edited = juce::JSON::parse (temp.folder.getChildFile ("index.json"));
    edited.getDynamicObject()->setProperty ("version", SoundIndex::version + 1);
    temp.folder.getChildFile ("index.json").replaceWithText (juce::JSON::toString (edited));
    SoundIndex older (temp.folder);
    older.load();
    CHECK (older.instruments.empty());
    CHECK (older.needsVisit (threeOsc()));

    CHECK (older.removeAll());
    CHECK_FALSE (temp.folder.exists());
}

TEST_CASE ("Sound index: only what changed is measured again", "[sounds][index]")
{
    SoundIndex index (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-no-index"));
    auto source = threeOsc();
    CHECK (index.needsVisit (source)); // new

    SoundIndex::Instrument i;
    i.source = source;
    i.complete = true;
    index.instruments[source.key] = i;
    CHECK_FALSE (index.needsVisit (source)); // unchanged
    auto changed = source;
    changed.fingerprint = "Spinus newer";
    CHECK (index.needsVisit (changed));
    index.instruments[source.key].complete = false; // stopped part way
    CHECK (index.needsVisit (source));
    index.instruments[source.key].complete = true;
    index.instruments[source.key].presetFiles = true; // more preset files may have come
    CHECK (index.needsVisit (source));
    index.instruments[source.key].presetFiles = false;
    index.instruments[source.key].problem = "crashed while loading"; // not again until it changes
    CHECK_FALSE (index.needsVisit (source));
    CHECK (index.needsVisit (changed));

    TempFolder temp;
    const auto pack = temp.folder.getChildFile ("Pack");
    writeKick (pack.getChildFile ("Kicks/k1.wav"));
    writeHat (pack.getChildFile ("h1.wav"));
    pack.getChildFile ("readme.txt").replaceWithText ("not audio");
    pack.getChildFile ("empty.wav").create();
    const auto files = SoundIndex::listSampleFiles ({ pack, pack.getChildFile ("Kicks") }); // overlapping folders
    REQUIRE (files.size() == 2);
    CHECK (files[0].file.getFullPathName() < files[1].file.getFullPathName()); // by path
    CHECK (files[0].file.getFileName() == "k1.wav");                           // "Pack/Kicks/..." before "Pack/h1.wav"
    CHECK (files[1].file.getFileName() == "h1.wav");

    for (const auto& f : files)
        index.samples.push_back ({ f.file.getFullPathName(), f.size, f.modified, {} });
    CHECK (index.changedSamples (files).empty());
    auto touched = files;
    touched[0].modified += 1000;
    const auto changedFiles = index.changedSamples (touched);
    REQUIRE (changedFiles.size() == 1);
    CHECK (changedFiles[0].file.getFileName() == "k1.wav");
}

//==============================================================================
#if defined(SPINUS_PLUGIN_SCANNER) && defined(SPINUS_BROKEN_PLUGINS_DIR)
TEST_CASE ("Sound index: Spinus's 3x Osc, a CLAP synth and a sample folder, in the helper", "[sounds][index][helper]")
{
    TempFolder temp;
    const auto pack = temp.folder.getChildFile ("My samples");
    writeKick (pack.getChildFile ("a1.wav"));
    writeHat (pack.getChildFile ("a2.wav"));
    writeBass (pack.getChildFile ("Basses/b1.wav"));
    pack.getChildFile ("broken.wav").replaceWithText ("RIFF, but not really a WAV file");
    {
        // An AIFF file named .wav (some packs have them): read by its contents.
        juce::AudioBuffer<float> b (1, 22050);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, (float)(0.5 * std::sin (2 * pi * 55.0 * i / 44100.0) * std::exp (-i / 8000.0)));
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::FileOutputStream> (pack.getChildFile ("aiff inside.wav"));
        auto writer = juce::AiffAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
    writeWav (pack.getChildFile ("empty.wav"), [] (double) { return 0.0; }, 0.0); // a header, no sound

    SoundIndex index (temp.folder.getChildFile ("index"));
    const std::vector<SoundSource> sources { threeOsc(), testSynth() };
    std::vector<SoundIndexer::Progress> seen;
    auto result = SoundIndexer::update (index, helperOptions(), sources, { pack }, {},
                                        [&] (const SoundIndexer::Progress& p) { seen.push_back (p); });
    CHECK_FALSE (result.cancelled);
    CHECK (result.instrumentsVisited == 2);
    CHECK (result.problems == 0);
    CHECK_FALSE (seen.empty());

    // 3x Osc: each factory preset (its programs, without "Default").
    const auto& osc = index.instruments.at ("stock-spinus.3x-osc");
    CHECK (osc.problem.isEmpty());
    CHECK (osc.complete);
    CHECK (osc.sounds.size() == (int)dsp::threeOscPresets::all().size());
    for (const auto& s : osc.sounds)
    {
        CHECK_FALSE ((bool)s["silent"]);
        CHECK (sounds::roles().contains (s["role"].toString()));
    }

    // The test synth's six presets: one voice, so no chords; the roles its settings make.
    const auto& synth = index.instruments.at ("plugin-test-synth.clap");
    CHECK (synth.problem.isEmpty());
    REQUIRE (synth.sounds.size() == 6);
    for (const auto& s : synth.sounds)
    {
        // (Bright Saw blows up; Sine Pluck is a 20 ms blip, too short to hear its pitch.)
        if (s["name"].toString() == "Bright Saw" || s["name"].toString() == "Sine Pluck")
            continue;
        CHECK ((bool)s["tracksPitch"]);
        CHECK_FALSE ((bool)s["polyphonic"]);
    }
    REQUIRE (findSound (synth, "Soft Sine Pad") != nullptr);
    CHECK ((*findSound (synth, "Soft Sine Pad"))["role"].toString() == "pad");
    CHECK ((*findSound (synth, "Square Pluck"))["role"].toString() == "pluck");
    CHECK ((*findSound (synth, "Sine Pluck"))["role"].toString() == "pluck");
    CHECK (juce::StringArray { "bass", "lead" }.contains ((*findSound (synth, "Resonant Saw"))["role"].toString()));
    // The test synth's filter blows up with Bright Saw's settings (infinite samples): noted, not measured.
    CHECK ((bool)(*findSound (synth, "Bright Saw"))["invalid"]);
    CHECK ((*findSound (synth, "Bright Saw"))["role"].toString() == "none");
    CHECK ((*findSound (synth, "Dark Square"))["role"].toString() == "bass");

    // The samples (the broken file noted, not fatal).
    REQUIRE (index.samples.size() == 6);
    std::map<juce::String, juce::var> byName;
    for (const auto& s : index.samples)
        byName[juce::File (s.path).getFileName()] = s.info;
    CHECK (byName["a1.wav"]["class"].toString() == "kick");
    CHECK (byName["a2.wav"]["class"].toString() == "hat");
    CHECK (byName["b1.wav"]["class"].toString() == "bass");
    CHECK (byName["broken.wav"].hasProperty ("problem"));
    CHECK (byName["empty.wav"]["problem"].toString() == "It holds no sound.");
    CHECK (byName["aiff inside.wav"]["kind"].toString() == "one-shot");
    const auto summary = index.summarize();
    CHECK (summary.instruments == 2);
    CHECK (summary.samples == 4);

    // Kept on disk.
    SoundIndex kept (temp.folder.getChildFile ("index"));
    kept.load();
    CHECK (kept.instruments.size() == 2);
    CHECK (kept.samples.size() == 6);
    CHECK (kept.rows().size() == (size_t)(osc.sounds.size() + 6 + 6));

    // Nothing changed: nothing is measured again.
    result = SoundIndexer::update (kept, helperOptions(), sources, { pack }, {}, {});
    CHECK (result.instrumentsVisited == 0);
    CHECK (result.soundsMeasured == 0);
    CHECK (result.samplesMeasured == 0);

    // A sample changed, one removed, one added; an instrument gone and one changed.
    writeHat (pack.getChildFile ("a1.wav"));
    pack.getChildFile ("a1.wav").setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::hours (1));
    pack.getChildFile ("a2.wav").deleteFile();
    writeKick (pack.getChildFile ("a3.wav"));
    auto synthChanged = testSynth();
    synthChanged.fingerprint = "2";
    result = SoundIndexer::update (kept, helperOptions(), { synthChanged }, { pack }, {}, {});
    CHECK (result.instrumentsVisited == 1);
    CHECK (result.soundsMeasured == 6);
    CHECK (result.samplesMeasured == 2);
    CHECK (result.samplesRemoved == 1);
    CHECK (kept.instruments.size() == 1);
    CHECK_FALSE (temp.folder.getChildFile ("index/instruments/stock-spinus.3x-osc.json").exists());
    byName.clear();
    for (const auto& s : kept.samples)
        byName[juce::File (s.path).getFileName()] = s.info;
    CHECK (byName.size() == 6);
    CHECK (byName["a1.wav"]["class"].toString() == "hat");
    CHECK (byName["a3.wav"]["class"].toString() == "kick");
}

TEST_CASE ("Sound index: presets that crash or hang the helper are left out", "[sounds][index][helper]")
{
    TempFolder temp;
    SoundIndex index (temp.folder);
    const auto source = testSynth ("misbehaving-synth.clap", "Spinus Misbehaving Synth");
    const auto result = SoundIndexer::update (index, helperOptions(), { source }, {}, {}, {});
    const auto& entry = index.instruments.at (source.key);
    CHECK (entry.problem.isEmpty());
    CHECK (entry.complete);
    CHECK (entry.sounds.size() == 6);
    CHECK (entry.problemPresets.size() == 2);
    CHECK (result.problems == 2);
    CHECK (findSound (entry, "Crashes") == nullptr);
    CHECK (findSound (entry, "Hangs") == nullptr);
    // Not tried again.
    SoundIndex again (temp.folder);
    again.load();
    CHECK (again.instruments.at (source.key).problemPresets.size() == 2);
}

TEST_CASE ("Sound index: an instrument that crashes listing its presets and loads its sounds late is still measured",
           "[sounds][index][helper]")
{
    // As some big sample instruments do (BFD Player crashed listing its presets; Ample's are silent until their
    // samples are loaded): its sound as it opens is measured, once it sounds.
    TempFolder temp;
    SoundIndex index (temp.folder);
    const auto source = testSynth ("difficult-synth.clap", "Spinus Difficult Synth");
    const auto result = SoundIndexer::update (index, helperOptions(), { source }, {}, {}, {});
    const auto& entry = index.instruments.at (source.key);
    CHECK (entry.problem.isEmpty());
    CHECK (entry.complete);
    CHECK (result.problems == 0);
    REQUIRE (entry.sounds.size() == 1);
    CHECK (entry.sounds[0]["name"].toString() == "Default sound");
    CHECK_FALSE ((bool)entry.sounds[0]["silent"]);
    CHECK (entry.sounds[0]["role"].toString() != "none");
}

TEST_CASE ("Sound index: an instrument that crashes or hangs while loading is a problem", "[sounds][index][helper]")
{
    TempFolder temp;
    SoundIndex index (temp.folder);
    auto crash = testSynth ("crash.clap", "Crash");
    auto hang = testSynth ("hang.clap", "Hang");
    auto missing = testSynth ("missing.clap", "Missing");
    const auto result = SoundIndexer::update (index, helperOptions (4000), { crash, hang, missing }, {}, {}, {});
    CHECK (result.problems == 3);
    CHECK (index.instruments.at (crash.key).problem.contains ("crashed while loading"));
    CHECK (index.instruments.at (hang.key).problem.contains ("stopped answering while loading"));
    CHECK (index.instruments.at (missing.key).problem.isNotEmpty());
    for (const auto& s : { crash, hang, missing })
        CHECK_FALSE (index.needsVisit (s)); // not again until it changes
}

TEST_CASE ("Sound index: cancelled, it carries on from where it stopped", "[sounds][index][helper]")
{
    TempFolder temp;
    SoundIndex index (temp.folder);
    std::atomic<int> sounds { 0 };
    auto result = SoundIndexer::update (
        index, helperOptions(), { testSynth() }, {}, [&] { return sounds.load() >= 2; },
        [&] (const SoundIndexer::Progress& p) { sounds = p.sounds; });
    CHECK (result.cancelled);
    const auto& entry = index.instruments.at ("plugin-test-synth.clap");
    CHECK_FALSE (entry.complete);
    const auto first = entry.sounds.size();
    CHECK (first >= 2);
    CHECK (first < 6);

    SoundIndex again (temp.folder);
    again.load();
    CHECK (again.needsVisit (testSynth()));
    result = SoundIndexer::update (again, helperOptions(), { testSynth() }, {}, {}, {});
    CHECK_FALSE (result.cancelled);
    CHECK (result.soundsMeasured == 6 - first);
    CHECK (again.instruments.at ("plugin-test-synth.clap").sounds.size() == 6);
    CHECK (again.instruments.at ("plugin-test-synth.clap").complete);
}

TEST_CASE ("Sound index: the background indexer reports on the message thread", "[sounds][index][helper]")
{
    TempFolder temp;
    SoundIndexer indexer (temp.folder, helperOptions());
    std::optional<SoundIndexer::Result> finished;
    int progressCalls = 0;
    REQUIRE (indexer.start (
        { testSynth() }, {}, [&] (const SoundIndexer::Progress&) { ++progressCalls; },
        [&] (const SoundIndexer::Result& r) { finished = r; }));
    CHECK (SoundIndexer::isAnyRunning());
    for (int i = 0; i < 600 && ! finished; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (finished);
    CHECK (finished.value().soundsMeasured == 6);
    CHECK (progressCalls > 0);
    CHECK_FALSE (SoundIndexer::isAnyRunning());
}
#endif

//==============================================================================
/** The sample index on the folders of the Browser's sample list (sample-folders.txt in the settings folder of
    the HOME it runs with), timed, then updated again with nothing changed. The index goes to that HOME's
    settings too. Never writes in the sample folders. Run by hand: spinus_tests "[.realsamples]". */
TEST_CASE ("Sound index on the Browser's real sample folders", "[.realsamples]")
{
    juce::Array<juce::File> folders;
    for (const auto& line :
         juce::StringArray::fromLines (getSettingsFolder().getChildFile ("sample-folders.txt").loadFileAsString()))
        if (line.trim().isNotEmpty())
            folders.add (juce::File (line.trim()));
    if (folders.isEmpty())
        SKIP ("no sample folders in " + getSettingsFolder().getFullPathName().toStdString());
    SoundIndex index (SoundIndex::getDefaultFolder());
    index.load();
    auto options = helperOptions();
    options.soundTimeoutMs = 30000;
    options.loadTimeoutMs = 60000;
    auto started = juce::Time::getMillisecondCounterHiRes();
    auto result = SoundIndexer::update (index, options, {}, folders, {}, {});
    const auto first = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    started = juce::Time::getMillisecondCounterHiRes();
    const auto again = SoundIndexer::update (index, options, {}, folders, {}, {});
    const auto second = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    std::cout << "measured " << result.samplesMeasured << " samples (" << result.problems << " problems) in " << first
              << " s; nothing changed: " << again.samplesMeasured << " measured in " << second << " s; index "
              << index.getFolder().getFullPathName() << "\n";
    CHECK (again.samplesMeasured == 0);
}
