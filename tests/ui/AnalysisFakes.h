#pragma once

#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <tuple>

/** Stand-ins for the Analyzer's models and a test song, shared by the Analyzer and analysis acceptance tests. */
namespace spinus::test::analysis
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-analyzer-" + juce::Uuid().toString());
};

/** The analysis engine with stand-ins for PyTorch, Demucs and Beat This! (ai/tests/fakes): the real AI
    service and worker run, the models give simple answers. */
struct FakeAnalysis
{
    FakeAnalysis()
    {
        const auto source = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory();
        const auto fakes = source.getChildFile ("ai/tests/fakes");
        oldPath = juce::SystemStats::getEnvironmentVariable ("PATH", {});
        ::setenv ("PATH", (fakes.getFullPathName() + ":" + oldPath).toRawUTF8(), 1);
        ::setenv ("FAKE_TORCH", fakes.getFullPathName().toRawUTF8(), 1);
        ::setenv ("REAL_PYTHON", source.getChildFile ("ai/.venv/bin/python").getFullPathName().toRawUTF8(), 1);
        ::setenv ("FAKE_GPU", "none", 1);
    }
    ~FakeAnalysis()
    {
        ::setenv ("PATH", oldPath.toRawUTF8(), 1);
        for (auto* name : { "FAKE_TORCH", "REAL_PYTHON", "FAKE_GPU" })
            ::unsetenv (name);
    }
    juce::String oldPath;
};

inline juce::File bundledPlugins()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR);
#else
    return {};
#endif
}

/** Spinus's own instruments, as in the app (the song's own sounds are loaded into them). */
inline std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
    return devices;
}

/** Model files of the right size where the service looks for them, as if downloaded. */
inline void placeModel (const juce::File& models, const juce::String& id,
                        std::initializer_list<std::pair<const char*, juce::int64>> files, const juce::String& yaml = {})
{
    const auto folder = models.getChildFile ("analysis").getChildFile (id);
    folder.createDirectory();
    for (auto [name, size] : files)
    {
        const auto file = folder.getChildFile (name);
        if (file.hasFileExtension ("yaml"))
            file.replaceWithText (yaml, false, false, "\n");
        else
        {
            file.create();
            std::filesystem::resize_file (file.getFullPathName().toStdString(), (std::uintmax_t)size);
        }
    }
}

/** Basic Pitch's model: the real one when SPINUS_TEST_BASIC_PITCH names it (it is not in the repository),
    otherwise a file of the right size that only counts as downloaded. */
inline bool placeBasicPitch (const juce::File& models)
{
    const auto folder = models.getChildFile ("analysis").getChildFile ("basic_pitch");
    folder.createDirectory();
    const juce::File real (juce::SystemStats::getEnvironmentVariable ("SPINUS_TEST_BASIC_PITCH", {}));
    if (real.existsAsFile())
        return real.copyFileTo (folder.getChildFile ("nmp.onnx"));
    placeModel (models, "basic_pitch", { { "nmp.onnx", 230444 } });
    return false;
}

/** A 6-second stereo song: a click every half second over an A minor chord. */
inline juce::File makeSong (const juce::File& folder, const juce::String& name = "Test Song")
{
    const auto file = folder.getChildFile (name + ".wav");
    const double rate = 44100.0;
    juce::AudioBuffer<float> audio (2, (int)(6.0 * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto t = (double)i / rate;
        float v = 0.0f;
        for (auto f : { 220.0, 261.63, 329.63 })
            v += 0.1f * (float)std::sin (juce::MathConstants<double>::twoPi * f * t);
        if (i % 22050 < 300)
            v += 0.5f;
        audio.setSample (0, i, v);
        audio.setSample (1, i, v);
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (16));
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}
/** A 6-second stereo 32-bit float stem: a tone at `frequency` (0: none), with a click every half second when
    `clicks`. */
inline juce::File makeStem (const juce::File& file, double frequency, bool clicks)
{
    const double rate = 44100.0;
    juce::AudioBuffer<float> audio (2, (int)(6.0 * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto t = (double)i / rate;
        auto v = frequency > 0.0 ? 0.2f * (float)std::sin (juce::MathConstants<double>::twoPi * frequency * t) : 0.0f;
        if (clicks && i % 22050 < 300)
            v += 0.6f * (float)std::exp (-(double)(i % 22050) / 60.0);
        audio.setSample (0, i, v);
        audio.setSample (1, i, v);
    }
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions()
                    .withSampleRate (rate)
                    .withNumChannels (2)
                    .withBitsPerSample (32)
                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

/** The first 20 hex digits of a file's SHA-256: how the AI service names what it keeps of it. */
inline juce::String digestOf (const juce::File& file)
{
    juce::FileInputStream in (file);
    return juce::SHA256 (in).toHexString().substring (0, 20);
}

/** As if the AI service had separated `prepared` with `model` (at its `revision`) before: it then answers
    from its cache (cacheFolder/stems), so a model that needs a real PyTorch (MVSep's BS-RoFormer) is not run. */
inline void placeSeparation (const juce::File& cacheFolder, const juce::File& prepared, const juce::String& model,
                             const juce::String& revision, const std::map<juce::String, juce::File>& stems)
{
    const auto folder = cacheFolder.getChildFile ("stems").getChildFile (digestOf (prepared) + "-" + model + "-" +
                                                                         revision.substring (0, 8));
    folder.createDirectory();
    auto* list = new juce::DynamicObject();
    for (auto& [name, file] : stems)
        list->setProperty (name, file.getFullPathName());
    folder.getChildFile ("stems.json").replaceWithText (juce::JSON::toString (juce::var (list)));
}

/** As if Basic Pitch had found these notes in a stem before (cacheFolder/analysis): pitch, start and end in
    seconds. */
inline void placeNotes (const juce::File& cacheFolder, const juce::File& stem,
                        std::initializer_list<std::tuple<int, double, double>> notes)
{
    juce::Array<juce::var> list;
    for (auto [pitch, start, end] : notes)
    {
        auto* n = new juce::DynamicObject();
        n->setProperty ("start", start);
        n->setProperty ("end", end);
        n->setProperty ("pitch", pitch);
        n->setProperty ("velocity", 0.8);
        list.add (juce::var (n));
    }
    const auto file = cacheFolder.getChildFile ("analysis").getChildFile (digestOf (stem) + "-basic_pitch.json");
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (list)));
}

/** The MVSep Mega 53 model's files, of the right size (as if downloaded). */
inline void placeManyInstruments (const juce::File& models)
{
    placeModel (models, "mvsep_mega_53", { { "model.ckpt", 1368919887 } });
    const auto config = models.getChildFile ("analysis/mvsep_mega_53/config.yaml");
    config.create();
    std::filesystem::resize_file (config.getFullPathName().toStdString(), 4184);
}
} // namespace spinus::test::analysis
