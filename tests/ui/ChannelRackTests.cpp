#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
juce::File bundledPlugins()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR);
#else
    return {};
#endif
}

struct Rack
{
    Rack() : session (host.getEngine(), makeDevices (host.getEngine())), rack (session) {}

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
        return devices;
    }

    model::ChannelId lastChannel() const { return session.getProject().channels.back().id; }

    EngineHost host { { .openAudioDevice = false } };
    Session session;
    ChannelRack rack;
};
} // namespace

TEST_CASE ("Clicking a step adds and removes a C5 sixteenth note", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ({}, "Kick"));
    r.rack.updateNow();
    CHECK (r.rack.getNumRows() == 1);
    const auto kick = r.lastChannel();

    // No pattern yet: the first step creates a one-bar pattern (16 steps in 4/4).
    REQUIRE (r.rack.toggleStep (kick, 4));
    REQUIRE (r.session.getCurrentPattern().isValid());
    CHECK (r.rack.getNumSteps() == 16);
    CHECK (r.rack.isStepOn (kick, 4));
    CHECK (! r.rack.isStepOn (kick, 0));

    const auto& notes = r.session.getProject().findPattern (r.session.getCurrentPattern())->notes.at (kick);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].start == 4 * 240);
    CHECK (notes[0].length == 240);
    CHECK (notes[0].pitch == 60);

    REQUIRE (r.rack.toggleStep (kick, 0));
    REQUIRE (r.rack.toggleStep (kick, 4));
    CHECK (r.rack.isStepOn (kick, 0));
    CHECK (! r.rack.isStepOn (kick, 4));
    CHECK (r.session.getHistory().getUndoDescriptions().joinIntoString (" | ").endsWith (
        "Add step | Add step | Remove step"));

    r.session.getHistory().undo();
    CHECK (r.rack.isStepOn (kick, 4));
}

TEST_CASE ("The steps are one bar of the pattern's time signature", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ({}, "Kick"));
    const auto kick = r.lastChannel();
    REQUIRE (r.rack.toggleStep (kick, 0));
    CHECK (r.rack.getNumSteps() == 16);

    // 3/4: twelve steps.
    juce::String error;
    auto threeFour = commands::CommandRegistry::get().create (
        "pattern.setTimeSignature",
        juce::JSON::parse ("{\"pattern\": " + juce::String ((int)r.session.getCurrentPattern().value) +
                           ", \"numerator\": 3, \"denominator\": 4}"),
        error);
    REQUIRE (r.session.getHistory().perform (std::move (threeFour)).wasOk());
    CHECK (r.rack.getNumSteps() == 12);

    // Notes in a later bar (from the Piano Roll) do not add steps: the row shows them as a picture.
    CHECK (! r.rack.hasPianoRollNotes (kick));
    auto later = commands::CommandRegistry::get().create (
        "pattern.setNotes",
        juce::JSON::parse (
            "{\"pattern\": " + juce::String ((int)r.session.getCurrentPattern().value) +
            ", \"channel\": " + juce::String ((int)kick.value) +
            R"(, "notes": [{"start": 0, "length": 240, "pitch": 60}, {"start": 2880, "length": 240, "pitch": 60}]})"),
        error);
    REQUIRE (r.session.getHistory().perform (std::move (later)).wasOk());
    CHECK (r.rack.getNumSteps() == 12);
    CHECK (r.rack.hasPianoRollNotes (kick));
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 2 * 2880);
}

TEST_CASE ("Triplet steps: six per beat, each a sixteenth-note triplet", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ({}, "Hat"));
    const auto hat = r.lastChannel();
    CHECK (! r.rack.hasTripletSteps());

    // No pattern yet: switching makes one, as the first step does.
    REQUIRE (r.rack.setTripletSteps (true));
    REQUIRE (r.session.getCurrentPattern().isValid());
    CHECK (r.rack.hasTripletSteps());
    CHECK (r.rack.getStepTicks() == 160);
    CHECK (r.rack.getNumSteps() == 24);

    REQUIRE (r.rack.toggleStep (hat, 1));
    REQUIRE (r.rack.toggleStep (hat, 23));
    const auto& notes = r.session.getProject().findPattern (r.session.getCurrentPattern())->notes.at (hat);
    REQUIRE (notes.size() == 2);
    CHECK (notes[0].start == 160);
    CHECK (notes[0].length == 160);
    CHECK (notes[1].start == 23 * 160);
    CHECK (! r.rack.hasPianoRollNotes (hat));

    // Back to sixteenths: the notes stay where they are; off the sixteenth grid, they show as a picture.
    REQUIRE (r.rack.setTripletSteps (false));
    CHECK (r.rack.getNumSteps() == 16);
    CHECK (r.rack.hasPianoRollNotes (hat));
    r.session.getHistory().undo();
    CHECK (r.rack.hasTripletSteps());
    CHECK (r.rack.isStepOn (hat, 1));
}

TEST_CASE ("An instrument plugin that cannot be loaded shows as Missing", "[ui][channelrack]")
{
    // E.g. a project made where the plugin is installed, opened where it is not (or a plugin of the system
    // that cannot run in the Flatpak): the channel says so instead of staying silent.
    Rack r;
    model::Device gone;
    gone.kind = model::Device::Kind::plugin;
    gone.plugin = { "VST3", "Gone Synth", "Nobody", "/nonexistent/Gone Synth.vst3", 1234 };
    REQUIRE (r.rack.addChannelWith (gone, "Gone Synth"));
    REQUIRE (r.rack.addChannel ({}, "Empty"));
    r.rack.updateNow();
    const auto& channels = r.session.getProject().channels;
    REQUIRE (channels.size() == 2);
    CHECK (r.rack.isInstrumentMissing (channels[0].id));
    CHECK (! r.rack.isInstrumentMissing (channels[1].id)); // no instrument: nothing is missing
    CHECK (! r.rack.showInstrumentWindow (channels[0].id));
}

TEST_CASE ("Channel mute, volume, pan and delete go through commands", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ({}, "Pad"));
    const auto pad = r.lastChannel();

    REQUIRE (r.rack.setChannelVolume (pad, -6.5));
    REQUIRE (r.rack.setChannelPan (pad, 0.25));
    REQUIRE (r.rack.setChannelMuted (pad, true));
    const auto* channel = r.session.getProject().findChannel (pad);
    REQUIRE (channel != nullptr);
    CHECK (juce::approximatelyEqual (channel->volumeDb, -6.5f));
    CHECK (juce::approximatelyEqual (channel->pan, 0.25f));
    CHECK (channel->muted);

    REQUIRE (r.rack.renameChannel (pad, "Warm pad"));
    REQUIRE (r.rack.setChannelColour (pad, 0xffd65c5c));
    CHECK (r.session.getProject().findChannel (pad)->name == "Warm pad");
    CHECK (r.session.getProject().findChannel (pad)->colour == 0xffd65c5cu);

    REQUIRE (r.rack.removeChannel (pad));
    r.rack.updateNow();
    CHECK (r.rack.getNumRows() == 0);
    r.session.getHistory().undo();
    r.rack.updateNow();
    CHECK (r.rack.getNumRows() == 1);
}

TEST_CASE ("Channel knobs are heard while dragged, one undo step per drag", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ({}, "Pad"));
    const auto pad = r.lastChannel();
    r.rack.updateNow();
    auto findKnob = [&]
    {
        juce::Slider* found = nullptr;
        std::function<void (juce::Component&)> look = [&] (juce::Component& c)
        {
            if (auto* s = dynamic_cast<juce::Slider*> (&c);
                s != nullptr && s->getHelpText().startsWith ("Channel volume"))
                found = s;
            for (auto* child : c.getChildren())
                look (*child);
        };
        look (r.rack);
        return found;
    };
    auto* knob = findKnob();
    REQUIRE (knob != nullptr);

    // A drag: every move reaches the engine, and all of them are one undo step.
    const auto steps = r.session.getHistory().getUndoDescriptions().size();
    auto* volume =
        r.session.getAdapter().findParameter ({ .kind = model::ParameterTarget::Kind::channelVolume, .channel = pad });
    REQUIRE (volume != nullptr);
    knob->onDragStart();
    for (double db : { -3.0, -9.0, -18.0 })
    {
        knob->setValue (db, juce::sendNotificationSync); // as the mouse moves
        r.rack.updateNow();                              // the model changed: the same knob stays (it is being dragged)
        CHECK (findKnob() == knob);
        CHECK (std::abs (tracktion::volumeFaderPositionToDB (volume->getCurrentValue()) - db) < 0.2);
    }
    knob->onDragEnd();
    CHECK (r.session.getHistory().getUndoDescriptions().size() == steps + 1);
    REQUIRE (r.session.getHistory().undo());
    CHECK (juce::approximatelyEqual (r.session.getProject().findChannel (pad)->volumeDb, 0.0f));
}

TEST_CASE ("A Surge XT channel plays its steps in pattern mode", "[ui][channelrack][audio]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("bundled plugins not built");

    Rack r;
    test::RealtimeRunner runner (r.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 4.0, .paceInRealTime = false });

    REQUIRE (r.rack.addChannel ("bundled.surge-xt", "Surge XT"));
    const auto synth = r.lastChannel();
    CHECK (r.session.getAdapter().getMissingDevices().empty());

    for (int step : { 0, 4, 8, 12 })
        REQUIRE (r.rack.toggleStep (synth, step));

    r.session.play();
    runner.start();
    runner.runUntil (2.5);
    runner.stop();
    r.session.stop();

    const auto sr = runner.getParams().sampleRate;
    CHECK (test::checks::rms (runner.getChannel (0), (int64_t)(0.5 * sr), (int64_t)(2.5 * sr)) > 0.005f);
}

TEST_CASE ("Freeze and Unfreeze a channel from the rack; its instrument window stays shut while frozen",
           "[ui][channelrack][freeze]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "Lead"));
    const auto lead = r.lastChannel();
    REQUIRE (r.rack.toggleStep (lead, 0));
    r.session.setMode (Session::Mode::song);

    REQUIRE (r.rack.setChannelFrozen (lead, true));
    CHECK (r.session.getProject().findChannel (lead)->frozen);
    for (int i = 0; i < 600 && r.session.getFreezer().isRendering(); ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    r.rack.updateNow();

    CHECK (! r.rack.showInstrumentWindow (lead));
    REQUIRE (r.rack.setChannelFrozen (lead, false));
    CHECK (! r.session.getProject().findChannel (lead)->frozen);
    CHECK (r.session.getHistory().getUndoDescriptions().contains ("Freeze channel 'Lead'"));
}

TEST_CASE ("Replace instrument: another instrument plays the channel's steps, one undo step", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "3x Osc")); // named after its instrument, as the + menu does
    const auto synth = r.lastChannel();
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "My bass"));
    const auto bass = r.lastChannel();
    REQUIRE (r.rack.toggleStep (synth, 0));
    REQUIRE (r.rack.toggleStep (bass, 4));
    const auto oldDevice = r.session.getProject().findChannel (synth)->instrument.value_or (model::Device {}).id;

    model::Device sampler;
    sampler.stockType = "spinus.sampler";
    REQUIRE (r.rack.replaceInstrument (synth, sampler, "Sampler"));
    REQUIRE (r.rack.replaceInstrument (bass, sampler, "Sampler"));

    const auto& p = r.session.getProject();
    const auto* channel = p.findChannel (synth);
    CHECK (channel->name == "Sampler");
    CHECK (p.findInsert (channel->targetInsert)->name == "Sampler");
    CHECK (channel->instrument.value_or (model::Device {}).stockType == "spinus.sampler");
    CHECK (r.session.getAdapter().getDevicePlugin (channel->instrument.value_or (model::Device {}).id) != nullptr);
    CHECK (r.session.getAdapter().getDevicePlugin (oldDevice) == nullptr);
    CHECK (r.rack.isStepOn (synth, 0));
    CHECK (p.findChannel (bass)->name == "My bass"); // a name of its own stays
    CHECK (r.rack.isStepOn (bass, 4));
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() ==
           "Replace instrument of 'My bass' with Sampler");

    REQUIRE (r.session.getHistory().undo());
    REQUIRE (r.session.getHistory().undo());
    CHECK (r.session.getProject().findChannel (synth)->name == "3x Osc");
    CHECK (r.session.getProject().findChannel (synth)->instrument.value_or (model::Device {}).id == oldDevice);
    CHECK (r.session.getAdapter().getDevicePlugin (oldDevice) != nullptr);
}

TEST_CASE ("Notes steps cannot show turn the row into a picture that opens the Piano Roll", "[ui][channelrack]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ChannelRack rack { session };
    rack.setSize (900, 300);
    REQUIRE (rack.addChannel ({}, "Lead"));
    const auto lead = session.getProject().channels[0].id;
    REQUIRE (rack.toggleStep (lead, 0));
    REQUIRE (rack.toggleStep (lead, 4));
    CHECK (! rack.hasPianoRollNotes (lead)); // steps

    auto setNotes = [&] (const juce::String& notes)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (
            "pattern.setNotes",
            juce::JSON::parse ("{\"pattern\": " + juce::String (session.getCurrentPattern().value) +
                               ", \"channel\": " + juce::String (lead.value) + ", \"notes\": " + notes + "}"),
            error);
        REQUIRE (command != nullptr);
        REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
    };
    setNotes (R"([{"start": 0, "length": 240, "pitch": 64, "velocity": 0.8}])"); // another pitch
    CHECK (rack.hasPianoRollNotes (lead));
    setNotes (R"([{"start": 0, "length": 960, "pitch": 60, "velocity": 0.8}])"); // longer than a step
    CHECK (rack.hasPianoRollNotes (lead));
    setNotes (R"([{"start": 120, "length": 120, "pitch": 60, "velocity": 0.8}])"); // off the grid
    CHECK (rack.hasPianoRollNotes (lead));
    setNotes (R"([{"start": 240, "length": 240, "pitch": 60, "velocity": 0.8}])"); // a plain step again
    CHECK (! rack.hasPianoRollNotes (lead));

    // Clicking the picture selects the channel and opens the Piano Roll.
    setNotes (R"([{"start": 0, "length": 480, "pitch": 67, "velocity": 0.8}])");
    bool opened = false;
    rack.onOpenPianoRoll = [&] { opened = true; };
    CHECK (rack.createComponentSnapshot (rack.getLocalBounds()).isValid()); // draws the picture
    CHECK (rack.openNotes (lead));
    CHECK (opened);
    CHECK (session.getSelectedChannel() == lead);
}

TEST_CASE ("Channel Rack note picture screenshot for the manual", "[.screenshot][channelrack]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ChannelRack rack { session };
    ui::SpinusLookAndFeel lookAndFeel { ui::Theme::dark() };
    rack.setLookAndFeel (&lookAndFeel);
    rack.setSize (760, 150);
    REQUIRE (rack.addChannel ({}, "Kick"));
    REQUIRE (rack.addChannel ({}, "Bass"));
    const auto kick = session.getProject().channels[0].id, bass = session.getProject().channels[1].id;
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (rack.toggleStep (kick, step));
    juce::String error;
    auto command = commands::CommandRegistry::get().create (
        "pattern.setNotes",
        juce::JSON::parse ("{\"pattern\": " + juce::String (session.getCurrentPattern().value) +
                           ", \"channel\": " + juce::String (bass.value) +
                           R"(, "notes": [{"start": 0, "length": 720, "pitch": 36, "velocity": 0.8},
                                          {"start": 960, "length": 240, "pitch": 43, "velocity": 0.8},
                                          {"start": 1440, "length": 480, "pitch": 41, "velocity": 0.8},
                                          {"start": 2400, "length": 960, "pitch": 39, "velocity": 0.8}]})"),
        error);
    REQUIRE (command != nullptr);
    REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    const auto image = rack.createComponentSnapshot (rack.getLocalBounds(), true, 1.0f);
    rack.setLookAndFeel (nullptr);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile ("docs/user/images/channel-rack-notes.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

TEST_CASE ("Swing plays every second step later; the notes stay where they are", "[ui][channelrack]")
{
    Rack r;
    REQUIRE (r.rack.addChannel ("spinus.3x-osc", "Hat"));
    const auto hat = r.lastChannel();
    REQUIRE (r.rack.toggleStep (hat, 0));
    REQUIRE (r.rack.toggleStep (hat, 1));
    REQUIRE (r.rack.toggleStep (hat, 2));

    // The starts (in beats) of the notes the engine plays for the hat.
    auto played = [&]
    {
        r.session.getEdit().dispatchPendingUpdatesSynchronously();
        std::vector<double> starts;
        for (auto* track : tracktion::getAudioTracks (r.session.getEdit()))
            if (track->getName() == "Hat")
                for (auto* c : track->getClips())
                    if (auto* midi = dynamic_cast<tracktion::MidiClip*> (c))
                        for (auto* note : midi->getSequence().getNotes())
                            starts.push_back (note->getStartBeat().inBeats());
        std::sort (starts.begin(), starts.end());
        return starts;
    };
    auto straight = played();
    REQUIRE (straight.size() == 3);
    CHECK (juce::approximatelyEqual (straight[1], 0.25));

    REQUIRE (r.rack.setSwing (1.0));
    CHECK (juce::approximatelyEqual (r.rack.getSwing(), 1.0));
    auto swung = played();
    REQUIRE (swung.size() == 3);
    CHECK (juce::approximatelyEqual (swung[0], 0.0));
    CHECK (juce::approximatelyEqual (swung[1], 0.375)); // half a step later
    CHECK (juce::approximatelyEqual (swung[2], 0.5));   // on the beat's half: not moved
    // The notes themselves didn't move.
    const auto& notes = r.session.getProject().findPattern (r.session.getCurrentPattern())->notes.at (hat);
    CHECK (notes[1].start == 240);

    // A knob drag is one undo step.
    REQUIRE (r.rack.setSwing (0.3));
    REQUIRE (r.rack.setSwing (0.5, true));
    r.session.getHistory().undo();
    CHECK (juce::approximatelyEqual (r.rack.getSwing(), 1.0));
}

TEST_CASE ("Channel groups: the rack shows one group, and a group can play through a bus", "[ui][channelrack][groups]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ChannelRack rack (session);
    rack.setSize (800, 400);
    for (auto name : { "Kick", "Snare", "Bass" })
        REQUIRE (rack.addChannel ({}, name));
    const auto& channels = session.getProject().channels;
    REQUIRE (rack.setChannelGroup (channels[0].id, "Drums"));
    REQUIRE (rack.setChannelGroup (channels[1].id, "Drums"));
    rack.updateNow();
    CHECK (rack.getGroups() == juce::StringArray { "Drums" });

    rack.showGroup ("Drums");
    CHECK (rack.getNumShownChannels() == 2);
    rack.showGroup (ChannelRack::noGroup);
    CHECK (rack.getNumShownChannels() == 1);
    rack.showGroup (ChannelRack::allGroups);
    CHECK (rack.getNumShownChannels() == 3);

    // The drums through a new "Drums" bus, which plays into the master: one undo step.
    const auto inserts = session.getProject().mixer.inserts.size();
    REQUIRE (rack.routeGroupToBus ("Drums"));
    const auto& mixer = session.getProject().mixer;
    REQUIRE (mixer.inserts.size() == inserts + 1);
    // Right after the drums' inserts, named after the group.
    const auto busAt = std::find_if (mixer.inserts.begin(), mixer.inserts.end(),
                                     [] (const model::Insert& i) { return i.name == "Drums"; });
    REQUIRE (busAt != mixer.inserts.end());
    const auto* bus = &*busAt;
    CHECK (busAt - mixer.inserts.begin() == 2);
    CHECK (bus->output == model::masterInsertId);
    CHECK (session.getProject().findInsert (channels[0].targetInsert)->output == bus->id);
    CHECK (session.getProject().findInsert (channels[1].targetInsert)->output == bus->id);
    CHECK (session.getProject().findInsert (channels[2].targetInsert)->output == model::masterInsertId);
    REQUIRE (session.getHistory().undo());
    CHECK (session.getProject().mixer.inserts.size() == inserts);
    CHECK (session.getProject().findInsert (channels[0].targetInsert)->output == model::masterInsertId);

    // The last channel leaving a group: the rack shows everything again.
    rack.showGroup ("Drums");
    REQUIRE (rack.setChannelGroup (channels[0].id, ""));
    REQUIRE (rack.setChannelGroup (channels[1].id, ""));
    rack.updateNow();
    CHECK (rack.getShownGroup() == ChannelRack::allGroups);
    CHECK (rack.getNumShownChannels() == 3);
}
