#include <catch2/catch_test_macros.hpp>

#include "FakeProvider.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/Automixer.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;

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

juce::var perform (Session& session, const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (name << ": " << error);
    REQUIRE (command != nullptr);
    auto* raw = command.get();
    REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
    return raw->getResult();
}

/** A bass and a pad on nearly the same notes, equally loud: masking to fix. */
void makeSong (Session& session)
{
    const auto pattern = perform (session, "pattern.add", R"({"name": "Loop", "length": 7680})")["pattern"];
    for (auto [name, pitch, volume] : { std::tuple { "Bass", 36, 0.0 }, std::tuple { "Pad", 38, 0.0 } })
    {
        const auto made = perform (session, "channel.add",
                                   juce::String (R"({"name": ")") + name +
                                       R"(", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})");
        perform (session, "insert.set",
                 R"({"insert": )" + made["insert"].toString() + R"(, "name": ")" + name + R"(", "volumeDb": )" +
                     juce::String (volume) + "}");
        perform (session, "pattern.setNotes",
                 R"({"pattern": )" + pattern.toString() + R"(, "channel": )" + made["channel"].toString() +
                     R"(, "notes": [{"start": 0, "length": 7000, "pitch": )" + juce::String (pitch) +
                     R"(, "velocity": 1.0}]})");
    }
    const auto lane = perform (session, "lane.add", R"({"name": "Song"})")["lane"];
    perform (session, "clip.addPattern",
             R"({"lane": )" + lane.toString() + R"(, "pattern": )" + pattern.toString() + R"(, "start": 0})");
}
} // namespace

TEST_CASE ("Automixer: measures, plans with reasons, applies as one undo step, compares at matched loudness",
           "[ui][ai][automixer]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-automixer-test-" + juce::Uuid().toString());
    root.createDirectory();
    {
        EngineHost host { { .openAudioDevice = false } };
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, host.getEngine(), { bundledPlugins() });
        Session session (host.getEngine(), std::move (devices));
        ai::AiService service (root, root.getChildFile ("models"));
        Automixer automixer (session, &service);
        automixer.setSize (620, 480);
        makeSong (session);

        // AI off: it says so.
        CHECK (! automixer.analyze (Automixer::Target::streaming));
        CHECK (automixer.getStatusText().contains ("turn on AI"));

        service.setEnabled (true);

        // A bright reference song (noise): the mix of two low parts should move towards it.
        const auto referenceFile = root.getChildFile ("Reference.wav");
        {
            juce::AudioBuffer<float> noise (2, 48000 * 4);
            juce::Random random (5);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < noise.getNumSamples(); ++i)
                    noise.setSample (c, i, 0.2f * (random.nextFloat() * 2.0f - 1.0f));
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (referenceFile);
            auto writer = juce::WavAudioFormat().createWriterFor (
                stream,
                juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (16));
            REQUIRE (writer != nullptr);
            writer->writeFromAudioSampleBuffer (noise, 0, noise.getNumSamples());
        }
        automixer.setReference (referenceFile);
        REQUIRE (automixer.analyze (Automixer::Target::streaming));
        REQUIRE (waitFor ([&] { return ! automixer.isBusy(); }, 120000));
        INFO (automixer.getStatusText());
        const auto& measured = automixer.getMeasurements()["inserts"];
        CHECK ((double)measured["Bass"]["loudness"] > -40.0);
        CHECK (automixer.getMeasurements()["masking"].size() > 0);
        CHECK (automixer.getNote().contains ("no AI connection")); // the rules' plan

        const auto& items = automixer.getItems();
        REQUIRE (! items.empty());
        auto has = [&] (const juce::String& kind, const juce::String& insert)
        {
            return std::any_of (items.begin(), items.end(),
                                [&] (auto& i) { return i.json["kind"] == kind && i.json["insert"] == insert; });
        };
        CHECK ((has ("eq", "Pad") || has ("eq", "Bass")));
        CHECK (has ("limiter", "Master"));
        CHECK (has ("eq", "Master")); // towards the reference's tone

        // Untick all but the EQ and the limiter; the rest becomes one proposal.
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].json["kind"] != "eq" && items[i].json["kind"] != "limiter")
                automixer.setTicked ((int)i, false);
        auto proposal = automixer.makeProposal();
        INFO (proposal.error);
        REQUIRE (proposal.canApply());
        CHECK (std::none_of (proposal.steps.begin(), proposal.steps.end(),
                             [] (auto& s) { return s.command == "insert.set"; }));

        const auto steps = session.getHistory().getUndoDescriptions().size();
        REQUIRE (automixer.apply());
        REQUIRE (waitFor ([&] { return ! automixer.isBusy(); }, 120000));
        INFO (automixer.getStatusText());
        REQUIRE (automixer.canCompare());
        CHECK (session.getHistory().getUndoDescriptions().size() == steps + 1);
        CHECK (session.getHistory().getUndoDescriptions().strings.getLast().startsWith ("Automixer: "));
        const auto& masterEffects = session.getProject().mixer.master.effects;
        REQUIRE (! masterEffects.empty());
        CHECK (masterEffects.back().device.stockType == "spinus.limiter");
        CHECK (masterEffects.front().device.stockType == "spinus.eq"); // the tone before the limiter
        CHECK (masterEffects.back().device.parameters.size() == 2);
        const auto eqs =
            std::count_if (session.getProject().mixer.inserts.begin(), session.getProject().mixer.inserts.end(),
                           [] (auto& insert)
                           {
                               return ! insert.effects.empty() &&
                                      insert.effects.back().device.stockType == "spinus.eq" &&
                                      insert.effects.back().device.parameters.size() >= 4;
                           });
        CHECK (eqs >= 1);

        // A/B: before is the project without the changes, after with them, equally loud.
        const auto trim = automixer.getMatchTrim();
        CHECK (std::abs (trim) > 0.5f); // the limiter changed the loudness; while comparing it is undone
        CHECK (juce::approximatelyEqual (session.getAdapter().getMonitorTrim(), trim));
        REQUIRE (automixer.listen (true));
        CHECK (session.getProject().mixer.master.effects.empty());
        CHECK (juce::approximatelyEqual (session.getAdapter().getMonitorTrim(), 0.0f));
        REQUIRE (automixer.listen (false));
        CHECK (! session.getProject().mixer.master.effects.empty());
        automixer.keep();
        CHECK (juce::approximatelyEqual (session.getAdapter().getMonitorTrim(), 0.0f));
        CHECK (! automixer.canCompare());
        service.setEnabled (false);
    }
    root.deleteRecursively();
}

TEST_CASE ("Automixer screenshot for the manual", "[.screenshot][automixer]")
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-automixer-shot-" + juce::Uuid().toString());
    root.createDirectory();
    {
        EngineHost host { { .openAudioDevice = false } };
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, host.getEngine(), { bundledPlugins() });
        Session session (host.getEngine(), std::move (devices));
        ai::AiService service (root, root.getChildFile ("models"));
        service.setEnabled (true);
        SpinusLookAndFeel lookAndFeel { Theme::dark() };
        Automixer automixer (session, &service);
        automixer.setLookAndFeel (&lookAndFeel);
        automixer.setSize (620, 480);
        makeSong (session);
        REQUIRE (automixer.analyze (Automixer::Target::streaming));
        REQUIRE (waitFor ([&] { return ! automixer.isBusy(); }, 120000));
        const auto image = automixer.createComponentSnapshot (automixer.getLocalBounds(), true, 1.0f);
        automixer.setLookAndFeel (nullptr);
        const auto file =
            juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/automixer.png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
        service.setEnabled (false);
    }
    root.deleteRecursively();
}
