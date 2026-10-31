#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/ParameterText.h"
#include "dsp/delay/DelayEditor.h"
#include "dsp/delay/DelayProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/SongWriter.h"
#include "ui/Mixer.h"

#include <cfloat>
#include <iostream>

using namespace spinus;
using namespace spinus::dsp;
namespace te = tracktion;

namespace
{
constexpr double rate = 48000.0;

/** Echoes only, no filtering to speak of: what comes back is the sound, delayed. */
DelaySettings plain()
{
    DelaySettings s;
    s.feedback = 0.0f;
    s.lowCutHz = 20.0f;
    s.highCutHz = 20000.0f;
    s.mix = 1.0f;
    return s;
}

struct Stereo
{
    std::vector<float> left, right;
};

/** Runs a stereo signal through the engine in blocks. */
Stereo run (DelayEngine& engine, const DelaySettings& s, const Stereo& input, int blockSize = 256, double bpm = 120.0)
{
    Stereo out;
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (size_t start = 0; start < input.left.size(); start += (size_t)blockSize)
    {
        const auto n = (int)juce::jmin ((size_t)blockSize, input.left.size() - start);
        buffer.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample (0, i, input.left[start + (size_t)i]);
            buffer.setSample (1, i, input.right[start + (size_t)i]);
        }
        engine.process (buffer, s, bpm);
        for (int i = 0; i < n; ++i)
        {
            out.left.push_back (buffer.getSample (0, i));
            out.right.push_back (buffer.getSample (1, i));
        }
    }
    return out;
}

Stereo impulse (double seconds, float left = 1.0f, float right = 1.0f)
{
    Stereo x { std::vector<float> ((size_t)(seconds * rate), 0.0f),
               std::vector<float> ((size_t)(seconds * rate), 0.0f) };
    x.left[0] = left;
    x.right[0] = right;
    return x;
}

size_t loudestAt (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
{
    size_t best = from;
    for (auto i = from; i < juce::jmin (to, x.size()); ++i)
        if (std::abs (x[i]) > std::abs (x[best]))
            best = i;
    return best;
}

float peakAround (const std::vector<float>& x, size_t centre, size_t span = 200)
{
    float p = 0.0f;
    for (auto i = centre > span ? centre - span : 0; i < juce::jmin (x.size(), centre + span); ++i)
        p = juce::jmax (p, std::abs (x[i]));
    return p;
}

void set (DelayProcessor& d, const juce::String& id, const juce::String& text)
{
    auto* parameter = d.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    const auto value = param::normalisedForText (*parameter, text);
    REQUIRE (value.has_value());
    parameter->setValueNotifyingHost (value.value_or (0.0f));
}

struct FixedTempo final : juce::AudioPlayHead
{
    double bpm = 90.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (true);
        return info;
    }
};
} // namespace

TEST_CASE ("A synced Delay's echo comes back after its note length at the song's tempo", "[dsp][delay]")
{
    struct Case
    {
        double beats, bpm;
        size_t samples;
    };
    for (auto c : { Case { 0.5, 120.0, 12000 },       // 1/8 at 120 BPM: 250 ms
                    Case { 0.75, 100.0, 21600 },      // 1/8 dotted at 100 BPM: 450 ms
                    Case { 2.0 / 3.0, 120.0, 16000 }, // 1/4 triplet at 120 BPM: 333 ms
                    Case { 1.0, 60.0, 48000 } })      // 1/4 at 60 BPM: a second
    {
        INFO (c.beats << " beats at " << c.bpm << " BPM");
        DelayEngine engine;
        engine.prepare (rate, 256);
        auto s = plain();
        s.beats = c.beats;
        const auto out = run (engine, s, impulse (1.5), 256, c.bpm);
        CHECK (loudestAt (out.left) == c.samples);
        CHECK (loudestAt (out.right) == c.samples);
        CHECK (std::abs (out.left[c.samples]) > 0.5f);
        CHECK (engine.getTimeMs() == Catch::Approx ((double)c.samples / rate * 1000.0));
    }
    CHECK (DelayEngine::timeMsFor (plain(), 120.0) == Catch::Approx (375.0)); // the default: 1/8 dotted
}

TEST_CASE ("A free Delay's echo comes back after its time in milliseconds", "[dsp][delay]")
{
    for (auto ms : { 1.0f, 30.0f, 350.0f, 2000.0f })
    {
        INFO (ms << " ms");
        DelayEngine engine;
        engine.prepare (rate, 512);
        auto s = plain();
        s.sync = false;
        s.timeMs = ms;
        const auto out = run (engine, s, impulse (2.5), 512, 140.0); // the tempo plays no part
        CHECK (loudestAt (out.left) == (size_t)std::round (ms / 1000.0f * rate));
    }
}

TEST_CASE ("The Delay's echoes die away by the feedback", "[dsp][delay]")
{
    DelayEngine engine;
    engine.prepare (rate, 256);
    auto s = plain();
    s.sync = false;
    s.timeMs = 100.0f;
    s.feedback = 0.5f;
    const auto out = run (engine, s, impulse (1.0));
    float previous = peakAround (out.left, 4800);
    CHECK (previous > 0.5f);
    for (size_t k = 2; k <= 6; ++k)
    {
        INFO ("echo " << k);
        const auto now = peakAround (out.left, k * 4800);
        CHECK (now < previous * 0.6f);
        CHECK (now > previous * 0.4f);
        previous = now;
    }
    // Between the echoes: nothing.
    CHECK (peakAround (out.left, 7200, 1000) < 1.0e-3f);
}

TEST_CASE ("The Delay's low and high cut darken and thin each repeat", "[dsp][delay]")
{
    // A 10 kHz sine burst: the high cut at 2 kHz takes most of it from the first echo and more from each next.
    auto run10k = [] (float highCutHz)
    {
        DelayEngine engine;
        engine.prepare (rate, 256);
        auto s = plain();
        s.sync = false;
        s.timeMs = 200.0f;
        s.feedback = 0.8f;
        s.highCutHz = highCutHz;
        Stereo x { std::vector<float> ((size_t)rate, 0.0f), std::vector<float> ((size_t)rate, 0.0f) };
        for (size_t i = 0; i < 960; ++i)
            x.left[i] = x.right[i] = 0.5f * (float)std::sin (2.0 * M_PI * 10000.0 * (double)i / rate);
        const auto out = run (engine, s, x);
        return std::pair { peakAround (out.left, 9600 + 480, 480), peakAround (out.left, 19200 + 480, 480) };
    };
    const auto [openFirst, openSecond] = run10k (20000.0f);
    const auto [darkFirst, darkSecond] = run10k (2000.0f);
    CHECK (openFirst > 0.3f);
    CHECK (darkFirst < openFirst * 0.35f);
    CHECK (darkSecond < darkFirst * 0.35f);
    (void)openSecond;
}

TEST_CASE ("Ping-pong echoes alternate sides; stereo keeps them, mono puts them in the middle", "[dsp][delay]")
{
    const size_t t = 4800; // 100 ms
    auto s = plain();
    s.sync = false;
    s.timeMs = 100.0f;
    s.feedback = 0.7f;

    SECTION ("Ping-pong")
    {
        s.mode = DelaySettings::Mode::pingPong;
        DelayEngine engine;
        engine.prepare (rate, 256);
        const auto out = run (engine, s, impulse (1.0));
        for (size_t k = 1; k <= 5; ++k)
        {
            INFO ("echo " << k);
            const auto& on = k % 2 == 1 ? out.left : out.right;
            const auto& off = k % 2 == 1 ? out.right : out.left;
            CHECK (peakAround (on, k * t) > 0.05f);
            CHECK (peakAround (off, k * t) < peakAround (on, k * t) * 1.0e-3f);
        }
        // Width 0: every echo in the middle.
        s.width = 0.0f;
        DelayEngine narrow;
        narrow.prepare (rate, 256);
        const auto middle = run (narrow, s, impulse (1.0));
        CHECK (middle.left == middle.right);
    }
    SECTION ("Stereo")
    {
        DelayEngine engine;
        engine.prepare (rate, 256);
        const auto out = run (engine, s, impulse (1.0, 1.0f, 0.0f)); // a sound on the left only
        CHECK (peakAround (out.left, t) > 0.5f);
        CHECK (peakAround (out.left, 2 * t) > 0.3f);
        for (auto x : out.right)
            CHECK (juce::exactlyEqual (x, 0.0f));
    }
    SECTION ("Mono")
    {
        s.mode = DelaySettings::Mode::mono;
        DelayEngine engine;
        engine.prepare (rate, 256);
        const auto out = run (engine, s, impulse (1.0, 1.0f, 0.0f));
        CHECK (peakAround (out.left, t) > 0.25f);
        CHECK (out.left == out.right);
    }
}

TEST_CASE ("The Delay keeps the dry sound by the mix", "[dsp][delay]")
{
    DelayEngine engine;
    engine.prepare (rate, 256);
    auto s = plain();
    s.mix = 0.3f; // the default: the sound at full, the echoes at 60%
    const auto out = run (engine, s, impulse (0.5));
    CHECK (out.left[0] == Catch::Approx (1.0f));
    CHECK (peakAround (out.left, 18000) == Catch::Approx (0.6f).margin (0.1f));

    s.mix = 0.0f;
    DelayEngine dryOnly;
    dryOnly.prepare (rate, 256);
    const auto dry = run (dryOnly, s, impulse (0.5));
    CHECK (dry.left[0] == Catch::Approx (1.0f));
    CHECK (juce::exactlyEqual (peakAround (dry.left, 18000, 2000), 0.0f));
}

TEST_CASE ("A new Delay time or tempo crossfades without a click", "[dsp][delay]")
{
    auto biggestStep = [] (bool changeTempo)
    {
        DelayEngine engine;
        engine.prepare (rate, 128);
        auto s = plain();
        s.feedback = 0.6f;
        s.sync = changeTempo;
        s.timeMs = 300.0f;
        juce::AudioBuffer<float> buffer (2, 128);
        float step = 0.0f, last = 0.0f;
        double phase = 0.0;
        for (int b = 0; b < (int)(3.0 * rate / 128); ++b)
        {
            const auto seconds = b * 128 / rate;
            if (seconds > 1.5)
                s.timeMs = 120.0f; // a jump of 180 ms
            for (int i = 0; i < 128; ++i, phase += 2.0 * M_PI * 220.0 / rate)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, 0.4f * (float)std::sin (phase));
            engine.process (buffer, s, changeTempo ? (seconds > 1.5 ? 95.0 : 128.0) : 120.0);
            for (int i = 0; i < 128; ++i)
            {
                const auto x = buffer.getSample (0, i);
                if (seconds > 1.0)
                    step = juce::jmax (step, std::abs (x - last));
                last = x;
            }
        }
        return step;
    };
    // A 220 Hz sine at 0.4 moves at most 0.012 a sample; with feedback, a little more. A jump would be ~0.5.
    CHECK (biggestStep (false) < 0.06f);
    CHECK (biggestStep (true) < 0.06f);
}

TEST_CASE ("The Delay stays finite and falls silent after its tail", "[dsp][delay]")
{
    DelayEngine engine;
    engine.prepare (rate, 512);
    auto s = plain();
    s.feedback = 0.95f; // the most
    s.mode = DelaySettings::Mode::pingPong;
    s.beats = 1.0 / 6.0;
    juce::Random random (7);
    Stereo noise { std::vector<float> ((size_t)(4.0 * rate)), std::vector<float> ((size_t)(4.0 * rate)) };
    for (size_t i = 0; i < noise.left.size(); ++i)
    {
        noise.left[i] = (random.nextFloat() * 2.0f - 1.0f) * 2.0f;
        noise.right[i] = (random.nextFloat() * 2.0f - 1.0f) * 2.0f;
    }
    const auto loud = run (engine, s, noise, 512);
    for (const auto* side : { &loud.left, &loud.right })
        CHECK (std::all_of (side->begin(), side->end(),
                            [] (float x) { return std::isfinite (x) && std::abs (x) < 200.0f; }));

    // Then silence: the echoes of 1/16 triplets at 120 BPM (83 ms) at 95% fall 120 dB in about 23 s.
    const auto quiet = run (engine, s, impulse (40.0, 0.0f, 0.0f), 512);
    const auto end = quiet.left.size();
    for (const auto* side : { &quiet.left, &quiet.right })
    {
        float last = 0.0f;
        for (auto i = end - (size_t)rate; i < end; ++i)
            last = juce::jmax (last, std::abs ((*side)[i]));
        CHECK (last < 1.0e-6f);
        CHECK (std::none_of (side->begin(), side->end(), [] (float x)
                             { return ! juce::exactlyEqual (x, 0.0f) && std::abs (x) < FLT_MIN; })); // no denormals
    }

    // A fresh Delay given silence gives silence, exactly.
    DelayEngine fresh;
    fresh.prepare (rate, 512);
    const auto nothing = run (fresh, s, impulse (1.0, 0.0f, 0.0f), 512);
    CHECK (
        std::all_of (nothing.left.begin(), nothing.left.end(), [] (float x) { return juce::exactlyEqual (x, 0.0f); }));
}

TEST_CASE ("The Delay sounds the same at any block size", "[dsp][delay]")
{
    auto s = plain();
    s.feedback = 0.5f;
    s.mode = DelaySettings::Mode::pingPong;
    s.mix = 0.4f;
    std::vector<Stereo> outs;
    for (int blockSize : { 1, 37, 512 })
    {
        DelayEngine engine;
        engine.prepare (rate, blockSize);
        outs.push_back (run (engine, s, impulse (1.2), blockSize));
    }
    CHECK (outs[0].left == outs[1].left);
    CHECK (outs[1].right == outs[2].right);
}

TEST_CASE ("The Delay plays mono too", "[dsp][delay]")
{
    DelayEngine engine;
    engine.prepare (rate, 256);
    auto s = plain();
    s.mode = DelaySettings::Mode::pingPong; // on one channel: echoes of itself
    s.feedback = 0.5f;
    juce::AudioBuffer<float> buffer (1, 256);
    std::vector<float> out;
    for (int b = 0; b < 200; ++b)
    {
        buffer.clear();
        if (b == 0)
            buffer.setSample (0, 0, 1.0f);
        engine.process (buffer, s, 120.0);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 256);
    }
    CHECK (loudestAt (out) == 18000);
    CHECK (peakAround (out, 36000) > 0.3f);
}

TEST_CASE ("The Delay's settings read as text and its state holds them", "[dsp][delay]")
{
    DelayProcessor a;
    set (a, "note", "1/4 triplet");
    set (a, "mode", "ping-pong");
    set (a, "feedback", "60%");
    set (a, "mix", "100%");
    set (a, "high_cut", "4.5 kHz");
    set (a, "low_cut", "250 Hz");
    set (a, "time", "1.5 s");
    auto s = a.getSettings();
    CHECK (s.sync);
    CHECK (s.beats == Catch::Approx (2.0 / 3.0));
    CHECK (s.mode == DelaySettings::Mode::pingPong);
    CHECK (s.feedback == Catch::Approx (0.6f).margin (0.005f));
    CHECK (s.highCutHz == Catch::Approx (4500.0f).epsilon (0.01));
    CHECK (s.timeMs == Catch::Approx (1500.0f).epsilon (0.01));
    auto& note = *a.getParameterState().getParameter ("note");
    CHECK (note.getText (note.getValue(), 100) == "1/4 triplet");
    auto& time = *a.getParameterState().getParameter ("time");
    set (a, "time", "350 ms");
    CHECK (time.getText (time.getValue(), 100) == "350 ms");
    auto& highCut = *a.getParameterState().getParameter ("high_cut");
    CHECK (highCut.getText (highCut.getValue(), 100) == "4.5 kHz");

    juce::MemoryBlock state;
    a.getStateInformation (state);
    DelayProcessor b;
    b.setStateInformation (state.getData(), (int)state.getSize());
    s = b.getSettings();
    CHECK (s.beats == Catch::Approx (2.0 / 3.0));
    CHECK (s.mode == DelaySettings::Mode::pingPong);
    CHECK (s.mix == Catch::Approx (1.0f));
    CHECK (s.timeMs == Catch::Approx (350.0f).epsilon (0.01));
    CHECK (s.lowCutHz == Catch::Approx (250.0f).epsilon (0.01));
}

TEST_CASE ("The Delay takes the tempo from the song", "[dsp][delay]")
{
    DelayProcessor d;
    set (d, "note", "1/4");
    FixedTempo head;
    d.setPlayHead (&head);
    d.prepareToPlay (rate, 256);
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    buffer.clear();
    d.processBlock (buffer, midi);
    CHECK (d.getEngine().getTimeMs() == Catch::Approx (60000.0 / 90.0));

    std::unique_ptr<juce::AudioProcessorEditor> editor (d.createEditorAndMakeActive());
    auto* window = dynamic_cast<DelayEditor*> (editor.get());
    REQUIRE (window != nullptr);
    CHECK (window->getTimeText() == "1/4 at 90 BPM: 667 ms");
    set (d, "sync", "Free");
    CHECK (window->getTimeText() == "350 ms");
    d.editorBeingDeleted (editor.get());
    d.setPlayHead (nullptr);
    CHECK (d.getTailLengthSeconds() > 1.0);
}

TEST_CASE ("The Delay on a mixer insert, and the AI's sends find it", "[dsp][delay]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), {});
    Session session (host.getEngine(), std::move (devices));
    ui::Mixer mixer (session);
    REQUIRE (mixer.addEffect (model::masterInsertId, "spinus.delay"));
    const auto& effects = session.getProject().mixer.master.effects;
    REQUIRE (effects.size() == 1);
    CHECK (session.getAdapter().getMissingDevices().empty());
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (effects[0].device.id));
    REQUIRE (plugin != nullptr);
    CHECK (dynamic_cast<DelayProcessor*> (plugin->getAudioPluginInstance()) != nullptr);

    // In the device list as an effect Spinus has, which the Track creator's sends take as a delay.
    const auto& entries = session.getDeviceList().getEntries();
    const auto entry = std::find_if (entries.begin(), entries.end(),
                                     [] (const auto& e) { return e.device.stockType == "spinus.delay"; });
    REQUIRE (entry != entries.end());
    CHECK (entry->name == "Spinus Delay");
    CHECK (! entry->instrument);
    CHECK (entry->available);
    CHECK (song::sendEffectKind (entry->name) == "delay");
}

TEST_CASE ("Delay window screenshot for the manual", "[.screenshot][delay]")
{
    DelayProcessor delay;
    set (delay, "mode", "Ping-pong");
    set (delay, "feedback", "55%");
    set (delay, "mix", "100%");
    set (delay, "high_cut", "5 kHz");
    set (delay, "low_cut", "200 Hz");
    std::unique_ptr<juce::AudioProcessorEditor> editor (delay.createEditorAndMakeActive());
    editor->setSize (640, 350);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/delay.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    delay.editorBeingDeleted (editor.get());
}

TEST_CASE ("Delay CPU cost: ping-pong, feedback filters, stereo, 48 kHz, 128 samples", "[.bench][delay]")
{
    DelayProcessor delay;
    set (delay, "mode", "Ping-pong");
    set (delay, "feedback", "60%");
    delay.prepareToPlay (rate, 128);
    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;
    juce::Random noise (3);

    const int blocks = (int)(5.0 * rate / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
    {
        for (int c = 0; c < 2; ++c)
            buffer.setSample (c, 0, noise.nextFloat());
        delay.processBlock (buffer, midi);
    }
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "Delay: " << seconds / 5.0 * 100.0 << " % of one core\n";
}
