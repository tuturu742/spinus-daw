#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/StockDevices.h"
#include "ui/AutomationEditor.h"
#include "ui/ChannelRack.h"
#include "ui/MainView.h"
#include "ui/Mixer.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::ui;

// An automation clip shows what it moves: selected in the Playlist (or shown in the Automation editor), its
// target is outlined in the Mixer and the Channel Rack, and its menu has "Show ...".
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
        mixer = std::make_unique<Mixer> (*session);
        mixer->setSize (900, 420);
        playlist = std::make_unique<Playlist> (*session);
        playlist->setSize (900, 400);
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Lead"));
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Bass"));
        REQUIRE (mixer->addEffect (model::masterInsertId, "spinus.filter"));
    }

    ~Fixture()
    {
        playlist = nullptr;
        mixer = nullptr;
        rack = nullptr;
        session = nullptr;
    }

    const model::Channel& channel (int i) const { return session->getProject().channels[(size_t)i]; }
    const model::Device& filter() const { return session->getProject().mixer.master.effects.front().device; }

    model::ParameterTarget filterCutoff() const
    {
        model::ParameterTarget t;
        t.kind = Kind::deviceParameter;
        t.device = filter().id;
        t.parameterId = "cutoff";
        return t;
    }

    model::ClipId clipFor (const model::ParameterTarget& t)
    {
        const auto id = session->createAutomationClip (t);
        REQUIRE (id.isValid());
        return id;
    }

    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<Mixer> mixer;
    std::unique_ptr<Playlist> playlist;
};
} // namespace

TEST_CASE ("Selecting an automation clip outlines the effect it moves in the Mixer", "[ui][automation]")
{
    Fixture f;
    const auto clip = f.clipFor (f.filterCutoff());
    const auto lead = f.channel (0);
    CHECK (f.session->getHighlightedTargets().empty());

    f.playlist->setSelection ({ clip });
    REQUIRE (f.session->getHighlightedTargets().size() == 1);
    CHECK (f.session->getHighlightedTargets().front() == f.filterCutoff());
    CHECK (f.mixer->isStripHighlighted (model::masterInsertId));
    CHECK (! f.mixer->isStripHighlighted (lead.targetInsert));
    f.mixer->selectInsert (model::masterInsertId);
    CHECK (f.mixer->isEffectHighlighted (f.filter().id));
    CHECK (! f.rack->isChannelHighlighted (lead.id));

    // The label names the device, where it is and the control.
    CHECK (f.session->describeTargetPlace (f.filterCutoff()) ==
           model::getDeviceDisplayName (f.filter()) + " (Master): Cutoff");

    // No longer selected: nothing is outlined.
    f.playlist->clearSelection();
    CHECK (f.session->getHighlightedTargets().empty());
    CHECK (! f.mixer->isStripHighlighted (model::masterInsertId));
    CHECK (! f.mixer->isEffectHighlighted (f.filter().id));
}

TEST_CASE ("Selecting an automation clip outlines the channel or strip it moves", "[ui][automation]")
{
    Fixture f;
    const auto lead = f.channel (0), bass = f.channel (1);

    // A channel's volume: its Channel Rack row.
    model::ParameterTarget volume;
    volume.kind = Kind::channelVolume;
    volume.channel = bass.id;
    const auto volumeClip = f.clipFor (volume);
    f.playlist->setSelection ({ volumeClip });
    CHECK (f.rack->isChannelHighlighted (bass.id));
    CHECK (! f.rack->isChannelHighlighted (lead.id));
    CHECK (! f.mixer->isStripHighlighted (bass.targetInsert));
    CHECK (f.session->describeTargetPlace (volume) == "Bass: Volume");

    // An instrument's control: the channel playing it.
    model::ParameterTarget cutoff;
    cutoff.kind = Kind::deviceParameter;
    cutoff.device = lead.instrument.value_or (model::Device {}).id;
    cutoff.parameterId = "cutoff";
    const auto cutoffClip = f.clipFor (cutoff);
    f.playlist->setSelection ({ cutoffClip });
    CHECK (f.rack->isChannelHighlighted (lead.id));
    CHECK (! f.rack->isChannelHighlighted (bass.id));

    // An insert's pan: its strip. Several clips selected: each target.
    model::ParameterTarget pan;
    pan.kind = Kind::insertPan;
    pan.insert = bass.targetInsert;
    const auto panClip = f.clipFor (pan);
    f.playlist->setSelection ({ panClip, cutoffClip });
    CHECK (f.mixer->isStripHighlighted (bass.targetInsert));
    CHECK (! f.mixer->isStripHighlighted (lead.targetInsert));
    CHECK (f.rack->isChannelHighlighted (lead.id));
    CHECK (f.session->describeTargetPlace (pan) == "2 Bass: Pan");

    // The clip deleted: its target is no longer outlined.
    REQUIRE (f.playlist->deleteClip (panClip));
    f.playlist->handleUpdateNowIfNeeded();
    CHECK (! f.mixer->isStripHighlighted (bass.targetInsert));
    CHECK (f.rack->isChannelHighlighted (lead.id));
}

TEST_CASE ("The Automation editor outlines its clip's target while it is shown", "[ui][automation]")
{
    Fixture f;
    const auto clip = f.clipFor (f.filterCutoff());
    {
        AutomationEditor editor (*f.session);
        editor.setSize (800, 330);
        editor.setVisible (true);
        editor.setClip (clip);
        CHECK (f.mixer->isStripHighlighted (model::masterInsertId));

        // Still outlined when the Playlist's selection changes.
        f.playlist->clearSelection();
        CHECK (f.mixer->isStripHighlighted (model::masterInsertId));

        editor.setVisible (false); // closed
        CHECK (! f.mixer->isStripHighlighted (model::masterInsertId));
        editor.setVisible (true);
        CHECK (f.mixer->isStripHighlighted (model::masterInsertId));
    }
    CHECK (f.session->getHighlightedTargets().empty());
}

TEST_CASE ("An automation clip's menu shows what it moves", "[ui][automation]")
{
    Fixture f;
    const auto filterClip = f.clipFor (f.filterCutoff());
    CHECK (f.playlist->getShowTargetText (filterClip) ==
           "Show " + model::getDeviceDisplayName (f.filter()) + " on Master");

    model::ParameterTarget volume;
    volume.kind = Kind::insertVolume;
    volume.insert = f.channel (1).targetInsert;
    CHECK (f.playlist->getShowTargetText (f.clipFor (volume)) == "Show 2 Bass in the Mixer");

    model::ParameterTarget pan;
    pan.kind = Kind::channelPan;
    pan.channel = f.channel (0).id;
    CHECK (f.playlist->getShowTargetText (f.clipFor (pan)) == "Show Lead in the Channel Rack");

    // Choosing it selects the clip and asks for its target to be shown.
    std::optional<model::ParameterTarget> shown;
    f.playlist->onShowAutomationTarget = [&] (const model::ParameterTarget& t) { shown = t; };
    REQUIRE (f.playlist->showAutomationTarget (filterClip));
    CHECK (shown == f.filterCutoff());
    CHECK (f.playlist->getSelection() == std::set<model::ClipId> { filterClip });

    // A pattern clip has no such item.
    CHECK (f.playlist->getShowTargetText (model::ClipId { 9999 }).isEmpty());
}

TEST_CASE ("Show opens the Mixer at the insert, or the Channel Rack at the channel", "[ui][automation]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session { host.getEngine(), std::move (devices) };
    MainView view { session };
    auto& workspace = view.getWorkspace();
    auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
    auto* mixer = dynamic_cast<Mixer*> (workspace.getPanel ("mixer"));
    REQUIRE (rack != nullptr);
    REQUIRE (mixer != nullptr);
    REQUIRE (rack->addChannel ("spinus.3x-osc", "Lead"));
    REQUIRE (rack->addChannel ("spinus.3x-osc", "Bass"));
    const auto lead = session.getProject().channels[0], bass = session.getProject().channels[1];

    model::ParameterTarget volume;
    volume.kind = Kind::insertVolume;
    volume.insert = bass.targetInsert;
    workspace.hidePanel ("mixer");
    view.showAutomationTarget (volume);
    CHECK (workspace.isPanelVisible ("mixer"));
    CHECK (mixer->getSelectedInsert() == bass.targetInsert);

    model::ParameterTarget pan;
    pan.kind = Kind::channelPan;
    pan.channel = lead.id;
    session.setSelectedChannel (bass.id);
    workspace.hidePanel ("channelRack");
    view.showAutomationTarget (pan);
    CHECK (workspace.isPanelVisible ("channelRack"));
    CHECK (session.getSelectedChannel() == lead.id);
}
