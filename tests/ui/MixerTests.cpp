#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"
#include "ui/RoutingView.h"
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

struct MixerFixture
{
    MixerFixture() : session (host.getEngine(), makeDevices (host.getEngine())), mixer (session)
    {
        mixer.setSize (900, 420);
    }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
        return devices;
    }

    const model::Project& project() const { return session.getProject(); }
    const model::Insert& insert (model::InsertId id) const { return *project().findInsert (id); }

    EngineHost host { { .openAudioDevice = false } };
    Session session;
    Mixer mixer;
};

const auto compressor = "bundled.lsp.compressor";
const auto reverb = "bundled.dragonfly.room";
} // namespace

TEST_CASE ("The mixer starts with Master and a + strip; + adds and selects an insert", "[ui][mixer]")
{
    MixerFixture f;
    CHECK (f.mixer.getNumStripComponents() == 1);
    CHECK (f.mixer.getSelectedInsert() == model::masterInsertId);

    REQUIRE (f.mixer.addInsert());
    f.mixer.updateNow();
    REQUIRE (f.project().mixer.inserts.size() == 1);
    const auto id = f.project().mixer.inserts[0].id;
    CHECK (f.mixer.getSelectedInsert() == id);
    CHECK (f.mixer.getNumStripComponents() == 2);
    CHECK (f.session.getAdapter().getInsertLevels (id) != nullptr); // its meter has a source

    REQUIRE (f.mixer.renameInsert (id, "Drums"));
    CHECK (f.insert (id).name == "Drums");
    REQUIRE (f.mixer.setInsertColour (id, 0xffe0894a));
    CHECK (f.insert (id).colour == 0xffe0894au);
}

TEST_CASE ("A fader drag is heard as it moves and is one undo step", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    const auto id = f.project().mixer.inserts[0].id;
    const auto steps = f.session.getHistory().getUndoDescriptions().size();

    REQUIRE (f.mixer.setInsertVolume (id, -3.0));
    REQUIRE (f.mixer.setInsertVolume (id, -6.0, true));
    REQUIRE (f.mixer.setInsertVolume (id, -9.0, true));
    CHECK (juce::approximatelyEqual (f.insert (id).volumeDb, -9.0f));
    CHECK (
        juce::approximatelyEqual (f.session.getAdapter().getInsertTrack (id)->getVolumePlugin()->getVolumeDb(), -9.0f));
    CHECK (f.session.getHistory().getUndoDescriptions().size() == steps + 1);

    REQUIRE (f.session.getHistory().undo());
    CHECK (juce::approximatelyEqual (f.insert (id).volumeDb, 0.0f));

    REQUIRE (f.mixer.setInsertPan (id, 0.5));
    REQUIRE (f.mixer.setInsertMuted (id, true));
    REQUIRE (f.mixer.setInsertSolo (id, true));
    CHECK (juce::approximatelyEqual (f.insert (id).pan, 0.5f));
    CHECK (f.insert (id).muted);
    CHECK (f.insert (id).solo);
}

TEST_CASE ("Effect slots: add, reorder, bypass, move to another insert, remove", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    REQUIRE (f.mixer.addInsert());
    const auto a = f.project().mixer.inserts[0].id;
    const auto b = f.project().mixer.inserts[1].id;

    REQUIRE (f.mixer.addEffect (a, compressor));
    REQUIRE (f.mixer.addEffect (a, reverb, 0)); // into the first slot
    REQUIRE (f.insert (a).effects.size() == 2);
    const auto reverbId = f.insert (a).effects[0].device.id;
    const auto compressorId = f.insert (a).effects[1].device.id;
    CHECK (f.insert (a).effects[0].device.stockType == reverb);

    // A slot beyond the last effect just appends.
    REQUIRE (f.mixer.addEffect (a, compressor, 9));
    CHECK (f.insert (a).effects.size() == 3);
    REQUIRE (f.mixer.removeEffect (f.insert (a).effects[2].device.id));

    REQUIRE (f.mixer.moveEffect (compressorId, a, 0));
    CHECK (f.insert (a).effects[0].device.id == compressorId);

    REQUIRE (f.mixer.setEffectBypassed (reverbId, true));
    CHECK (f.insert (a).effects[1].device.bypassed);

    REQUIRE (f.mixer.moveEffect (reverbId, b, 0));
    CHECK (f.insert (a).effects.size() == 1);
    CHECK (f.insert (b).effects[0].device.id == reverbId);

    REQUIRE (f.mixer.removeEffect (compressorId));
    CHECK (f.insert (a).effects.empty());
}

TEST_CASE ("An insert takes any number of effects; the slot list grows", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    const auto a = f.project().mixer.inserts[0].id;
    f.mixer.selectInsert (a);
    CHECK (f.mixer.getNumShownSlots() == 10);

    for (int i = 0; i < 14; ++i)
        REQUIRE (f.mixer.addEffect (a, compressor));
    f.mixer.updateNow();
    CHECK (f.insert (a).effects.size() == 14);
    CHECK (f.mixer.getNumShownSlots() == 15); // always one empty slot after the last effect
    // The engine takes them all: none is missing.
    const auto missing = f.session.getAdapter().getMissingDevices();
    for (auto& slot : f.insert (a).effects)
        if (std::find (missing.begin(), missing.end(), slot.device.id) == missing.end())
            CHECK (f.session.getAdapter().getDevicePlugin (slot.device.id) != nullptr);
}

TEST_CASE ("Routing: output, sends, and refused feedback loops", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    REQUIRE (f.mixer.addInsert());
    const auto drums = f.project().mixer.inserts[0].id;
    const auto bus = f.project().mixer.inserts[1].id;

    REQUIRE (f.mixer.setOutput (drums, bus));
    CHECK (f.insert (drums).output == bus);

    CHECK (! f.mixer.setOutput (bus, drums));
    CHECK (f.mixer.getLastError().contains ("feedback loop"));
    CHECK (f.insert (bus).output == model::masterInsertId);

    REQUIRE (f.mixer.setOutput (drums, model::masterInsertId));
    CHECK (f.mixer.getLastError().isEmpty());
    REQUIRE (f.mixer.setSend (drums, bus, -6.0));
    REQUIRE (f.mixer.setSend (drums, bus, -9.0, true));
    REQUIRE (f.insert (drums).sends.size() == 1);
    CHECK (juce::approximatelyEqual (f.insert (drums).sends[0].levelDb, -9.0f));
    REQUIRE (f.mixer.removeSend (drums, bus));
    CHECK (f.insert (drums).sends.empty());
}

TEST_CASE ("A strip says where a sidechain comes from and where its sound goes", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    REQUIRE (f.mixer.addInsert());
    REQUIRE (f.mixer.addInsert());
    const auto kick = f.project().mixer.inserts[0].id;
    const auto bass = f.project().mixer.inserts[1].id;
    const auto bus = f.project().mixer.inserts[2].id;
    REQUIRE (f.mixer.renameInsert (kick, "Kick"));
    REQUIRE (f.mixer.renameInsert (bus, "Bus"));
    f.mixer.updateNow();
    CHECK (! f.mixer.getStripHelpText (bass).contains ("sidechain"));

    REQUIRE (f.mixer.setSidechain (bass, kick));
    REQUIRE (f.mixer.setOutput (bass, bus));
    f.mixer.updateNow();
    CHECK (f.mixer.getStripHelpText (bass).contains ("sidechain from Kick"));
    CHECK (f.mixer.getStripHelpText (bass).contains ("goes to Bus instead of the Master"));
    CHECK (! f.mixer.getStripHelpText (kick).contains ("sidechain"));

    REQUIRE (f.mixer.setSend (kick, bus, -6.0));
    f.mixer.updateNow();
    CHECK (f.mixer.getStripHelpText (kick).contains ("sends to Bus"));
}

TEST_CASE ("Remove unused inserts is one undo step and keeps inserts in use", "[ui][mixer]")
{
    MixerFixture f;
    for (int i = 0; i < 4; ++i)
        REQUIRE (f.mixer.addInsert());
    const auto inserts = f.project().mixer.inserts;

    REQUIRE (f.mixer.addEffect (inserts[0].id, compressor));    // has an effect
    REQUIRE (f.mixer.setOutput (inserts[2].id, inserts[1].id)); // 1 is fed by 2; 2 has no input
    const auto steps = f.session.getHistory().getUndoDescriptions().size();

    CHECK (f.mixer.findUnusedInserts() == std::vector { inserts[2].id, inserts[3].id });
    REQUIRE (f.mixer.removeUnusedInserts());
    CHECK (f.project().mixer.inserts.size() == 2);
    CHECK (f.session.getHistory().getUndoDescriptions().size() == steps + 1);
    CHECK (f.session.getHistory().getUndoDescriptions()[(int)steps] == "Remove unused inserts");

    REQUIRE (f.session.getHistory().undo());
    CHECK (f.project().mixer.inserts.size() == 4);
}

TEST_CASE ("Only visible strips exist, even with 500 inserts", "[ui][mixer]")
{
    MixerFixture f;
    auto transaction = std::make_unique<commands::Transaction> ("Add 500 inserts");
    for (int i = 0; i < 500; ++i)
    {
        juce::String error;
        transaction->add (commands::CommandRegistry::get().create ("insert.add", juce::JSON::parse ("{}"), error));
    }
    REQUIRE (f.session.getHistory().perform (std::move (transaction)).wasOk());
    f.mixer.updateNow();

    const auto visible = f.mixer.getNumStripComponents();
    CHECK (visible > 4);
    CHECK (visible < 15);

    f.mixer.scrollToStrip (400);
    CHECK (f.mixer.getNumStripComponents() <= visible + 1);
    REQUIRE (f.mixer.moveInsert (f.project().mixer.inserts[399].id, 0));
}

TEST_CASE ("Freeze an insert from the mixer; one that cannot be frozen says why", "[ui][mixer][freeze]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    REQUIRE (f.mixer.addInsert());
    const auto a = f.project().mixer.inserts[0].id, b = f.project().mixer.inserts[1].id;

    REQUIRE (f.mixer.setInsertFrozen (a, true));
    CHECK (f.insert (a).frozen);
    REQUIRE (f.mixer.setInsertFrozen (a, false));

    // A sends to B and goes to the master: freezing B would lose A's sound in the master.
    REQUIRE (f.mixer.setSend (a, b, -6.0));
    CHECK (! f.mixer.setInsertFrozen (b, true));
    CHECK (f.mixer.getLastError().contains ("cannot be frozen"));
    CHECK (! f.mixer.setInsertFrozen (model::masterInsertId, true));
}

TEST_CASE ("An effect is copied to another insert with its settings, in one undo step", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer.addInsert());
    REQUIRE (f.mixer.addInsert());
    const auto a = f.project().mixer.inserts[0].id, b = f.project().mixer.inserts[1].id;
    REQUIRE (f.mixer.addEffect (a, "spinus.filter"));
    const auto original = f.insert (a).effects[0].device.id;

    // A setting changed in the effect's window (not through a command).
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (f.session.getAdapter().getDevicePlugin (original));
    REQUIRE (plugin != nullptr);
    auto* parameter = plugin->getAudioPluginInstance()->getParameters()[0];
    parameter->setValueNotifyingHost (0.123f);
    const auto changed = parameter->getValue();

    const auto steps = f.session.getHistory().getUndoDescriptions().size();
    REQUIRE (f.mixer.copyEffect (original, b));
    REQUIRE (f.insert (b).effects.size() == 1);
    const auto copy = f.insert (b).effects[0].device;
    CHECK (copy.stockType == "spinus.filter");
    CHECK (copy.id != original);
    auto* copied = dynamic_cast<tracktion::ExternalPlugin*> (f.session.getAdapter().getDevicePlugin (copy.id));
    REQUIRE (copied != nullptr);
    CHECK (juce::approximatelyEqual (copied->getAudioPluginInstance()->getParameters()[0]->getValue(), changed));
    CHECK (f.insert (a).effects.size() == 1); // the original stays
    CHECK (f.session.getHistory().getUndoDescriptions().size() == steps + 1);

    f.session.getHistory().undo();
    CHECK (f.insert (b).effects.empty());
}

TEST_CASE ("Mixer screenshot for the manual", "[.screenshot][mixer]")
{
    MixerFixture f;
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    f.mixer.setLookAndFeel (&lookAndFeel);
    f.mixer.setSize (900, 420);
    for (auto name : { "Kick", "Bass", "Lead", "Drums", "Reverb" })
    {
        REQUIRE (f.mixer.addInsert());
        REQUIRE (f.mixer.renameInsert (f.project().mixer.inserts.back().id, name));
    }
    const auto& inserts = f.project().mixer.inserts;
    const auto kick = inserts[0].id, bass = inserts[1].id, lead = inserts[2].id, drums = inserts[3].id,
               reverbBus = inserts[4].id;
    REQUIRE (f.mixer.setInsertColour (kick, 0xffe0894a));
    REQUIRE (f.mixer.setInsertColour (bass, 0xff4a90e0));
    REQUIRE (f.mixer.setInsertColour (drums, 0xffe0894a));
    REQUIRE (f.mixer.setOutput (kick, drums));
    REQUIRE (f.mixer.addEffect (bass, "spinus.ducker"));
    REQUIRE (f.mixer.setSidechain (bass, kick));
    REQUIRE (f.mixer.addEffect (lead, "bundled.lsp.eq"));
    REQUIRE (f.mixer.addEffect (lead, compressor));
    REQUIRE (f.mixer.setSend (lead, reverbBus, -9.0));
    REQUIRE (f.mixer.addEffect (reverbBus, reverb));
    REQUIRE (f.mixer.setInsertVolume (bass, -4.0, false));
    f.mixer.selectInsert (lead);
    f.mixer.updateNow();
    const auto image = f.mixer.createComponentSnapshot (f.mixer.getLocalBounds(), true, 1.0f);
    f.mixer.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/mixer.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

namespace
{
/** Kick and Snare into a Drums bus; the Bass ducked by the Kick; the Lead sending to a Reverb. */
void makeRoutedSong (MixerFixture& f)
{
    ChannelRack rack (f.session);
    for (auto name : { "Kick", "Snare", "Bass", "Lead" })
        REQUIRE (rack.addChannel ("spinus.3x-osc", name));
    const auto insertOf = [&f] (int channel) { return f.project().channels[(size_t)channel].targetInsert; };
    REQUIRE (f.mixer.addInsert());
    const auto drums = f.project().mixer.inserts.back().id;
    REQUIRE (f.mixer.renameInsert (drums, "Drums"));
    REQUIRE (f.mixer.addInsert());
    const auto reverbBus = f.project().mixer.inserts.back().id;
    REQUIRE (f.mixer.renameInsert (reverbBus, "Reverb"));
    REQUIRE (f.mixer.setOutput (insertOf (0), drums));
    REQUIRE (f.mixer.setOutput (insertOf (1), drums));
    REQUIRE (f.mixer.setSidechain (insertOf (2), insertOf (0)));
    REQUIRE (f.mixer.setSend (insertOf (3), reverbBus, -8.0));
}
} // namespace

TEST_CASE ("The Routing panel draws every connection, and a click selects the insert", "[ui][mixer][routing]")
{
    MixerFixture f;
    RoutingView routing (f.session);
    routing.setSize (860, 480);
    makeRoutedSong (f);
    routing.updateNow();

    const auto& nodes = routing.getNodes();
    auto nodeNamed = [&nodes] (const juce::String& name) -> const RoutingView::Node&
    {
        for (const auto& n : nodes)
            if (n.name == name || n.name.endsWith (" " + name))
                return n;
        FAIL ("no node " << name);
        return nodes[0];
    };
    // 4 channels, 6 inserts, the Master.
    CHECK (nodes.size() == 11);
    // Columns: channels, then Kick and Snare (two steps from the Master), then Drums with the others, then the Master.
    CHECK (nodeNamed ("Master").column == 3);
    CHECK (nodeNamed ("Drums").column == 2);
    CHECK (std::count_if (nodes.begin(), nodes.end(), [] (auto& n)
                          { return n.kind == RoutingView::Node::Kind::channel && n.name == "Bass"; }) == 1);

    int outputs = 0, sends = 0, sidechains = 0;
    for (const auto& e : routing.getEdges())
    {
        outputs += e.kind == RoutingView::Edge::Kind::output ? 1 : 0;
        sends += e.kind == RoutingView::Edge::Kind::send ? 1 : 0;
        if (e.kind == RoutingView::Edge::Kind::sidechain)
        {
            ++sidechains;
            CHECK (nodes[e.from].name.endsWith ("Kick"));
            CHECK (nodes[e.to].name.endsWith ("Bass"));
        }
        if (e.kind == RoutingView::Edge::Kind::send)
            CHECK (e.label == "-8.0 dB");
    }
    CHECK (outputs == 4 + 6); // each channel into its insert; each insert into its output

    // No line passes behind a box it does not connect: the Bass and Lead channels' lines go round the Kick
    // and Snare inserts in the column between.
    for (const auto& e : routing.getEdges())
    {
        const auto path = routing.getEdgePath (e);
        for (int step = 0; (float)step * 2.0f < path.getLength(); ++step)
            for (size_t i = 0; i < nodes.size(); ++i)
                if (i != e.from && i != e.to)
                {
                    INFO (nodes[e.from].name << " -> " << nodes[e.to].name << " behind " << nodes[i].name);
                    REQUIRE (! nodes[i].box.contains (path.getPointAlongPath ((float)step * 2.0f)));
                }
    }
    CHECK (sends == 1);
    CHECK (sidechains == 1);

    model::InsertId clicked;
    routing.onInsertClicked = [&clicked] (model::InsertId id) { clicked = id; };
    const auto& drums = nodeNamed ("Drums");
    REQUIRE (routing.nodeAt (drums.box.getCentre()).has_value());
    auto* canvas = routing.getChildComponent (0)->getChildComponent (0)->getChildComponent (0);
    REQUIRE (canvas != nullptr);
    canvas->mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), drums.box.getCentre(), {},
                                         1.0f, 0.0f, 0.0f, 0.0f, 0.0f, canvas, canvas, juce::Time::getCurrentTime(),
                                         drums.box.getCentre(), juce::Time::getCurrentTime(), 1, false));
    CHECK (clicked.value == drums.id);
}

TEST_CASE ("Routing panel screenshot for the manual", "[.screenshot][routing]")
{
    MixerFixture f;
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    RoutingView routing (f.session);
    routing.setLookAndFeel (&lookAndFeel);
    routing.setSize (860, 300);
    makeRoutedSong (f);
    routing.updateNow();
    const auto image = routing.createComponentSnapshot (routing.getLocalBounds(), true, 1.0f);
    routing.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/routing.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
