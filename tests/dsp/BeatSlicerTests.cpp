#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "dsp/slicer/BeatSlicerEditor.h"
#include "dsp/slicer/BeatSlicerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/SlicesToPattern.h"
#include "ui/ChannelRack.h"

using namespace spinus;
using namespace spinus::dsp;
namespace te = tracktion;

namespace
{
constexpr double rate = 48000.0;

/** A drum-loop stand-in: decaying noise bursts at the given times (seconds), each `level` loud. */
juce::AudioBuffer<float> hits (double seconds, const std::vector<std::pair<double, float>>& at)
{
    juce::AudioBuffer<float> b (2, (int)(seconds * rate));
    b.clear();
    juce::Random random (11);
    for (auto [time, level] : at)
        for (int i = 0; i < (int)(0.12 * rate); ++i)
        {
            const auto n = (int)(time * rate) + i;
            if (n >= b.getNumSamples())
                break;
            const auto x = level * std::exp (-(float)i / (float)(0.025 * rate)) * (random.nextFloat() * 2.0f - 1.0f);
            b.setSample (0, n, x);
            b.setSample (1, n, x);
        }
    return b;
}

std::vector<double> inSeconds (const std::vector<juce::int64>& points)
{
    std::vector<double> s;
    s.reserve (points.size());
    for (auto p : points)
        s.push_back ((double)p / rate);
    return s;
}

/** Renders `seconds` of the slicer with these MIDI events at sample 0; returns the left channel. */
std::vector<float> play (BeatSlicerProcessor& slicer, const std::vector<juce::MidiMessage>& events, double seconds)
{
    slicer.prepareToPlay (rate, 256);
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    for (auto& e : events)
        midi.addEvent (e, 0);
    for (int done = 0; done < (int)(seconds * rate); done += 256)
    {
        slicer.processBlock (buffer, midi);
        midi.clear();
        for (int i = 0; i < 256; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    return out;
}
} // namespace

TEST_CASE ("Slicing at hits finds each hit, at a zero crossing just before it", "[dsp][slicer]")
{
    const std::vector<double> times { 0.0, 0.25, 0.5, 0.875, 1.0, 1.3, 1.5, 1.75 }; // not all on a grid
    std::vector<std::pair<double, float>> loud;
    loud.reserve (times.size());
    for (auto t : times)
        loud.emplace_back (t, 0.8f);
    const auto audio = hits (2.0, loud);

    const auto found = inSeconds (slicing::byTransients (audio, rate, 0.5f));
    REQUIRE (found.size() == times.size());
    for (size_t i = 0; i < times.size(); ++i)
    {
        INFO ("hit " << i << " at " << times[i] << " found at " << found[i]);
        CHECK (found[i] <= times[i] + 0.0005);
        CHECK (found[i] > times[i] - 0.0015);
    }

    // Quiet hits between loud ones: only a high sensitivity finds them.
    auto mixed = loud;
    for (auto t : { 0.125, 0.625, 1.125, 1.625 })
        mixed.emplace_back (t, 0.05f);
    const auto mixedAudio = hits (2.0, mixed);
    CHECK (slicing::byTransients (mixedAudio, rate, 0.1f).size() == times.size());
    CHECK (slicing::byTransients (mixedAudio, rate, 1.0f).size() == times.size() + 4);
}

TEST_CASE ("Slicing by grid, tidying and guessing the loop's length", "[dsp][slicer]")
{
    const auto grid = slicing::byGrid (96000, 8.0, 0.25); // 2 bars in 1/16 notes
    REQUIRE (grid.size() == 32);
    CHECK (grid[1] == 3000);
    CHECK (grid[31] == 93000);

    CHECK (slicing::tidy ({ 500, 0, 500, 120000, -3, 200 }, 1000) == std::vector<juce::int64> { 0, 200, 500 });
    CHECK (slicing::byGrid (1000, 128.0, 0.125).size() == (size_t)slicing::maxSlices);

    CHECK (slicing::estimateBeats (3.53, 120.0, "Amen break 136bpm") == Catch::Approx (8.0));
    CHECK (slicing::estimateBeats (3.53, 120.0, "Amen_136_BPM") == Catch::Approx (8.0));
    CHECK (slicing::estimateBeats (2.0, 120.0, "Loop") == Catch::Approx (4.0));
    CHECK (slicing::estimateBeats (4.1, 120.0, "Loop") == Catch::Approx (8.0));
    CHECK (slicing::estimateBeats (1.9, 70.0, "Loop") == Catch::Approx (2.0)); // 63 BPM is closer to 70 than 126
}

TEST_CASE ("Each slice plays from its own key, at its own pitch", "[dsp][slicer]")
{
    BeatSlicerProcessor slicer;
    // Four quarter notes at 120 BPM; hit 3 is much louder, so its key is easy to tell.
    slicer.setLoop (hits (2.0, { { 0.0, 0.2f }, { 0.5, 0.2f }, { 1.0, 0.9f }, { 1.5, 0.2f } }), rate, "Loop");
    CHECK (slicer.getLoopBeats() == Catch::Approx (4.0));
    REQUIRE (slicer.getSlicePoints().size() == 4);

    auto peakOf = [] (const std::vector<float>& x)
    {
        float p = 0.0f;
        for (auto v : x)
            p = juce::jmax (p, std::abs (v));
        return p;
    };
    const auto third = peakOf (play (slicer, { juce::MidiMessage::noteOn (1, 62, (juce::uint8)127) }, 0.4));
    const auto first = peakOf (play (slicer, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) }, 0.4));
    CHECK (third > 3.0f * first);
    CHECK (peakOf (play (slicer, { juce::MidiMessage::noteOn (1, 64, (juce::uint8)127) }, 0.4)) < 1.0e-6f);

    // A slice stops at its end (no sound from the next slice), fading out over a few milliseconds.
    const auto out = play (slicer, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) }, 0.7);
    CHECK (test::checks::rms (out, (int64_t)(0.505 * rate), (int64_t)(0.7 * rate)) < 1.0e-6f);
}

TEST_CASE ("Each slice can play from a key of its own choosing; keys follow added and removed slices", "[dsp][slicer]")
{
    BeatSlicerProcessor slicer;
    slicer.setLoop (hits (2.0, { { 0.0, 0.2f }, { 0.5, 0.2f }, { 1.0, 0.9f }, { 1.5, 0.2f } }), rate, "Loop");
    REQUIRE (slicer.getSliceKeys() == std::vector<int> { 60, 61, 62, 63 });
    CHECK (BeatSlicerProcessor::keyName (60) == "C5");
    CHECK (BeatSlicerProcessor::keyName (66) == "F#5");

    auto peakOf = [] (const std::vector<float>& x)
    {
        float p = 0.0f;
        for (auto v : x)
            p = juce::jmax (p, std::abs (v));
        return p;
    };

    // The loud third slice on C3: it plays from there, and not from D5.
    slicer.setSliceKey (2, 48);
    CHECK (slicer.getSliceKeys() == std::vector<int> { 60, 61, 48, 63 });
    const auto moved = peakOf (play (slicer, { juce::MidiMessage::noteOn (1, 48, (juce::uint8)127) }, 0.4));
    const auto quiet = peakOf (play (slicer, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) }, 0.4));
    CHECK (moved > 3.0f * quiet);
    CHECK (peakOf (play (slicer, { juce::MidiMessage::noteOn (1, 62, (juce::uint8)127) }, 0.4)) < 1.0e-6f);

    // One slice a key: taking a slice's key swaps the two.
    slicer.setSliceKey (0, 63);
    CHECK (slicer.getSliceKeys() == std::vector<int> { 63, 61, 48, 60 });
    CHECK (slicer.getSliceNotes()[2].note == 48); // Slices to pattern uses the chosen keys

    // A new slice takes the lowest free key from C5; removing one drops its key; the state keeps them.
    slicer.addSlicePoint (12000);
    CHECK (slicer.getSliceKeys() == std::vector<int> { 63, 62, 61, 48, 60 });
    slicer.removeSlicePoint (2);
    CHECK (slicer.getSliceKeys() == std::vector<int> { 63, 62, 48, 60 });
    juce::MemoryBlock state;
    slicer.getStateInformation (state);
    BeatSlicerProcessor copy;
    copy.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (copy.getSliceKeys() == slicer.getSliceKeys());

    // Reset, or slicing again: C5 upwards.
    copy.resetSliceKeys();
    CHECK (copy.getSliceKeys() == std::vector<int> { 60, 61, 62, 63 });
    slicer.slice (BeatSlicerProcessor::Method::grid, 0.5f, 1.0);
    CHECK (slicer.getSliceKeys() == std::vector<int> { 60, 61, 62, 63 });
}

TEST_CASE ("Slices can be added, moved and removed; the state keeps the loop and its slices", "[dsp][slicer]")
{
    BeatSlicerProcessor slicer;
    slicer.setLoop (hits (2.0, { { 0.0, 0.8f }, { 1.0, 0.8f } }), rate, "Two hits 120bpm");
    REQUIRE (slicer.getSlicePoints().size() == 2);

    slicer.addSlicePoint (24000);
    REQUIRE (slicer.getSlicePoints().size() == 3);
    slicer.moveSlicePoint (1, 30000);
    CHECK (slicer.getSlicePoints()[1] == 30000);
    slicer.moveSlicePoint (1, 90000); // cannot pass the next slice
    CHECK (slicer.getSlicePoints()[1] < slicer.getSlicePoints()[2]);
    slicer.moveSlicePoint (0, 500); // the first slice always starts at 0
    CHECK (slicer.getSlicePoints()[0] == 0);
    slicer.removeSlicePoint (1);
    CHECK (slicer.getSlicePoints().size() == 2);

    slicer.slice (BeatSlicerProcessor::Method::grid, 0.5f, 0.5);
    CHECK (slicer.getSlicePoints().size() == 8); // 4 beats in eighth notes
    slicer.setLoopBeats (8.0);
    CHECK (slicer.getSlicePoints().size() == 16);

    juce::MemoryBlock state;
    slicer.getStateInformation (state);
    BeatSlicerProcessor copy;
    copy.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (copy.getSlicePoints() == slicer.getSlicePoints());
    CHECK (copy.getLoopBeats() == Catch::Approx (8.0));
    CHECK (copy.getMethod() == BeatSlicerProcessor::Method::grid);
    CHECK (copy.getLoop().getLength() == slicer.getLoop().getLength());
    CHECK (copy.getLoop().name == "Two hits 120bpm");

    const auto notes = copy.getSliceNotes();
    REQUIRE (notes.size() == 16);
    CHECK (notes[3].note == 63);
    CHECK (notes[3].startBeats == Catch::Approx (1.5));
    CHECK (notes[3].lengthBeats == Catch::Approx (0.5));
}

namespace
{
struct SlicerSession
{
    SlicerSession() : session (host.getEngine(), makeDevices (host.getEngine())), rack (session)
    {
        REQUIRE (rack.addChannel ("spinus.beat-slicer", "Loop"));
        channel = session.getProject().channels[0].id;
        const auto* c = session.getProject().findChannel (channel);
        REQUIRE ((c != nullptr && c->instrument.has_value()));
        const auto device = c->instrument.value_or (model::Device {}).id;
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
        REQUIRE (plugin != nullptr);
        slicer = dynamic_cast<BeatSlicerProcessor*> (plugin->getAudioPluginInstance());
        REQUIRE (slicer != nullptr);
    }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (te::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, {});
        return devices;
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session;
    ui::ChannelRack rack;
    model::ChannelId channel;
    BeatSlicerProcessor* slicer = nullptr;
};
} // namespace

TEST_CASE ("Slices to pattern writes the loop as notes, in one undo step", "[dsp][slicer][ui]")
{
    SlicerSession s;
    CHECK (! slicesToPattern (s.session, s.channel).wasOk()); // no loop yet
    CHECK (! hasSlices (s.session, s.channel));

    // Two bars at 120 BPM, hits on beats 1, 2, 3.5 and 5.
    s.slicer->setLoop (hits (4.0, { { 0.0, 0.8f }, { 0.5, 0.8f }, { 1.25, 0.8f }, { 2.0, 0.8f } }), rate, "Break");
    CHECK (s.slicer->getLoopBeats() == Catch::Approx (8.0));
    CHECK (hasSlices (s.session, s.channel));

    // No pattern yet: a new two-bar pattern, named after the loop, becomes the current one.
    const auto before = s.session.getProject().patterns.size();
    REQUIRE (slicesToPattern (s.session, s.channel).wasOk());
    REQUIRE (s.session.getProject().patterns.size() == before + 1);
    const auto* pattern = s.session.getProject().findPattern (s.session.getCurrentPattern());
    REQUIRE (pattern != nullptr);
    CHECK (pattern->name == "Break");
    CHECK (pattern->length == 2 * 3840);
    const auto& notes = pattern->notes.at (s.channel);
    REQUIRE (notes.size() == 4);
    CHECK (notes[0].pitch == 60);
    CHECK (notes[2].pitch == 62);
    CHECK (std::abs (notes[1].start - 960) <= 4);
    CHECK (std::abs (notes[2].start - 2400) <= 4);
    CHECK (std::abs (notes[3].start - 3840) <= 4);
    CHECK (s.session.getHistory().getUndoDescriptions().joinIntoString (" | ").endsWith ("Slices to pattern"));

    // Undo takes the pattern away as well.
    s.session.getHistory().undo();
    CHECK (s.session.getProject().patterns.size() == before);

    // A current pattern that is long enough gets the notes instead.
    s.session.getHistory().redo();
    s.slicer->slice (BeatSlicerProcessor::Method::grid, 0.5f, 1.0);
    REQUIRE (slicesToPattern (s.session, s.channel).wasOk());
    CHECK (s.session.getProject().findPattern (s.session.getCurrentPattern())->notes.at (s.channel).size() == 8);
    CHECK (s.session.getProject().patterns.size() == before + 1);
}

TEST_CASE ("A sliced loop plays back from its pattern", "[dsp][slicer][audio]")
{
    SlicerSession s;
    test::RealtimeRunner runner (s.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = false });
    s.slicer->setLoop (hits (2.0, { { 0.0, 0.8f }, { 0.5, 0.8f }, { 1.0, 0.8f }, { 1.5, 0.8f } }), rate, "Beat");
    REQUIRE (slicesToPattern (s.session, s.channel).wasOk());

    s.session.play();
    runner.start();
    runner.runUntil (2.0);
    runner.stop();
    s.session.stop();

    // Each beat starts with a hit.
    const auto& left = runner.getChannel (0);
    for (double beat : { 0.0, 0.5, 1.0, 1.5 })
    {
        INFO ("beat at " << beat);
        CHECK (test::checks::rms (left, (int64_t)((beat + 0.005) * rate), (int64_t)((beat + 0.05) * rate)) > 0.05f);
    }
}

TEST_CASE ("The Beat Slicer's window", "[dsp][slicer][ui]")
{
    BeatSlicerProcessor slicer;
    std::unique_ptr<juce::AudioProcessorEditor> editor (slicer.createEditorAndMakeActive());
    auto* window = dynamic_cast<BeatSlicerEditor*> (editor.get());
    REQUIRE (window != nullptr);
    CHECK (window->getMessage() == "No loop yet.");
    slicer.setLoop (hits (2.0, { { 0.0, 0.8f }, { 1.0, 0.8f } }), rate, "Beat");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    CHECK (window->getMessage().startsWith ("Beat: 2 slices"));
    slicer.editorBeingDeleted (editor.get());
}

TEST_CASE ("Beat Slicer window screenshot for the manual", "[.screenshot][slicer]")
{
    BeatSlicerProcessor slicer;
    std::vector<std::pair<double, float>> pattern;
    for (int i = 0; i < 16; ++i)
        if (i % 4 == 0 || i % 4 == 3 || i == 6 || i == 10)
            pattern.emplace_back (i * 0.125 * 2.0, i % 4 == 0 ? 0.9f : 0.45f);
    slicer.setLoop (hits (4.0, pattern), rate, "Drum loop 120bpm");
    slicer.onSlicesToPattern = [] {};
    std::unique_ptr<juce::AudioProcessorEditor> editor (slicer.createEditorAndMakeActive());
    editor->setSize (760, 460);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/beat-slicer.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    slicer.editorBeingDeleted (editor.get());
}

TEST_CASE ("Open in Beat Slicer: a file, or a Sampler channel's sample, becomes a sliced Beat Slicer channel",
           "[dsp][slicer][ui]")
{
    SlicerSession s;
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-open-in-slicer-" + juce::Uuid().toString());
    const auto file = folder.getChildFile ("Break 120bpm.wav");
    {
        REQUIRE (folder.createDirectory());
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        const auto audio = hits (2.0, { { 0.0, 0.8f }, { 0.5, 0.8f }, { 1.0, 0.8f }, { 1.5, 0.8f } });
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    auto slicerOfChannel = [&] (model::ChannelId id) -> BeatSlicerProcessor*
    {
        const auto* c = s.session.getProject().findChannel (id);
        if (c == nullptr || ! c->instrument)
            return nullptr;
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (s.session.getAdapter().getDevicePlugin (c->instrument->id));
        return plugin != nullptr ? dynamic_cast<BeatSlicerProcessor*> (plugin->getAudioPluginInstance()) : nullptr;
    };

    // From a file (the Browser's and Audio tracks' menus).
    REQUIRE (s.rack.addBeatSlicerChannel (file));
    auto* fromFile = slicerOfChannel (s.session.getSelectedChannel());
    REQUIRE (fromFile != nullptr);
    CHECK (fromFile->getSlicePoints().size() == 4);
    CHECK (s.session.getProject().findChannel (s.session.getSelectedChannel())->name == "Break 120bpm");

    // From a Sampler channel: a new Beat Slicer channel; the Sampler stays.
    REQUIRE (s.rack.addSamplerChannel (file));
    const auto sampler = s.session.getSelectedChannel();
    CHECK (s.rack.hasSamplerSample (sampler));
    CHECK (! s.rack.hasSamplerSample (s.channel)); // a Beat Slicer is not a Sampler
    const auto before = s.session.getProject().channels.size();
    REQUIRE (s.rack.sliceSamplerChannel (sampler));
    CHECK (s.session.getProject().channels.size() == before + 1);
    auto* fromSampler = slicerOfChannel (s.session.getSelectedChannel());
    REQUIRE (fromSampler != nullptr);
    CHECK (fromSampler->getSlicePoints().size() == 4);
    CHECK (fromSampler->getLoop().getLength() == (juce::int64)(2.0 * rate));
    CHECK (s.session.getProject().findChannel (s.session.getSelectedChannel())->name == "Break 120bpm (sliced)");
    folder.deleteRecursively();
}
