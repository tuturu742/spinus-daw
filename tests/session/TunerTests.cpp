#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "commands/CommandRegistry.h"
#include "dsp/tune/PitchDetector.h"
#include "engine/EngineHost.h"
#include "session/Session.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace spinus;
using namespace spinus::model;
namespace te = tracktion;

namespace
{
std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

/** A 220 Hz voice-like take, 2 s, at the engine's rate. */
juce::File writeTake (const juce::File& file, double rate)
{
    std::vector<float> x ((size_t)(2.0 * rate));
    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = 0.0;
        for (int h = 1; h < 20; ++h)
            v += std::sin (2.0 * M_PI * h * 220.0 * (double)i / rate) / h;
        x[i] = (float)(0.3 * v);
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    const float* channels[] = { x.data() };
    writer->writeFromFloatArrays (channels, 1, (int)x.size());
    return file;
}

/** What ai.v1.listen.vocal would say about it: a steady A3. */
juce::var analysisOfTake()
{
    juce::Array<juce::var> times, hz, voiced;
    for (int i = 0; i < 400; ++i)
    {
        times.add (i * 0.005);
        hz.add (220.0);
        voiced.add (0.95);
    }
    auto* track = new juce::DynamicObject();
    track->setProperty ("times", times);
    track->setProperty ("hz", hz);
    track->setProperty ("voiced", voiced);
    auto* result = new juce::DynamicObject();
    result->setProperty ("track", juce::var (track));
    result->setProperty ("duration", 2.0);
    return juce::var (result);
}
} // namespace

TEST_CASE ("A tuned audio clip plays its rendering; without an analysis it plays as recorded", "[tune][session]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
    session.setMode (Session::Mode::song);
    const auto rate = host.getEngine().getDeviceManager().getSampleRate();
    const juce::TemporaryFile take (".wav");
    writeTake (take.getFile(), rate);

    auto& history = session.getHistory();
    REQUIRE (history.perform (make ("lane.add", R"({"name": "Vocal"})")).wasOk());
    const auto lane = session.getProject().playlist.lanes.back().id;
    REQUIRE (history
                 .perform (make ("clip.addAudio", "{\"lane\": " + juce::String (lane.value) + ", \"file\": " +
                                                      juce::JSON::toString (take.getFile().getFullPathName()) +
                                                      ", \"start\": 0, \"length\": 7680}"))
                 .wasOk());
    const auto clip = session.getProject().playlist.clips.back().id;
    const auto setTune = "{\"clip\": " + juce::String (clip.value) +
                         ", \"notes\": [{\"start\": 0.3, \"end\": 1.7, \"pitch\": 57, \"shift\": 2}]}";

    // Tuned, but its pitch is not known here yet: as recorded.
    REQUIRE (history.perform (make ("clip.setTune", setTune)).wasOk());
    auto& tuner = session.getTuner();
    CHECK (tuner.getStatus (clip).state == Tuner::State::needsAnalysis);
    CHECK (session.getAdapter().describe().contains ("file=" + take.getFile().getFileName()));

    // Spinus Tune's window stores the analysis: it renders, then plays the rendering.
    tuner.storeAnalysis (take.getFile(), analysisOfTake());
    for (int i = 0; i < 1000 && (tuner.isRendering() || tuner.getRenderCount() == 0); ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    REQUIRE (tuner.getRenderCount() == 1);
    CHECK (tuner.getStatus (clip).state == Tuner::State::ready);
    const auto text = session.getAdapter().describe();
    INFO (text);
    CHECK (! text.contains ("file=" + take.getFile().getFileName()));

    // The song plays B3 inside the note.
    session.getEdit().dispatchPendingUpdatesSynchronously();
    const auto out = juce::File::createTempFile (".wav");
    juce::WavAudioFormat wav;
    te::Renderer::Parameters params (session.getEdit());
    params.destFile = out;
    params.tracksToDo = te::toBitSet (te::getAllTracks (session.getEdit()));
    params.audioFormat = &wav;
    params.bitDepth = 32;
    params.sampleRateForAudio = rate;
    params.time = { te::TimePosition(), te::TimePosition::fromSeconds (1.5) };
    params.usePlugins = true;
    params.useMasterPlugins = true;
    params.checkNodesForAudio = false;
    const auto rendered = te::Renderer::renderToFile ("tuner test", params);
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (rendered), true));
    REQUIRE (reader != nullptr);
    std::vector<float> left ((size_t)reader->lengthInSamples);
    float* channels[] = { left.data() };
    reader->read (channels, 1, 0, (int)left.size());
    reader.reset();
    rendered.deleteFile();

    dsp::PitchDetector detector;
    detector.prepare (rate, 60.0f, 1200.0f);
    const auto inside = detector.analyse (left.data() + (size_t)(1.0 * rate));
    REQUIRE (inside.isVoiced());
    CHECK (std::abs (1200.0 * std::log2 (rate / inside.period / 246.94)) < 8.0);

    // Undo: as recorded again.
    history.undo();
    CHECK (session.getAdapter().describe().contains ("file=" + take.getFile().getFileName()));
    CHECK (tuner.getStatus (clip).state == Tuner::State::none);
}
