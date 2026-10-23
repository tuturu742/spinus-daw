#include <catch2/catch_test_macros.hpp>

#include "dsp/sampler/SamplerProcessor.h"
#include "RealtimeRunner.h"
#include "AudioChecks.h"
#include "engine/EngineHost.h"
#include "engine/SamplePreview.h"
#include "ui/Browser.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
juce::File writeTone (const juce::File& file, double seconds, double rate = 22050.0)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.5f * (float)std::sin (2.0 * M_PI * 440.0 * i / rate));
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

struct Temp
{
    Temp() { root.createDirectory(); }
    ~Temp() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-samples") + "-" + juce::Uuid().toString());
};

/** Renders the preview in blocks until it stops; returns how many samples were not silent and the peak. */
std::pair<int, float> renderAll (engine::SamplePreview& preview, int maxBlocks = 4000)
{
    juce::AudioBuffer<float> block (2, 512);
    int heard = 0;
    float peak = 0.0f;
    for (int i = 0; i < maxBlocks && (i == 0 || preview.isPlaying()); ++i)
    {
        block.clear();
        preview.mix (block.getArrayOfWritePointers(), 2, 512);
        for (int s = 0; s < 512; ++s)
            if (std::abs (block.getSample (0, s)) > 1.0e-4f)
                ++heard;
        peak = juce::jmax (peak, block.getMagnitude (0, 512));
    }
    return { heard, peak };
}
} // namespace

TEST_CASE ("The sample preview plays a file once, at the device's rate, and stops", "[samples]")
{
    Temp t;
    engine::SamplePreview preview;
    preview.setSampleRate (48000.0);
    auto& formats = dsp::SamplerProcessor::getFormats();

    REQUIRE (preview.play (writeTone (t.root.getChildFile ("Hat.wav"), 0.5), formats));
    CHECK (preview.isPlaying());
    const auto [heard, peak] = renderAll (preview);
    CHECK (! preview.isPlaying());
    CHECK (std::abs (heard - 24000) < 600); // half a second at 48 kHz (zero crossings aside)
    CHECK (std::abs (peak - 0.5f * engine::SamplePreview::gain) < 0.02f);

    // Stop silences it; a new file replaces the one playing.
    REQUIRE (preview.play (t.root.getChildFile ("Hat.wav"), formats));
    preview.stop();
    CHECK (renderAll (preview).first == 0);

    // Long files preview their start only; unreadable ones are refused.
    REQUIRE (preview.play (
        writeTone (t.root.getChildFile ("Song.wav"), engine::SamplePreview::maxSeconds + 5.0, 8000.0), formats));
    CHECK (renderAll (preview, 10000).first <= (int)(engine::SamplePreview::maxSeconds * 48000.0));
    t.root.getChildFile ("Broken.wav").replaceWithText ("not audio");
    CHECK (! preview.play (t.root.getChildFile ("Broken.wav"), formats));
}

TEST_CASE ("Sample folders: added, listed, searched, remembered and removed", "[ui][samples]")
{
    Temp t;
    const auto library = t.root.getChildFile ("My Samples");
    writeTone (library.getChildFile ("Drums/Kicks/Kick 02.wav"), 0.1);
    writeTone (library.getChildFile ("Drums/Kicks/Kick 10.wav"), 0.1);
    writeTone (library.getChildFile ("Drums/Snare.wav"), 0.1);
    library.getChildFile ("Drums/readme.txt").replaceWithText ("not a sample");
    library.getChildFile ("Drums/.hidden.wav").replaceWithText ("hidden");
    library.getChildFile ("Empty").createDirectory();

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    Browser browser (session);
    browser.setSize (300, 600);
    const auto settings = t.root.getChildFile ("settings/sample-folders.txt");
    browser.setSampleFoldersFile (settings);

    auto texts = [&]
    {
        juce::StringArray result;
        bool inSamples = false;
        for (auto& r : browser.getRows())
        {
            if (r.kind == Browser::RowKind::section)
                inSamples = r.key == "samples";
            else if (inSamples)
                result.add (juce::String (r.depth) + ":" + r.text);
        }
        return result;
    };
    CHECK (texts() == juce::StringArray { "0:+ Add a folder of samples..." });

    // Added: open, its subfolders first, then its audio files; text files and hidden files are left out.
    REQUIRE (browser.addSampleFolder (library));
    CHECK (! browser.addSampleFolder (library)); // already there
    CHECK (texts() == juce::StringArray { "0:My Samples", "1:Drums", "1:Empty", "0:+ Add a folder of samples..." });
    browser.toggleFolder (library.getChildFile ("Drums"));
    browser.toggleFolder (library.getChildFile ("Drums/Kicks"));
    CHECK (texts() == juce::StringArray { "0:My Samples", "1:Drums", "2:Kicks", "3:Kick 02", "3:Kick 10", "2:Snare",
                                          "1:Empty", "0:+ Add a folder of samples..." });

    // Searching finds files anywhere inside, with their folder's name.
    browser.setSearchText ("kick");
    CHECK (texts() == juce::StringArray { "0:Kick 02", "0:Kick 10" });
    const auto& rows = browser.getRows();
    const auto kick = std::find_if (rows.begin(), rows.end(), [] (auto& r) { return r.text == "Kick 02"; });
    REQUIRE (kick != rows.end());
    CHECK (kick->note == "Kicks");

    // Dragging carries the file; double-clicking uses it.
    juce::File used;
    browser.onUseSample = [&] (const juce::File& f) { used = f; };
    browser.useRow ((int)(kick - rows.begin()));
    CHECK (used == library.getChildFile ("Drums/Kicks/Kick 02.wav"));
    juce::var drag (new juce::DynamicObject());
    drag.getDynamicObject()->setProperty ("sampleFile", used.getFullPathName());
    CHECK (Browser::sampleFromDrag (drag) == used);
    CHECK (! Browser::sampleFromDrag (juce::var ("Surge XT")).has_value());
    browser.setSearchText ({});

    // Remembered in the settings file, and removed (the files stay).
    Browser again (session);
    again.setSampleFoldersFile (settings);
    CHECK (again.getSampleFolders() == juce::Array<juce::File> { library });
    browser.removeSampleFolder (library);
    CHECK (texts() == juce::StringArray { "0:+ Add a folder of samples..." });
    CHECK (library.getChildFile ("Drums/Snare.wav").existsAsFile());
    again.setSampleFoldersFile (settings);
    CHECK (again.getSampleFolders().isEmpty());
}

TEST_CASE ("A preview is heard on the engine's output, after the song", "[samples][audio]")
{
    Temp t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 2.0, .paceInRealTime = true });
    runner.start();
    runner.runUntil (0.2);
    const auto start = runner.getSecondsProcessed();
    REQUIRE (session.getSamplePreview().play (writeTone (t.root.getChildFile ("Clap.wav"), 0.3),
                                              dsp::SamplerProcessor::getFormats()));
    runner.runUntil (start + 0.8);
    runner.stop();

    const auto sr = runner.getParams().sampleRate;
    auto level = [&] (double from, double to)
    { return test::checks::rms (runner.getChannel (0), (int64_t)((start + from) * sr), (int64_t)((start + to) * sr)); };
    CHECK (level (0.0, 0.3) > 0.05f);  // the sample
    CHECK (level (0.5, 0.7) < 0.001f); // then silence
    CHECK (! session.getSamplePreview().isPlaying());
}
