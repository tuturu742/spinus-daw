#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"

using namespace spinus;
namespace te = tracktion;

namespace
{
/** A session with two 3x Osc channels and a one-bar pattern, played in real time; the test plays the
    engine's MIDI input like a keyboard. */
struct Fixture
{
    Fixture()
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        rack = std::make_unique<ui::ChannelRack> (*session);
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Lead"));
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Bass"));
        REQUIRE (rack->toggleStep (session->getProject().channels[1].id, 0)); // makes a one-bar pattern

        // The test engine's MIDI input (the runner's hosted device makes it).
        runner.start();
        runner.runUntil (0.3);
        for (auto& d : host.getEngine().getDeviceManager().getMidiInDevices())
            if (dynamic_cast<te::VirtualMidiInputDevice*> (d.get()) == nullptr)
                keyboard = d;
        REQUIRE (keyboard != nullptr);
    }

    ~Fixture()
    {
        runner.stop();
        rack = nullptr;
        session = nullptr;
    }

    void press (int note, float velocity = 0.8f) { keyboard->keyboardState.noteOn (1, note, velocity); }
    void release (int note) { keyboard->keyboardState.noteOff (1, note, 0.0f); }
    void waitUntil (double seconds) { runner.runUntil (seconds); }

    const std::vector<model::Note>* notesOf (size_t channel) const
    {
        const auto* pattern = session->getProject().findPattern (session->getCurrentPattern());
        if (pattern == nullptr)
            return nullptr;
        auto it = pattern->notes.find (session->getProject().channels[channel].id);
        return it != pattern->notes.end() ? &it->second : nullptr;
    }

    EngineHost host { { .openAudioDevice = false } };
    test::RealtimeRunner runner { host.getEngine(),
                                  { .outputChannels = 2, .maxSeconds = 20.0, .paceInRealTime = true } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ui::ChannelRack> rack;
    std::shared_ptr<te::MidiInputDevice> keyboard;
};
} // namespace

TEST_CASE ("The MIDI keyboard plays the selected channel", "[session][recording][audio]")
{
    Fixture f;
    CHECK (f.session->getRecorder().getInputNames().contains (f.keyboard->getName()));
    CHECK (f.session->getSelectedChannel() == f.session->getProject().channels[1].id); // the newest

    const auto sr = f.runner.getParams().sampleRate;
    const auto before = f.runner.getSecondsProcessed();
    f.press (69);
    f.waitUntil (before + 0.4);
    f.release (69);
    f.waitUntil (before + 0.8);
    CHECK (test::checks::rms (f.runner.getChannel (0), (int64_t)((before + 0.1) * sr),
                              (int64_t)((before + 0.35) * sr)) > 0.01f);

    // Selecting the other channel moves the keyboard there.
    auto* context = f.session->getEdit().getCurrentPlaybackContext();
    REQUIRE (context != nullptr);
    f.session->setSelectedChannel (f.session->getProject().channels[0].id);
    auto* lead = f.session->getAdapter().getChannelTrack (f.session->getProject().channels[0].id);
    REQUIRE (lead != nullptr);
    for (auto* instance : context->getAllInputs())
        if (&instance->owner == f.keyboard.get())
            CHECK (instance->getTargets() == juce::Array<te::EditItemID> { lead->itemID });
}

TEST_CASE ("Recording writes the played notes into the current pattern", "[session][recording][audio]")
{
    Fixture f;
    auto& session = *f.session;
    const auto* before = f.notesOf (1);
    REQUIRE (before != nullptr);
    REQUIRE (before->size() == 1); // the step at 0

    session.setRecordArmed (true);
    CHECK (session.getMode() == Session::Mode::pattern);
    const auto start = f.runner.getSecondsProcessed();
    session.play();
    CHECK (session.isRecording());

    // 120 BPM: a beat is 0.5 s. Play E at beat 1 (0.5 s) and G at beat 2.5 (1.25 s).
    f.waitUntil (start + 0.5);
    f.press (64, 1.0f);
    f.waitUntil (start + 0.8);
    f.release (64);
    f.waitUntil (start + 1.25);
    f.press (67, 0.5f);
    f.waitUntil (start + 1.4);
    f.release (67);
    f.waitUntil (start + 1.6);
    session.stop();

    CHECK (! session.isRecording());
    CHECK (session.getLastRecordedCount() == 2);
    const auto* notes = f.notesOf (1);
    REQUIRE (notes != nullptr);
    REQUIRE (notes->size() == 3);

    auto find = [&] (int pitch) -> const model::Note*
    {
        for (auto& n : *notes)
            if (n.pitch == pitch)
                return &n;
        return nullptr;
    };
    const auto* e = find (64);
    const auto* g = find (67);
    REQUIRE (e != nullptr);
    REQUIRE (g != nullptr);
    // Quantized to sixteenths (240 ticks); real-time timing may land a step either side.
    CHECK (e->start % 240 == 0);
    CHECK (std::abs (e->start - 960) <= 240);
    CHECK (std::abs (g->start - 2400) <= 240);
    CHECK (std::abs (e->length - 576) <= 240); // 0.3 s
    CHECK (e->velocity > g->velocity);
    CHECK (session.getHistory().getUndoDescriptions().strings.getLast() == "Record notes");

    // One undo step removes the take.
    REQUIRE (session.getHistory().undo());
    REQUIRE (f.notesOf (1) != nullptr);
    CHECK (f.notesOf (1)->size() == 1);

    // Disarmed: playing does not record.
    session.setRecordArmed (false);
    session.play();
    CHECK (! session.isRecording());
    f.press (60);
    f.waitUntil (f.runner.getSecondsProcessed() + 0.2);
    f.release (60);
    session.stop();
    CHECK (f.notesOf (1)->size() == 1);
}

TEST_CASE ("The computer keyboard plays and records the selected channel", "[session][recording][audio]")
{
    Fixture f;
    auto& session = *f.session;
    auto& recorder = session.getRecorder();
    // The computer keyboard is an input of its own, apart from the MIDI keyboards.
    REQUIRE (recorder.hasComputerKeyboard());
    CHECK (! recorder.getInputNames().contains (MidiRecorder::computerKeyboardName));

    const auto sr = f.runner.getParams().sampleRate;
    const auto before = f.runner.getSecondsProcessed();
    recorder.playComputerKey (69, true);
    f.waitUntil (before + 0.4);
    recorder.playComputerKey (69, false);
    f.waitUntil (before + 0.8);
    CHECK (test::checks::rms (f.runner.getChannel (0), (int64_t)((before + 0.1) * sr),
                              (int64_t)((before + 0.35) * sr)) > 0.01f);

    session.setRecordArmed (true);
    const auto start = f.runner.getSecondsProcessed();
    session.play();
    f.waitUntil (start + 0.5);
    recorder.playComputerKey (72, true);
    f.waitUntil (start + 0.8);
    recorder.playComputerKey (72, false);
    f.waitUntil (start + 1.0);
    session.stop();
    CHECK (session.getLastRecordedCount() == 1);
    const auto* notes = f.notesOf (1);
    REQUIRE (notes != nullptr);
    CHECK (std::any_of (notes->begin(), notes->end(), [] (auto& n) { return n.pitch == 72; }));
}
