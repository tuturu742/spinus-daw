/*  Golden-audio tests: render reference projects offline and compare them with stored
    "fingerprints" (the level of each channel in 50 ms windows), to catch audio regressions.

    Each reference project lives in tests/golden/<name>.json (a normal project.json) next to its
    fingerprint <name>.golden.json. After a deliberate sound change, regenerate both with:
        SPINUS_UPDATE_GOLDEN=1 ./build/release/tests/spinus_tests "[golden]"
    and explain the change in the pull request.
*/

#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"

using namespace spinus;
using namespace spinus::model;
namespace te = tracktion;

namespace
{

constexpr double renderSeconds = 4.0;
constexpr double sampleRate = 48000.0;
constexpr int windowSamples = 2400; // 50 ms
constexpr float floorDb = -90.0f;

juce::File goldenDir() { return juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("golden"); }

bool updating() { return juce::SystemStats::getEnvironmentVariable ("SPINUS_UPDATE_GOLDEN", {}) == "1"; }

Device stock (Project& p, const char* type)
{
    return { p.allocateId<DeviceId>(), Device::Kind::stock, type, {}, false };
}

Note note (int beat16th, int pitch, float velocity = 0.8f)
{
    return { beat16th * ticksPerQuarterNote / 4, ticksPerQuarterNote / 4, pitch, velocity, 0.5f, 0.0f, 0.0f, {} };
}

//==============================================================================
// Reference projects. They only use devices the test factory provides (Tracktion built-ins standing in
// for Spinus's stock devices until those exist).

/** A sine tone through an insert with a compressor, a send to a reverb bus, and a quieter master. */
Project mixerProject()
{
    Project p;
    p.settings.tempoBpm = 120.0;

    Insert reverb;
    reverb.id = p.allocateId<InsertId>();
    reverb.name = "Reverb";
    reverb.effects.push_back ({ stock (p, "spinus.reverb"), 1.0f });

    Insert lead;
    lead.id = p.allocateId<InsertId>();
    lead.name = "Lead";
    lead.volumeDb = -3.0f;
    lead.pan = -0.5f;
    lead.effects.push_back ({ stock (p, "bundled.lsp.compressor-stereo"), 1.0f });
    lead.sends.push_back ({ reverb.id, -6.0f });
    p.mixer.inserts = { lead, reverb };
    p.mixer.master.volumeDb = -2.0f;

    Channel tone;
    tone.id = p.allocateId<ChannelId>();
    tone.name = "Tone";
    tone.instrument = stock (p, "test.tone");
    tone.targetInsert = lead.id;
    p.channels = { tone };
    return p;
}

/** A synth playing a pattern twice; the second placement starts later in the pattern and a muted lane
    holds a third placement that must stay silent. */
Project patternProject()
{
    Project p;
    p.settings.tempoBpm = 150.0;

    Insert synthBus;
    synthBus.id = p.allocateId<InsertId>();
    synthBus.name = "Synth";
    p.mixer.inserts = { synthBus };

    Channel synth;
    synth.id = p.allocateId<ChannelId>();
    synth.name = "Synth";
    synth.instrument = stock (p, "spinus.sampler"); // stands in as 4OSC
    synth.targetInsert = synthBus.id;
    p.channels = { synth };

    Pattern pattern;
    pattern.id = p.allocateId<PatternId>();
    pattern.name = "Riff";
    pattern.length = ticksPerQuarterNote * 4;
    pattern.notes[synth.id] = { note (0, 48), note (4, 55, 0.6f), note (8, 60), note (12, 55, 0.4f) };
    p.patterns = { pattern };

    Lane a { p.allocateId<LaneId>(), "A", 0xff3d4450, false };
    Lane muted { p.allocateId<LaneId>(), "Muted", 0xff3d4450, true };
    p.playlist.lanes = { a, muted };
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), a.id, 0, pattern.length, false, PatternClip { pattern.id, 0 } });
    p.playlist.clips.push_back ({ p.allocateId<ClipId>(), a.id, pattern.length * 2, pattern.length, false,
                                  PatternClip { pattern.id, ticksPerQuarterNote * 2 } });
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), muted.id, pattern.length, pattern.length, false, PatternClip { pattern.id, 0 } });
    return p;
}

/** Two tones through a chain of inserts (channel -> A -> B -> master), one channel muted half-way along
    the chain by solo. */
Project routingProject()
{
    Project p;

    Insert b;
    b.id = p.allocateId<InsertId>();
    b.name = "B";
    b.volumeDb = -6.0f;

    Insert a;
    a.id = p.allocateId<InsertId>();
    a.name = "A";
    a.output = b.id;
    a.pan = 0.75f;
    p.mixer.inserts = { a, b };

    Channel one;
    one.id = p.allocateId<ChannelId>();
    one.name = "One";
    one.instrument = stock (p, "test.tone");
    one.targetInsert = a.id;

    Channel two;
    two.id = p.allocateId<ChannelId>();
    two.name = "Two";
    two.instrument = stock (p, "test.tone");
    two.volumeDb = -12.0f;
    two.targetInsert = b.id;
    p.channels = { one, two };
    return p;
}

const std::pair<const char*, Project (*)()> references[] {
    { "mixer", mixerProject },
    { "pattern", patternProject },
    { "routing", routingProject },
};

//==============================================================================
/** Renders the project offline (deterministic, faster than real time) and returns the channel levels in
    dBFS per 50 ms window. */
std::vector<std::vector<float>> renderFingerprint (const Project& project)
{
    EngineHost host ({ .openAudioDevice = false });
    test::TestDevices devices (host.getEngine());
    auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
    engine::EngineAdapter adapter (*edit, devices);
    adapter.sync (project);

    const auto file = juce::File::createTempFile (".wav");
    juce::WavAudioFormat wav;
    te::Renderer::Parameters params (*edit);
    params.destFile = file;
    params.tracksToDo = te::toBitSet (te::getAllTracks (*edit)); // empty would render nothing
    params.audioFormat = &wav;
    params.bitDepth = 32;
    params.sampleRateForAudio = sampleRate;
    params.blockSizeForAudio = 512;
    params.time = { te::TimePosition(), te::TimePosition::fromSeconds (renderSeconds) };
    params.usePlugins = true;
    params.useMasterPlugins = true;
    params.shouldNormalise = false;
    params.ditheringEnabled = false;

    const auto rendered = te::Renderer::renderToFile ("golden", params);
    REQUIRE (rendered.existsAsFile());

    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (rendered), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    reader.reset();
    rendered.deleteFile();

    std::vector<std::vector<float>> levels ((size_t)audio.getNumChannels());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int start = 0; start + windowSamples <= audio.getNumSamples(); start += windowSamples)
            levels[(size_t)ch].push_back (juce::jmax (
                floorDb, juce::Decibels::gainToDecibels (audio.getRMSLevel (ch, start, windowSamples), floorDb)));

    return levels;
}

juce::var toVar (const std::vector<std::vector<float>>& levels)
{
    juce::Array<juce::var> channels;
    for (auto& ch : levels)
    {
        juce::Array<juce::var> values;
        for (auto v : ch)
            values.add (std::round ((double)v * 10.0) / 10.0);
        channels.add (values);
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("windowMs", 50);
    o->setProperty ("levelsDb", channels);
    return juce::var (o);
}

void writeText (const juce::File& file, const juce::String& text)
{
    REQUIRE (file.replaceWithText (text.replace ("\r\n", "\n") + "\n", false, false, "\n"));
}

} // namespace

//==============================================================================
TEST_CASE ("Reference projects still sound the same", "[golden]")
{
    for (auto& [name, build] : references)
    {
        INFO ("reference project: " << name);
        const auto projectFile = goldenDir().getChildFile (juce::String (name) + ".json");
        const auto goldenFile = goldenDir().getChildFile (juce::String (name) + ".golden.json");

        if (updating())
            writeText (projectFile, toJsonText (build()));

        Project project;
        const auto loaded = fromJsonText (projectFile.loadFileAsString(), project);
        INFO (loaded.getErrorMessage());
        REQUIRE (loaded.wasOk());

        const auto levels = renderFingerprint (project);

        if (updating())
            writeText (goldenFile, juce::JSON::toString (toVar (levels), juce::JSON::FormatOptions().withSpacing (
                                                                             juce::JSON::Spacing::singleLine)));

        const auto golden = juce::JSON::parse (goldenFile.loadFileAsString());
        auto* channels = golden["levelsDb"].getArray();
        REQUIRE (channels != nullptr);
        REQUIRE (channels->size() == (int)levels.size());

        int mismatches = 0;
        juce::String first;
        for (size_t ch = 0; ch < levels.size(); ++ch)
        {
            auto* expected = (*channels)[(int)ch].getArray();
            REQUIRE (expected != nullptr);
            REQUIRE (expected->size() == (int)levels[ch].size());

            for (size_t w = 0; w < levels[ch].size(); ++w)
            {
                const auto want = (float)(double)(*expected)[(int)w];
                const auto got = levels[ch][w];
                // Loud windows must match within 1 dB; quiet ones must both stay quiet.
                const bool ok = want > -60.0f ? std::abs (got - want) <= 1.0f : got <= -50.0f;
                if (! ok && mismatches++ == 0)
                    first = "channel " + juce::String ((int)ch) + " at " + juce::String ((int)w * 50) +
                            " ms: expected " + juce::String (want, 1) + " dB, got " + juce::String (got, 1) + " dB";
            }
        }

        INFO ("first difference: " << first);
        CHECK (mismatches == 0);
    }
}

TEST_CASE ("Offline rendering is repeatable", "[golden]")
{
    // Tracktion's synth starts voices with random oscillator phases, so renders are not bit-exact; the
    // levels must still agree within the golden tolerance (1 dB) or the golden test would be flaky.
    const auto project = patternProject();
    const auto a = renderFingerprint (project);
    const auto b = renderFingerprint (project);
    REQUIRE (a.size() == b.size());

    float worst = 0.0f;
    for (size_t ch = 0; ch < a.size(); ++ch)
        for (size_t w = 0; w < a[ch].size(); ++w)
            worst = std::max (worst, std::abs (a[ch][w] - b[ch][w]));

    CHECK (worst < 1.0f);
}
