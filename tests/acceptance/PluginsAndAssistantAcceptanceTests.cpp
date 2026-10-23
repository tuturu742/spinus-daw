#include "Acceptance.h"
#include <catch2/catch_approx.hpp>
#include "FakeProvider.h"
#include "dsp/ducker/DuckerProcessor.h"
#include "dsp/slicer/BeatSlicerProcessor.h"
#include "session/SlicesToPattern.h"
#include "ui/AiSettingsDialog.h"
#include "ui/AssistantPanel.h"
#include "plugins/SurgePresets.h"
#include "plugins/clap/ClapPluginFormat.h"

/** The plugins and assistant acceptance criteria in one song, done through the panels: the new
    effects (Filter with automation, a Ducker following the kick, Stereo Utility, more than ten effects on
    one insert), a Beat Slicer loop written out as notes, per-note pan and volume, a Surge XT factory
    preset; exported, saved, opened again and sounding the same, all with AI off.
    Then AI is turned on with a local model and its proposal is applied as one undo step.

    Windows plugins need Wine and a real installer: that check is tools/owner-checks/windows-plugins.sh. */

#if defined(SPINUS_PLUGINS_DIR)

using namespace spinus;
using namespace spinus::acceptance;
namespace te = tracktion;
using Lane = ui::PianoRoll::LaneMode;

namespace
{
/** Two bars at 120 BPM with a short noise hit on every beat. */
juce::AudioBuffer<float> drumLoop()
{
    juce::AudioBuffer<float> audio (1, (int)(4.0 * rate));
    audio.clear();
    juce::Random random (1);
    for (int beat = 0; beat < 8; ++beat)
        for (int i = 0; i < (int)(0.08 * rate); ++i)
            audio.setSample (0, (int)(beat * 0.5 * rate) + i,
                             0.8f * (random.nextFloat() * 2.0f - 1.0f) * std::exp (-(float)i / (0.02f * (float)rate)));
    return audio;
}

template <typename Processor> Processor* processorOf (Session& session, model::DeviceId device)
{
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    return plugin != nullptr ? dynamic_cast<Processor*> (plugin->getAudioPluginInstance()) : nullptr;
}

float rmsOf (const Render& r, int channel, double from, double to)
{
    const auto start = (int)(from * rate), end = juce::jmin (r.audio.getNumSamples(), (int)(to * rate));
    double sum = 0.0;
    for (int i = start; i < end; ++i)
        sum += (double)r.audio.getSample (channel, i) * r.audio.getSample (channel, i);
    return end > start ? (float)std::sqrt (sum / (end - start)) : 0.0f;
}

model::DeviceId instrumentOf (const model::Channel& c) { return c.instrument.value_or (model::Device {}).id; }

juce::File stemOf (const std::vector<juce::File>& files, const juce::String& insertName)
{
    for (size_t i = 1; i < files.size(); ++i)
        if (files[i].getFileNameWithoutExtension().endsWith (insertName))
            return files[i];
    FAIL ("no stem for " << insertName);
    return {};
}

/** The loaded patch's name from Surge XT's saved state (binary, with the patch as XML inside:
    <meta name="..." category=...>). */
juce::String surgePatchName (juce::AudioPluginInstance& surge)
{
    juce::MemoryBlock state;
    surge.getStateInformation (state);
    const std::string_view data (static_cast<const char*> (state.getData()), state.getSize());
    const std::string_view tag = "<meta name=\"";
    const auto start = data.find (tag);
    if (start == std::string_view::npos)
        return {};
    const auto end = data.find ('"', start + tag.size());
    if (end == std::string_view::npos)
        return {};
    return juce::String::fromUTF8 (data.data() + start + tag.size(), (int)(end - start - tag.size()));
}
} // namespace

TEST_CASE ("Plugins and assistant acceptance: new devices, note expressions, Surge's panel and the assistant",
           "[acceptance][plugins]")
{
    Fixture f ("spinus-plugins-acceptance");
    auto& history = f.session->getHistory();
    const auto& project = f.session->getProject();

    // 6. AI is off: everything up to the assistant works without the AI service.
    ai::AiService service (f.root.getChildFile ("settings"));
    CHECK (! service.isEnabled());

    // 3. Beat Slicer: a drum loop, sliced at its hits and written out as notes (a new two-bar pattern).
    REQUIRE (f.rack->addChannel ("spinus.beat-slicer", "Break"));
    const auto breakChannel = project.channels.back().id;
    auto* slicer = processorOf<dsp::BeatSlicerProcessor> (*f.session, instrumentOf (project.channels.back()));
    REQUIRE (slicer != nullptr);
    slicer->setLoop (drumLoop(), rate, "Break");
    CHECK (hasSlices (*f.session, breakChannel));
    REQUIRE (slicesToPattern (*f.session, breakChannel).wasOk());
    CHECK (project.findPattern (f.session->getCurrentPattern())->notes.at (breakChannel).size() == 8);

    // A kick, a long bass note and a lead whose notes have their own pan and volume (5. note lanes).
    REQUIRE (f.rack->addChannel ("spinus.drum-machine", "Kick"));
    const auto kick = project.channels.back().id;
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (f.rack->toggleStep (kick, step));
    REQUIRE (f.rack->addChannel ("spinus.3x-osc", "Bass"));
    f.session->setSelectedChannel (project.channels.back().id);
    f.roll->handleUpdateNowIfNeeded();
    REQUIRE (f.roll->addNote (45, 0, 2 * bar, 0.8f));
    REQUIRE (f.rack->addChannel ("spinus.3x-osc", "Lead"));
    f.session->setSelectedChannel (project.channels.back().id);
    f.roll->handleUpdateNowIfNeeded();
    // Quarter notes, each an eighth long: hard left, half volume, plain.
    for (int i = 0; i < 3; ++i)
        REQUIRE (f.roll->addNote (69, (model::Ticks)i * model::ticksPerQuarterNote, 480, 0.8f));
    REQUIRE (f.roll->setNoteValue (0, Lane::pan, -1.0f));
    REQUIRE (f.roll->setNoteValue (1, Lane::volume, 0.5f));

    // 4. Surge XT with one of its factory presets, loaded as its own browser does (not built under
    //    AddressSanitizer).
    const auto surgeFile = juce::File (SPINUS_PLUGINS_DIR).getChildFile ("Surge XT.clap");
    juce::String surgePreset;
    if (surgeFile.existsAsFile())
    {
        REQUIRE (f.rack->addChannel ("bundled.surge-xt", "Surge"));
        f.session->setSelectedChannel (project.channels.back().id);
        f.roll->handleUpdateNowIfNeeded();
        REQUIRE (f.roll->addNote (60, 0, bar, 0.8f));
        auto* surge = processorOf<juce::AudioPluginInstance> (*f.session, instrumentOf (project.channels.back()));
        REQUIRE (surge != nullptr);
        const auto presets = plugins::SurgePresets::scan (plugins::SurgePresets::findDataFolder (surgeFile), {});
        REQUIRE (! presets.empty());
        REQUIRE (plugins::ClapPluginFormat::loadPreset (*surge, presets[0].file).wasOk());
        surgePreset = presets[0].name;
        CHECK (surgePatchName (*surge) == surgePreset);
    }

    auto insertOf = [&] (const juce::String& name)
    {
        for (auto& c : project.channels)
            if (c.name == name)
                return c.targetInsert;
        FAIL ("no channel " << name);
        return model::InsertId {};
    };

    // 2. Effects: a Filter with its cutoff automated and a Ducker following the kick on the bass; a
    //    Stereo Utility on the loop; twelve effects on the lead (any number).
    const auto bass = insertOf ("Bass");
    REQUIRE (f.mixer->addEffect (bass, "spinus.filter"));
    REQUIRE (f.mixer->addEffect (bass, "spinus.ducker"));
    REQUIRE (f.mixer->setSidechain (bass, insertOf ("Kick")));
    model::ParameterTarget cutoff;
    cutoff.kind = model::ParameterTarget::Kind::deviceParameter;
    cutoff.device = project.findInsert (bass)->effects[0].device.id;
    cutoff.parameterId = "cutoff";
    REQUIRE (f.session->createAutomationClip (cutoff).isValid());
    REQUIRE (f.mixer->addEffect (insertOf ("Break"), "spinus.stereo-utility"));
    for (int i = 0; i < 12; ++i)
        REQUIRE (f.mixer->addEffect (insertOf ("Lead"), "spinus.stereo-utility"));
    CHECK (project.findInsert (insertOf ("Lead"))->effects.size() == 12);
    CHECK (f.session->getAdapter().getMissingDevices().empty());

    REQUIRE (f.playlist->placePattern (0, 0));
    f.session->setMode (Session::Mode::song);

    // Export the mix and the stems.
    const auto first = f.exportSong ("first", true);
    const auto firstFiles = f.lastFiles;
    CHECK (rmsOf (first, 0, 0.1, 3.5) > 0.01f);

    // The loop's slices play on every beat.
    const auto loop = readWav (stemOf (firstFiles, "Break"));
    for (int beat = 0; beat < 8; ++beat)
    {
        INFO ("beat " << beat);
        CHECK (rmsOf (loop, 0, beat * 0.5 + 0.005, beat * 0.5 + 0.05) > 0.05f);
    }

    // The bass dips after each kick and comes back before the next one, in its stem too.
    const auto bassStem = readWav (stemOf (firstFiles, "Bass"));
    for (int beat = 1; beat < 4; ++beat)
    {
        INFO ("beat " << beat);
        const auto dipped = rmsOf (bassStem, 0, beat * 0.5 + 0.02, beat * 0.5 + 0.08);
        const auto recovered = rmsOf (bassStem, 0, beat * 0.5 + 0.40, beat * 0.5 + 0.48);
        CHECK (recovered > 0.005f);
        CHECK (dipped < 0.7f * recovered);
    }

    // The lead's first note is hard left, the second at half volume.
    const auto lead = readWav (stemOf (firstFiles, "Lead"));
    const auto plain = rmsOf (lead, 0, 1.03, 1.2);
    REQUIRE (plain > 0.01f);
    CHECK (rmsOf (lead, 0, 0.03, 0.2) > 0.5f * plain);
    CHECK (rmsOf (lead, 1, 0.03, 0.2) < 0.01f * plain);
    CHECK (rmsOf (lead, 0, 0.53, 0.7) / plain == Catch::Approx (0.5f).margin (0.04f));

    // Saved, closed, opened again: the same sound, slices, sidechain and Surge preset.
    REQUIRE (f.document->saveAs (f.root.getChildFile ("Acceptance")).wasOk());
    f.document->newProject();
    REQUIRE (f.document->open (f.root.getChildFile ("Acceptance")).wasOk());
    CHECK (f.session->getAdapter().getMissingDevices().empty());
    CHECK (hasSlices (*f.session, breakChannel));
    CHECK (project.findInsert (bass)->sidechainSources == std::vector { insertOf ("Kick") });
    f.session->setMode (Session::Mode::song);
    f.exportSong ("reopened", true);
    REQUIRE (f.lastFiles.size() == firstFiles.size());
    for (size_t i = 0; i < firstFiles.size(); ++i)
    {
        INFO (firstFiles[i].getFileName());
        const auto before = readWav (firstFiles[i]), after = readWav (f.lastFiles[i]);
        if (surgePreset.isNotEmpty() && (i == 0 || firstFiles[i].getFileName().contains ("Surge")))
            // Surge XT's oscillators drift at random: only as loud.
            CHECK (std::abs (rmsOf (after, 0, 0.0, 2.0) / rmsOf (before, 0, 0.0, 2.0) - 1.0f) < 0.1f);
        else
            CHECK (difference (before, after) < 0.001f);
    }
    if (surgePreset.isNotEmpty())
    {
        const auto& surgeChannel = *std::find_if (project.channels.begin(), project.channels.end(),
                                                  [] (auto& c) { return c.name == "Surge"; });
        auto* surge = processorOf<juce::AudioPluginInstance> (*f.session, instrumentOf (surgeChannel));
        REQUIRE (surge != nullptr);
        CHECK (surgePatchName (*surge) == surgePreset);
    }

    // 6. The assistant, with a local model: "add a reverb bus for the kick" comes back as a proposal of
    //    three steps, applied as one undo step.
    if (ai::AiService::findInstallation().python == juce::File())
    {
        WARN ("the AI service's Python environment is not set up (uv sync in ai/): assistant part skipped");
        return;
    }
    const auto replies = f.root.getChildFile ("replies.json");
    replies.replaceWithText (juce::JSON::toString (juce::JSON::parse (R"([
        {"tool": ["project_overview", {}]},
        {"tool": ["propose_changes", {"summary": "Add a reverb bus for the kick", "steps": [
            {"command": "insert.add", "parameters": {"name": "Kick Reverb"}},
            {"command": "insert.addEffect", "parameters": {"insert": "$1.insert",
                "device": {"kind": "stock", "stockType": "bundled.dragonfly.plate"}}},
            {"command": "insert.setSend", "parameters": {"insert": )" +
                                                                      juce::String (insertOf ("Kick").value) +
                                                                      R"(, "target": "$1.insert", "levelDb": -6}}
        ]}]},
        {"text": "Apply the proposal to send the kick to a new reverb bus."}
    ])")));
    test::FakeProvider provider (replies);
    REQUIRE (provider.ready);

    ui::AssistantPanel panel (*f.session, service);
    panel.setSize (300, 600);
    panel.ask ("Add a reverb bus for the kick");
    CHECK (panel.getEntries().empty()); // AI is still off

    ui::AiSettingsDialog settings (service);
    settings.setAiOn (true);
    settings.addConnection ("local", "Ollama", {}, provider.url());
    REQUIRE (test::waitFor ([&] { return ! settings.isWaiting() && ! settings.getConnections().empty(); }));
    settings.useModel (0, "fake-model");
    REQUIRE (test::waitFor ([&] { return settings.getAssistantRoute() == "Ollama: fake-model"; }));

    panel.ask ("Add a reverb bus for the kick");
    REQUIRE (test::waitFor ([&] { return ! panel.isAnswering(); }));
    const auto& entries = panel.getEntries();
    for (auto& e : entries)
        UNSCOPED_INFO ((int)e.kind << ": " << e.text);
    REQUIRE (entries.size() == 3);
    REQUIRE (entries[2].kind == ui::AssistantPanel::Entry::Kind::proposal);
    INFO (entries[2].proposal->error);
    REQUIRE (entries[2].proposal->canApply());
    CHECK (entries[2].proposal->steps.size() == 3);

    const auto before = project;
    REQUIRE (panel.applyProposal (2).wasOk());
    const auto& reverb = project.mixer.inserts.back();
    CHECK (reverb.name == "Kick Reverb");
    CHECK (reverb.effects.size() == 1);
    CHECK (project.findInsert (insertOf ("Kick"))->sends.size() == 1);
    CHECK (history.getUndoDescriptions().strings.getLast() == "Assistant: Add a reverb bus for the kick");
    CHECK (f.session->getAdapter().getMissingDevices().empty());

    REQUIRE (history.undo());
    auto undone = project;
    undone.nextId = before.nextId; // IDs are never reused
    CHECK (undone == before);
    CHECK (! history.canUndo()); // one step, on a freshly opened project

    settings.setAiOn (false);
}

#endif
