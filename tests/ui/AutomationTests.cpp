#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::ui;
namespace te = tracktion;

namespace
{
using Kind = model::ParameterTarget::Kind;

struct Fixture
{
    Fixture()
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        rack = std::make_unique<ChannelRack> (*session);
        playlist = std::make_unique<Playlist> (*session);
        playlist->setSize (900, 400);
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Lead"));
    }

    ~Fixture()
    {
        playlist = nullptr;
        rack = nullptr;
        session = nullptr;
    }

    const model::Channel& lead() const { return session->getProject().channels.front(); }

    model::ParameterTarget insertVolume() const
    {
        model::ParameterTarget t;
        t.kind = Kind::insertVolume;
        t.insert = lead().targetInsert;
        return t;
    }

    const model::AutomationClip& automationOf (model::ClipId id) const
    {
        return std::get<model::AutomationClip> (session->getProject().findClip (id)->content);
    }

    bool perform (const juce::String& name, const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
        INFO (error);
        return command != nullptr && session->getHistory().perform (std::move (command)).wasOk();
    }

    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<Playlist> playlist;
};
} // namespace

TEST_CASE ("Create automation clip: a flat line at the current value, on a new lane", "[ui][automation]")
{
    Fixture f;
    auto* parameter = f.session->getAdapter().findParameter (f.insertVolume());
    REQUIRE (parameter != nullptr);
    const auto current = parameter->getCurrentNormalisedValue();

    const auto lanes = f.session->getProject().playlist.lanes.size();
    const auto id = f.session->createAutomationClip (f.insertVolume());
    REQUIRE (id.isValid());
    CHECK (f.session->getProject().playlist.lanes.size() == lanes + 1);
    CHECK (f.session->getProject().playlist.lanes.back().name == "Lead volume");
    const auto& points = f.automationOf (id).points;
    REQUIRE (points.size() == 2);
    CHECK (juce::approximatelyEqual (points[0].value, current));
    CHECK (points[1].time == 4 * 3840);

    // The engine follows it in song mode only.
    f.session->setMode (Session::Mode::pattern);
    CHECK (parameter->getCurve().getNumPoints() == 0);
    f.session->setMode (Session::Mode::song);
    CHECK (parameter->getCurve().getNumPoints() == 2);

    // A muted lane, or a muted clip, does nothing.
    REQUIRE (f.playlist->setLaneMuted ((int)lanes, true));
    CHECK (parameter->getCurve().getNumPoints() == 0);
    REQUIRE (f.session->getHistory().undo());
    CHECK (parameter->getCurve().getNumPoints() == 2);

    // Nothing to automate: no clip.
    model::ParameterTarget unknown;
    unknown.kind = Kind::deviceParameter;
    unknown.device = f.lead().instrument.value_or (model::Device {}).id;
    unknown.parameterId = "no-such-parameter";
    CHECK (! f.session->createAutomationClip (unknown).isValid());
}

TEST_CASE ("Automation holds its value between clips, and reaches device parameters", "[ui][automation]")
{
    Fixture f;
    f.session->setMode (Session::Mode::song);

    // A device parameter: 3x Osc's cutoff, by its parameter ID.
    const auto device = f.lead().instrument.value_or (model::Device {}).id;
    const auto parameters = f.session->getDeviceParameters (device);
    CHECK (std::find (parameters.begin(), parameters.end(),
                      std::pair<juce::String, juce::String> ("cutoff", "Cutoff")) != parameters.end());
    model::ParameterTarget cutoff;
    cutoff.kind = Kind::deviceParameter;
    cutoff.device = device;
    cutoff.parameterId = "cutoff";
    const auto first = f.session->createAutomationClip (cutoff);
    REQUIRE (first.isValid());
    CHECK (f.session->getProject().playlist.lanes.back().name == "3x Osc Cutoff");

    // Two clips on that lane: 0.2 -> 0.8 in bar 1, then 0.5 from bar 3. Between them it holds 0.8.
    const auto lane = f.session->getProject().findClip (first)->lane;
    REQUIRE (f.perform ("clip.set", "{\"clip\": " + juce::String ((int)first.value) + ", \"length\": 3840}"));
    REQUIRE (f.perform ("clip.setPoints",
                        "{\"clip\": " + juce::String ((int)first.value) +
                            ", \"points\": [{\"time\": 0, \"value\": 0.2}, {\"time\": 3840, \"value\": 0.8}]}"));
    REQUIRE (f.perform ("clip.addAutomation",
                        "{\"lane\": " + juce::String ((int)lane.value) +
                            R"(, "start": 7680, "length": 3840, "target": {"kind": "deviceParameter", "device": )" +
                            juce::String ((int)cutoff.device.value) +
                            R"(, "parameterId": "cutoff"}, "points": [{"time": 0, "value": 0.5}]})"));

    auto* parameter = f.session->getAdapter().findParameter (cutoff);
    REQUIRE (parameter != nullptr);
    auto& curve = parameter->getCurve();
    auto at = [&] (double beats)
    {
        const auto time = f.session->getEdit().tempoSequence.toTime (te::BeatPosition::fromBeats (beats));
        return parameter->valueRange.convertTo0to1 (curve.getValueAt (time, 0.0f));
    };
    CHECK (std::abs (at (0.0) - 0.2f) < 0.01f);
    CHECK (std::abs (at (2.0) - 0.5f) < 0.02f);  // halfway up the first ramp
    CHECK (std::abs (at (6.0) - 0.8f) < 0.01f);  // held between the clips
    CHECK (std::abs (at (12.0) - 0.5f) < 0.01f); // the second clip
}

TEST_CASE ("An automated insert volume fades the sound in", "[ui][automation][audio]")
{
    Fixture f;
    f.session->setMode (Session::Mode::song);
    REQUIRE (f.rack->toggleStep (f.lead().id, 0)); // a pattern
    for (int step = 1; step < 16; ++step)
        REQUIRE (f.rack->toggleStep (f.lead().id, step));
    REQUIRE (f.playlist->placePattern (0, 0));

    // Silent for the first beat, then up to full volume.
    const auto id = f.session->createAutomationClip (f.insertVolume());
    REQUIRE (f.perform ("clip.setPoints", "{\"clip\": " + juce::String ((int)id.value) +
                                              ", \"points\": [{\"time\": 0, \"value\": 0}, {\"time\": 960, \"value\": "
                                              "0}, {\"time\": 1440, \"value\": 0.8}]}"));

    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 4.0, .paceInRealTime = true });
    runner.start();
    runner.runUntil (0.2);
    const auto start = runner.getSecondsProcessed();
    f.session->play();
    runner.runUntil (start + 1.3);
    f.session->stop();
    runner.stop();

    const auto sr = runner.getParams().sampleRate;
    auto level = [&] (double from, double to)
    { return test::checks::rms (runner.getChannel (0), (int64_t)((start + from) * sr), (int64_t)((start + to) * sr)); };
    CHECK (level (0.1, 0.4) < 0.002f); // first beat: 0.0-0.5 s at 120 BPM
    CHECK (level (0.9, 1.2) > 0.01f);  // after the ramp
}

TEST_CASE ("Automation points are edited on the Playlist", "[ui][automation]")
{
    Fixture f;
    const auto id = f.session->createAutomationClip (f.insertVolume());
    REQUIRE (id.isValid());

    REQUIRE (f.playlist->addAutomationPoint (id, 1920, 0.25f));
    REQUIRE (f.automationOf (id).points.size() == 3);
    CHECK (f.automationOf (id).points[1].time == 1920); // kept in time order

    REQUIRE (f.playlist->moveAutomationPoint (id, 1, 960, 1.0f));
    CHECK (f.automationOf (id).points[1].time == 960);
    CHECK (juce::approximatelyEqual (f.automationOf (id).points[1].value, 1.0f));

    REQUIRE (f.playlist->removeAutomationPoint (id, 1));
    CHECK (f.automationOf (id).points.size() == 2);
    REQUIRE (f.playlist->removeAutomationPoint (id, 0));
    CHECK (! f.playlist->removeAutomationPoint (id, 0)); // the last point stays

    // Each edit is one undo step.
    REQUIRE (f.session->getHistory().undo());
    CHECK (f.automationOf (id).points.size() == 2);
    CHECK (f.session->getHistory().getRedoDescriptions()[0] == "Edit automation");

    // Painting works.
    CHECK (f.playlist->createComponentSnapshot (f.playlist->getLocalBounds()).isValid());
}

TEST_CASE ("The last control moved in a plugin's window can be automated; the menu shows values", "[ui][automation]")
{
    Fixture f;
    auto& session = *f.session;
    CHECK (! session.getLastMovedControl());

    // Move a 3x Osc control the way its window does (notifying the host).
    const auto device = f.lead().instrument.value_or (model::Device {}).id;
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    REQUIRE (plugin != nullptr);
    auto* instance = plugin->getAudioPluginInstance();
    REQUIRE (instance != nullptr);
    auto* control = instance->getParameters()[3];
    control->beginChangeGesture();
    control->setValueNotifyingHost (0.25f);
    control->endChangeGesture();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    const auto last = session.getLastMovedControl();
    REQUIRE (last);
    const auto moved = last.value_or (model::ParameterTarget {});
    CHECK (moved.device == device);
    CHECK (session.describeLastMovedControl().contains ("3x Osc - Lead"));
    CHECK (session.getDeviceParameterValueText (device, moved.parameterId).isNotEmpty());

    // Automation playing back does not count as moving it.
    const auto clip = session.createAutomationClip (moved);
    REQUIRE (clip.isValid());
    CHECK (f.automationOf (clip).target == moved);
    CHECK (session.getLastMovedControl().value_or (model::ParameterTarget {}) == moved);
}

TEST_CASE ("A channel's volume knob moves with its automation while it plays", "[ui][automation][audio]")
{
    Fixture f;
    f.session->setMode (Session::Mode::song);
    REQUIRE (f.rack->toggleStep (f.lead().id, 0));
    REQUIRE (f.playlist->placePattern (0, 0));
    f.rack->setVisible (true);

    // Volume down to -24 dB for the whole first bar.
    model::ParameterTarget volume;
    volume.kind = Kind::channelVolume;
    volume.channel = f.lead().id;
    const auto id = f.session->createAutomationClip (volume);
    REQUIRE (id.isValid());
    const auto low = tracktion::decibelsToVolumeFaderPosition (-24.0f);
    REQUIRE (f.perform ("clip.setPoints", "{\"clip\": " + juce::String ((int)id.value) +
                                              ", \"points\": [{\"time\": 0, \"value\": " + juce::String (low) +
                                              "}, {\"time\": 3840, \"value\": " + juce::String (low) + "}]}"));

    juce::Slider* knob = nullptr;
    std::function<void (juce::Component&)> look = [&] (juce::Component& c)
    {
        if (auto* s = dynamic_cast<juce::Slider*> (&c); s != nullptr && s->getHelpText().startsWith ("Channel volume"))
            knob = s;
        for (auto* child : c.getChildren())
            look (*child);
    };
    f.rack->updateNow();
    look (*f.rack);
    REQUIRE (knob != nullptr);
    CHECK (std::abs (knob->getValue()) < 0.01); // the channel's own 0 dB

    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = true });
    runner.start();
    f.session->play();
    runner.runUntil (0.5);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200); // the rack's timer
    CHECK (std::abs (knob->getValue() - -24.0) < 0.5);

    // Pattern mode: no automation, so the knob shows the channel's own volume again.
    f.session->stop();
    runner.stop();
    f.session->setMode (Session::Mode::pattern);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    CHECK (std::abs (knob->getValue()) < 0.01);
}
