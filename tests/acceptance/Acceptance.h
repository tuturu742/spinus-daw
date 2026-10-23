#pragma once

#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "plugins/DeviceList.h"
#include "session/ProjectDocument.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"
#include "ui/PianoRoll.h"
#include "ui/Playlist.h"

/** What the acceptance tests share: a session with every panel, test sounds, exports and comparing
    renders. */

#if defined(SPINUS_PLUGINS_DIR)

namespace spinus::acceptance
{
constexpr double rate = 48000.0;
constexpr model::Ticks bar = 4 * model::ticksPerQuarterNote;

inline juce::File writeTone (const juce::File& file, double seconds, double frequency)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.3f * (float)std::sin (2.0 * M_PI * frequency * i / rate));
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

/** The first plugin in a VST3 or LV2 bundle, as a Spinus device (described by the engine's own format,
    as after a scan). */
inline model::Device loadPlugin (const juce::String& formatName, const juce::String& fileOrIdentifier,
                                 tracktion::Engine& engine)
{
    juce::AudioPluginFormat* format = nullptr;
    for (auto* f : engine.getPluginManager().pluginFormatManager.getFormats())
        if (f->getName() == formatName)
            format = f;
    REQUIRE (format != nullptr);
    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile (found, fileOrIdentifier);
    REQUIRE (! found.isEmpty());
    engine.getPluginManager().knownPluginList.addType (*found[0]);
    return plugins::DeviceList::makeDevice (*found[0]);
}

struct Render
{
    juce::AudioBuffer<float> audio;
    double seconds = 0.0;
};

/** How different two renders are: the RMS of their difference over the RMS of the first. */
inline float difference (const Render& a, const Render& b)
{
    REQUIRE (a.audio.getNumSamples() == b.audio.getNumSamples());
    double diff = 0.0, level = 0.0;
    for (int ch = 0; ch < a.audio.getNumChannels(); ++ch)
        for (int i = 0; i < a.audio.getNumSamples(); ++i)
        {
            const double x = a.audio.getSample (ch, i), y = b.audio.getSample (ch, i);
            diff += (x - y) * (x - y);
            level += x * x;
        }
    return level > 0.0 ? (float)std::sqrt (diff / level) : 1.0f;
}

inline Render readWav (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    Render r;
    r.audio.setSize ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&r.audio, 0, (int)reader->lengthInSamples, 0, true, true);
    r.seconds = (double)reader->lengthInSamples / reader->sampleRate;
    return r;
}

inline float rmsOf (const Render& r, double from, double to)
{
    const auto start = (int)(from * rate), end = juce::jmin (r.audio.getNumSamples(), (int)(to * rate));
    double sum = 0.0;
    for (int i = start; i < end; ++i)
        sum += (double)r.audio.getSample (0, i) * r.audio.getSample (0, i);
    return end > start ? (float)std::sqrt (sum / (end - start)) : 0.0f;
}
struct Fixture
{
    explicit Fixture (const juce::String& folderName)
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::String (folderName) + "-" + juce::Uuid().toString()))
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, host.getEngine(), { juce::File (SPINUS_PLUGINS_DIR) });
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        document = std::make_unique<ProjectDocument> (*session, root.getChildFile ("settings"));
        rack = std::make_unique<ui::ChannelRack> (*session);
        roll = std::make_unique<ui::PianoRoll> (*session);
        playlist = std::make_unique<ui::Playlist> (*session);
        mixer = std::make_unique<ui::Mixer> (*session);
        playlist->setSize (900, 400);
    }

    ~Fixture()
    {
        mixer = nullptr;
        playlist = nullptr;
        roll = nullptr;
        rack = nullptr;
        document = nullptr;
        session = nullptr;
        root.deleteRecursively();
    }

    const model::Channel& channel (const juce::String& name) const
    {
        for (auto& c : session->getProject().channels)
            if (c.name == name)
                return c;
        FAIL ("no channel " << name);
        return session->getProject().channels.front();
    }

    /** Exports the song (and stems) and reads the mix back. */
    Render exportSong (const juce::String& name, bool stems = false)
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav32Float;
        settings.stems = stems;
        std::optional<engine::ExportJob::Result> result;
        const auto file = root.getChildFile ("exports").getChildFile (name + ".wav");
        auto job = session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        for (int i = 0; i < 1200 && ! result; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        REQUIRE (result.has_value());
        const auto done = result.value_or (engine::ExportJob::Result {});
        INFO (done.error);
        REQUIRE (done.ok);
        lastFiles = done.files;

        return readWav (file);
    }

    juce::File root;
    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ProjectDocument> document;
    std::unique_ptr<ui::ChannelRack> rack;
    std::unique_ptr<ui::PianoRoll> roll;
    std::unique_ptr<ui::Playlist> playlist;
    std::unique_ptr<ui::Mixer> mixer;
    std::vector<juce::File> lastFiles;
};

} // namespace spinus::acceptance

#endif
