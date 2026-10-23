#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "dsp/ducker/DuckerEditor.h"
#include "dsp/ducker/DuckerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"

using namespace spinus;
using namespace spinus::dsp;
namespace te = tracktion;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 128;

/** Kick-like hits on the sidechain: a decaying 60 Hz burst every `every` seconds. */
std::vector<float> kicks (double seconds, double every, float level = 0.8f)
{
    std::vector<float> x ((size_t)(seconds * rate), 0.0f);
    for (int hit = 0; 0.1 + hit * every < seconds; ++hit)
        for (int i = 0; i < (int)(0.15 * rate) && (size_t)((0.1 + hit * every) * rate) + (size_t)i < x.size(); ++i)
            x[(size_t)((0.1 + hit * every) * rate) + (size_t)i] =
                level * std::exp (-(float)i / (float)(0.03 * rate)) * (float)std::sin (2.0 * M_PI * 60.0 * i / rate);
    return x;
}

/** Runs constant main audio (1.0) through the engine with the sidechain signal; returns the gain per sample. */
std::vector<float> gains (DuckerEngine& engine, const DuckerSettings& s, const std::vector<float>& sidechain,
                          double seconds, double startBeat = -1.0)
{
    std::vector<float> out;
    juce::AudioBuffer<float> main (2, block), side (2, block);
    const auto total = (size_t)(seconds * rate);
    for (size_t start = 0; start + block <= total; start += block)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
            {
                main.setSample (c, i, 1.0f);
                side.setSample (c, i, start + (size_t)i < sidechain.size() ? sidechain[start + (size_t)i] : 0.0f);
            }
        const auto beat = startBeat < 0.0 ? -1.0 : startBeat + (double)start / rate * 2.0; // 120 BPM
        engine.process (main, side, s, 120.0, beat);
        for (int i = 0; i < block; ++i)
            out.push_back (main.getSample (0, i));
    }
    return out;
}

float at (const std::vector<float>& x, double seconds) { return x[(size_t)(seconds * rate)]; }

void set (DuckerProcessor& d, const juce::String& id, float value)
{
    auto* parameter = d.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}
} // namespace

TEST_CASE ("The dip's shape: down quickly, back over the release, the curve holds it down longer", "[dsp][ducker]")
{
    DuckerSettings s;
    s.amount = 0.8f;
    s.release = 0.3f;
    s.curve = 0.0f;
    CHECK (DuckerEngine::gainAt (s, 0.0f) == Catch::Approx (1.0f));
    CHECK (DuckerEngine::gainAt (s, DuckerEngine::attackSeconds) == Catch::Approx (0.2f));
    CHECK (DuckerEngine::gainAt (s, DuckerEngine::attackSeconds + 0.15f) == Catch::Approx (0.6f)); // halfway
    CHECK (DuckerEngine::gainAt (s, DuckerEngine::attackSeconds + 0.3f) == Catch::Approx (1.0f));

    const auto even = DuckerEngine::gainAt (s, DuckerEngine::attackSeconds + 0.15f);
    s.curve = 1.0f;
    CHECK (DuckerEngine::gainAt (s, DuckerEngine::attackSeconds + 0.15f) < even - 0.2f);
}

TEST_CASE ("Each sidechain hit dips the sound once", "[dsp][ducker]")
{
    DuckerEngine engine;
    engine.prepare (rate);
    DuckerSettings s;
    s.amount = 0.9f;
    s.release = 0.2f;

    const auto g = gains (engine, s, kicks (2.0, 0.5), 2.0);
    CHECK (engine.getNumTriggers() == 4); // hits at 0.1, 0.6, 1.1 and 1.6 s
    CHECK (at (g, 0.05) == Catch::Approx (1.0f));
    CHECK (at (g, 0.1 + 0.006) < 0.15f);                              // down within a few milliseconds of the hit
    CHECK (at (g, 0.1 + 0.25) == Catch::Approx (1.0f).margin (0.01)); // back up after the release
    CHECK (at (g, 0.6 + 0.006) < 0.15f);
}

TEST_CASE ("No sidechain sound, no dip; a long sound triggers once", "[dsp][ducker]")
{
    DuckerEngine engine;
    engine.prepare (rate);
    DuckerSettings s;

    const auto silent = gains (engine, s, {}, 1.0);
    CHECK (engine.getNumTriggers() == 0);
    CHECK (std::all_of (silent.begin(), silent.end(), [] (float x) { return juce::exactlyEqual (x, 1.0f); }));

    std::vector<float> bass ((size_t)rate);
    for (size_t i = 0; i < bass.size(); ++i)
        bass[i] = 0.5f * (float)std::sin (2.0 * M_PI * 55.0 * (double)i / rate);
    gains (engine, s, bass, 1.0);
    CHECK (engine.getNumTriggers() == 1);
}

TEST_CASE ("Beat triggers dip in time with the song, without a sidechain", "[dsp][ducker]")
{
    DuckerEngine engine;
    engine.prepare (rate);
    DuckerSettings s;
    s.trigger = DuckerSettings::Trigger::everyBeat;
    s.amount = 1.0f;
    s.release = 0.1f;

    const auto g = gains (engine, s, {}, 2.0, 0.0); // 120 BPM from the start: beats at 0, 0.5, 1 and 1.5 s
    CHECK (engine.getNumTriggers() == 4);
    CHECK (at (g, 0.5 + 0.004) < 0.05f);
    CHECK (at (g, 0.5 - 0.01) == Catch::Approx (1.0f).margin (0.01));

    s.trigger = DuckerSettings::Trigger::everyHalfBeat;
    engine.reset();
    gains (engine, s, {}, 2.0, 0.0);
    CHECK (engine.getNumTriggers() == 8);

    engine.reset();
    gains (engine, s, {}, 2.0, -1.0); // stopped
    CHECK (engine.getNumTriggers() == 0);
}

TEST_CASE ("The Ducker's state holds its settings", "[dsp][ducker]")
{
    DuckerProcessor a;
    set (a, "trigger", 2.0f);
    set (a, "amount", 0.4f);
    set (a, "release", 0.5f);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    DuckerProcessor b;
    b.setStateInformation (state.getData(), (int)state.getSize());
    const auto s = b.getSettings();
    CHECK (s.trigger == DuckerSettings::Trigger::everyHalfBeat);
    CHECK (s.amount == Catch::Approx (0.4f).margin (0.001));
    CHECK (s.release == Catch::Approx (0.5f).margin (0.001));
}

TEST_CASE ("Sidechain routing is a command: set, undo, and no feedback loops", "[commands][ducker]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ui::Mixer mixer (session);
    REQUIRE (mixer.addInsert());
    REQUIRE (mixer.addInsert());
    const auto kick = session.getProject().mixer.inserts[0].id;
    const auto bass = session.getProject().mixer.inserts[1].id;

    REQUIRE (mixer.setSidechain (bass, kick));
    CHECK (session.getProject().findInsert (bass)->sidechainSources == std::vector { kick });
    CHECK (session.getHistory().getUndoDescriptions().joinIntoString (" | ").endsWith ("Set sidechain"));

    // The bass waits for the kick's sound, so the kick may not take the bass's sound in any way.
    CHECK (! mixer.setOutput (bass, kick));
    CHECK (! mixer.setSend (bass, kick, -6.0));
    CHECK (! mixer.setSidechain (kick, bass));
    CHECK (! mixer.setSidechain (bass, bass));
    CHECK (mixer.setOutput (kick, bass)); // the same direction: fine

    REQUIRE (mixer.setSidechain (bass, {}));
    CHECK (session.getProject().findInsert (bass)->sidechainSources.empty());
    session.getHistory().undo();
    CHECK (session.getProject().findInsert (bass)->sidechainSources == std::vector { kick });
}

TEST_CASE ("A Ducker on the bass's insert follows the kick's insert", "[dsp][ducker][audio]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), {});
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    ui::Mixer mixer (session);

    REQUIRE (rack.addChannel ("spinus.3x-osc", "Kick"));
    REQUIRE (rack.addChannel ("spinus.3x-osc", "Bass"));
    const auto& channels = session.getProject().channels;
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (rack.toggleStep (channels[0].id, step));
    REQUIRE (rack.toggleStep (channels[1].id, 0));
    const auto kickInsert = channels[0].targetInsert;
    const auto bassInsert = channels[1].targetInsert;
    REQUIRE (kickInsert != bassInsert);

    REQUIRE (mixer.addEffect (bassInsert, "spinus.ducker"));
    const auto device = session.getProject().findInsert (bassInsert)->effects[0].device.id;
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    REQUIRE (plugin != nullptr);
    CHECK (plugin->canSidechain());
    auto* ducker = dynamic_cast<DuckerProcessor*> (plugin->getAudioPluginInstance());
    REQUIRE (ducker != nullptr);

    // Dips counted over two seconds (four kicks at 120 BPM) once the Ducker reacts. Paced in real time:
    // the sidechain connection is built on the message thread, which can take seconds on a busy computer.
    float deepest = 0.0f, busiest = 0.0f; // the Mixer's reading of the Ducker: gain reduction and CPU
    auto play = [&] (double waitSeconds)
    {
        test::RealtimeRunner runner (host.getEngine(),
                                     { .outputChannels = 2, .maxSeconds = waitSeconds + 3.0, .paceInRealTime = true });
        session.getEdit().dispatchPendingUpdatesSynchronously();
        const auto before = ducker->getEngine().getNumTriggers();
        runner.start();
        // Opening the stand-in sound card can stop the transport a moment later on a busy computer: play
        // again if it did.
        while (ducker->getEngine().getNumTriggers() == before && runner.getSecondsProcessed() < waitSeconds)
        {
            if (! session.isPlaying())
                session.play();
            runner.runUntil (runner.getSecondsProcessed() + 0.05);
        }
        // A graph rebuild can give one stray dip before the connection is there: count two-second windows
        // until one has dips in it (or the time is up).
        int hits = 0;
        do
        {
            if (! session.isPlaying())
                session.play();
            const auto from = ducker->getEngine().getNumTriggers();
            const auto until = runner.getSecondsProcessed() + 2.0;
            while (runner.getSecondsProcessed() < until)
            {
                runner.runUntil (juce::jmin (until, runner.getSecondsProcessed() + 0.02));
                if (auto activity = session.getAdapter().getDeviceActivity (device))
                {
                    deepest = juce::jmin (deepest, activity->gainReductionDb);
                    busiest = juce::jmax (busiest, activity->load);
                }
            }
            hits = ducker->getEngine().getNumTriggers() - from;
        } while (hits == 0 && runner.getSecondsProcessed() < waitSeconds);
        runner.stop();
        session.stop();
        return hits;
    };

    CHECK (play (1.0) == 0); // nothing on the sidechain yet
    CHECK (deepest > -0.5f); // and no reduction seen
    CHECK (busiest > 0.0f);
    REQUIRE (mixer.setSidechain (bassInsert, kickInsert));
    const auto hits = play (20.0);
    INFO ("hits " << hits);
    CHECK (hits >= 3); // a kick on every beat, at 120 BPM: four in two seconds
    CHECK (hits <= 5);
    // The default Ducker dips by 70% (about -10 dB); the measured reduction shows most of that.
    INFO ("deepest " << deepest);
    CHECK (deepest < -6.0f);
    CHECK (deepest > -40.0f);
}

TEST_CASE ("The Ducker's window says where its trigger comes from", "[dsp][ducker][ui]")
{
    DuckerProcessor ducker;
    ducker.prepareToPlay (rate, block);
    std::unique_ptr<juce::AudioProcessorEditor> editor (ducker.createEditorAndMakeActive());
    auto* window = dynamic_cast<DuckerEditor*> (editor.get());
    REQUIRE (window != nullptr);
    CHECK (window->getStatusText().startsWith ("No sound on the sidechain"));

    set (ducker, "trigger", 1.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    CHECK (window->getStatusText().startsWith ("Dips in time with the song"));
    ducker.editorBeingDeleted (editor.get());
}

TEST_CASE ("Ducker window screenshot for the manual", "[.screenshot][ducker]")
{
    DuckerProcessor ducker;
    set (ducker, "amount", 0.8f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (ducker.createEditorAndMakeActive());
    editor->setSize (520, 350);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/ducker.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    ducker.editorBeingDeleted (editor.get());
}

TEST_CASE ("Ducker CPU cost: sidechain, stereo, 48 kHz, 128 samples", "[.bench][ducker]")
{
    DuckerEngine engine;
    engine.prepare (rate);
    const DuckerSettings s;
    const auto side = kicks (5.0, 0.5);
    const auto start = juce::Time::getHighResolutionTicks();
    gains (engine, s, side, 5.0);
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "Ducker (with the test's buffer filling): " << seconds / 5.0 * 100.0 << " % of one core\n";
}
