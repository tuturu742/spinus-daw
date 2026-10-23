#include "Acceptance.h"
#include <catch2/catch_approx.hpp>
#include "FakeProvider.h"
#include "commands/CommandRegistry.h"
#include "dsp/tune/PitchDetector.h"
#include "ui/AiSettingsDialog.h"
#include "ui/PatternGenerator.h"
#include "ui/RecordIdea.h"
#include "ui/SpinusTune.h"

/** The creative AI acceptance criteria in one song, done through the panels: Freeze,
    ghost notes, real-time Spinus Tune following a melody, a hummed idea and the pattern generator as
    suggestions, and graphical Spinus Tune; exported, saved and opened again.

    Recording from a real microphone needs real hardware: the take here is a file (the recorder
    itself is tested through a loopback cable in tests/session/IdeaTests.cpp). */

#if defined(SPINUS_PLUGINS_DIR)

using namespace spinus;
using namespace spinus::acceptance;
namespace te = tracktion;
using spinus::test::FakeProvider;
using spinus::test::waitFor;

namespace
{
/** A voice-like tone (a band-limited sawtooth). */
juce::File writeVoice (const juce::File& file, double seconds, double hz, double silenceBefore = 0.0)
{
    juce::AudioBuffer<float> audio (1, (int)((seconds + silenceBefore) * rate));
    audio.clear();
    const auto first = (int)(silenceBefore * rate);
    for (int i = 0; i + first < audio.getNumSamples(); ++i)
    {
        double v = 0.0;
        for (int h = 1; h < 16; ++h)
            v += std::sin (2.0 * M_PI * h * hz * i / rate) / h;
        const auto t = i / rate;
        const auto fade = std::min (1.0, std::min (t, seconds - t) / 0.01);
        audio.setSample (0, first + i, (float)(0.25 * v * fade));
    }
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

/** The session's Edit rendered offline at the engine's rate (what plays). */
Render renderSession (Session& session, double seconds)
{
    session.getEdit().dispatchPendingUpdatesSynchronously();
    const auto file = juce::File::createTempFile (".wav");
    juce::WavAudioFormat wav;
    te::Renderer::Parameters params (session.getEdit());
    params.destFile = file;
    params.tracksToDo = te::toBitSet (te::getAllTracks (session.getEdit()));
    params.audioFormat = &wav;
    params.bitDepth = 32;
    params.sampleRateForAudio = session.getEdit().engine.getDeviceManager().getSampleRate();
    params.blockSizeForAudio = 512;
    params.time = { te::TimePosition(), te::TimePosition::fromSeconds (seconds) };
    params.usePlugins = true;
    params.useMasterPlugins = true;
    params.canRenderInMono = false;
    params.checkNodesForAudio = false;
    const auto rendered = te::Renderer::renderToFile ("creative ai acceptance", params);
    auto result = readWav (rendered);
    rendered.deleteFile();
    return result;
}

double pitchAt (const Render& r, double seconds, double sampleRate)
{
    dsp::PitchDetector detector;
    detector.prepare (sampleRate, 60.0f, 1200.0f);
    std::vector<float> mono ((size_t)detector.getFrameLength());
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = r.audio.getSample (0, (int)(seconds * sampleRate) + (int)i);
    const auto found = detector.analyse (mono.data());
    return found.isVoiced() ? sampleRate / found.period : 0.0;
}

double cents (double hz, double reference) { return 1200.0 * std::log2 (hz / reference); }

bool perform (Session& session, const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    return command != nullptr && session.getHistory().perform (std::move (command)).wasOk();
}

model::ChannelId channelPlaying (Session& session, const juce::String& file)
{
    for (auto& clip : session.getProject().playlist.clips)
        if (auto* audio = std::get_if<model::AudioClip> (&clip.content);
            audio != nullptr && audio->file.contains (file))
            return audio->channel;
    return {};
}

model::ClipId clipPlaying (Session& session, const juce::String& file)
{
    for (auto& clip : session.getProject().playlist.clips)
        if (auto* audio = std::get_if<model::AudioClip> (&clip.content);
            audio != nullptr && audio->file.contains (file))
            return clip.id;
    return {};
}
} // namespace

TEST_CASE ("Creative AI acceptance: Freeze, ghost notes, Spinus Tune, ideas and the pattern generator in one song",
           "[acceptance][creative-ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    Fixture f ("spinus-creative-ai-acceptance");
    auto& session = *f.session;
    const auto engineRate = session.getEdit().engine.getDeviceManager().getSampleRate();
    session.setMode (Session::Mode::song);

    // The song: A minor; Drums (kick on every beat) and a Bass, one bar, in the playlist.
    REQUIRE (perform (session, "project.setKey", R"({"tonic": 9, "scale": "minor"})"));
    REQUIRE (f.rack->addChannel ("spinus.drum-machine", "Drums"));
    const auto drums = f.channel ("Drums").id;
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (f.rack->toggleStep (drums, step));
    REQUIRE (f.rack->addChannel ("spinus.3x-osc", "Bass"));
    const auto bass = f.channel ("Bass").id;
    REQUIRE (f.playlist->placePattern (0, 0));
    const auto pattern = session.getCurrentPattern();

    // 1. Freeze: the drums play as audio, sound the same, and their Drum Machine is unloaded.
    const auto live = renderSession (session, 2.0);
    REQUIRE (f.rack->setChannelFrozen (drums, true));
    REQUIRE (waitFor ([&] { return ! session.getFreezer().isRendering(); }, 60000));
    CHECK (session.getFreezer().getStatus (drums).state == Freezer::State::frozen);
    CHECK (session.getAdapter().getDevicePlugin (f.channel ("Drums").instrument.value_or (model::Device {}).id) ==
           nullptr);
    const auto frozen = renderSession (session, 2.0);
    CHECK (rmsOf (live, 0.0, 2.0) > 0.01f);
    CHECK (difference (live, frozen) < 0.01f);

    // 2. Ghost notes: the Piano Roll for the Bass shows the kick.
    session.setSelectedChannel (bass);
    session.dispatchPendingMessages();
    f.roll->handleUpdateNowIfNeeded();
    REQUIRE (f.roll->getGhosts().size() == 1);
    CHECK (f.roll->getGhosts()[0].name == "Drums");
    CHECK (f.roll->getGhosts()[0].notes.size() == 4);

    // 3. AI on (a local model): the pattern generator writes a bass line, shown as a suggestion; Accept is
    //    one undo step. Its E3/A#2 out of A minor are snapped into the key.
    const auto replies = f.root.getChildFile ("replies.json");
    REQUIRE (f.root.createDirectory());
    REQUIRE (replies.replaceWithText (juce::String (R"([
        {"tool": ["write_pattern", {"title": "Root and fifth", "parts": [{"channel": BASS, "notes": [
            {"step": 0, "length": 4, "note": "A1"}, {"step": 4, "length": 4, "note": "E2"},
            {"step": 8, "length": 4, "note": "A1"}, {"step": 12, "length": 4, "note": "A#1"}]}]}]}
    ])")
                                          .replace ("BASS", juce::String (bass.value))));
    FakeProvider provider (replies);
    REQUIRE (provider.ready);
    ai::AiService service (f.root.getChildFile ("settings"), f.root.getChildFile ("models"));
    ui::AiSettingsDialog settings (service);
    settings.setAiOn (true);
    settings.addConnection ("local", "Local model", {}, provider.url());
    REQUIRE (waitFor (
        [&]
        {
            return ! settings.isWaiting() && ! settings.getConnections().empty() &&
                   ! settings.getConnections()[0].models.isEmpty();
        }));

    ui::PatternGenerator generator (session, service);
    ui::PatternGenerator::Request request;
    request.text = "a simple bass line for these drums";
    request.channels = { bass };
    REQUIRE (generator.generate (request).wasOk());
    auto& suggestions = session.getSuggestions();
    REQUIRE (waitFor ([&] { return suggestions.isShowing() || ! generator.isBusy(); }));
    INFO (suggestions.getWorkingMessage());
    REQUIRE (suggestions.isShowing());
    f.roll->handleUpdateNowIfNeeded();
    REQUIRE (f.roll->getSuggestedNotes() != nullptr);
    CHECK (f.roll->getSuggestedNotes()->size() == 4);
    CHECK (f.roll->getSuggestedNotes()->at (3).pitch == 33); // A#1 is not in A minor: A1
    const auto stepsBefore = session.getHistory().getUndoDescriptions().size();
    REQUIRE (suggestions.accept().wasOk());
    CHECK (session.getHistory().getUndoDescriptions().size() == stepsBefore + 1);
    CHECK (session.getProject().findPattern (pattern)->notes.at (bass).size() == 4);

    // 4. Record idea: a hummed take (C5 E5 over the pattern) becomes notes for a Lead, accepted.
    REQUIRE (f.rack->addChannel ("spinus.3x-osc", "Lead"));
    const auto lead = f.channel ("Lead").id;
    session.setSelectedChannel (lead);
    juce::AudioBuffer<float> hum (1, (int)(2.5 * rate));
    hum.clear();
    {
        const auto c5 = writeVoice (f.root.getChildFile ("c.wav"), 0.45, 523.25);
        const auto e5 = writeVoice (f.root.getChildFile ("e.wav"), 0.45, 659.26);
        const auto a = readWav (c5), b = readWav (e5);
        hum.copyFrom (0, (int)(0.52 * rate), a.audio, 0, 0, a.audio.getNumSamples());
        hum.copyFrom (0, (int)(1.52 * rate), b.audio, 0, 0, b.audio.getNumSamples());
    }
    const auto take = f.root.getChildFile ("take.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take);
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (hum, 0, hum.getNumSamples());
    }
    ui::RecordIdea idea (session, service);
    idea.analyse (ui::RecordIdea::Kind::melody, { take, 0.5, 2.0, 120.0 });
    REQUIRE (waitFor ([&] { return suggestions.isShowing() || ! idea.isBusy(); }));
    INFO (suggestions.getWorkingMessage());
    REQUIRE (suggestions.isShowing());
    const auto& hummed = suggestions.getCurrent()->notes.at (lead);
    REQUIRE (hummed.size() == 2);
    CHECK ((hummed[0].pitch == 72 && hummed[0].start == 0));
    CHECK ((hummed[1].pitch == 76 && hummed[1].start == 1920));
    REQUIRE (suggestions.accept().wasOk());

    // 5. Spinus Tune, real time: a vocal (280 Hz) on its insert follows a Guide channel's D4.
    const auto vocalFile = writeVoice (f.root.getChildFile ("vocal.wav"), 2.0, 280.0);
    REQUIRE (f.playlist->addAudioFile (1, 0, vocalFile));
    const auto vocal = channelPlaying (session, "vocal.wav");
    REQUIRE (vocal.isValid());
    const auto vocalInsert = session.getProject().findChannel (vocal)->targetInsert;
    REQUIRE (f.mixer->addEffect (vocalInsert, "spinus.tune"));
    REQUIRE (f.rack->addChannel ({}, "Guide"));
    const auto guide = f.channel ("Guide").id;
    REQUIRE (perform (session, "pattern.setNotes",
                      "{\"pattern\": " + juce::String (pattern.value) + ", \"channel\": " + juce::String (guide.value) +
                          ", \"notes\": [{\"start\": 0, \"length\": 3840, \"pitch\": 62}]}"));
    REQUIRE (f.mixer->setNotesFrom (vocalInsert, guide));
    REQUIRE (f.mixer->setInsertSolo (vocalInsert, true));
    const auto tunedLive = renderSession (session, 1.8);
    CHECK (std::abs (cents (pitchAt (tunedLive, 1.0, engineRate), 293.66)) < 10.0);
    REQUIRE (f.mixer->setInsertSolo (vocalInsert, false));

    // 6. Spinus Tune, graphical: a second vocal (A3), analysed by the AI service, moved up a tone.
    const auto secondFile = writeVoice (f.root.getChildFile ("second.wav"), 1.6, 220.0, 0.2);
    REQUIRE (f.playlist->addAudioFile (2, 0, secondFile));
    const auto secondClip = clipPlaying (session, "second.wav");
    const auto secondInsert = session.getProject().findChannel (channelPlaying (session, "second.wav"))->targetInsert;
    {
        ui::SpinusTuneEditor editor (session, secondClip, &service);
        editor.setSize (900, 480);
        REQUIRE (waitFor ([&] { return editor.isAnalysed(); }));
        REQUIRE (editor.getNotes().size() == 1);
        CHECK (editor.getNotes()[0].pitch == Catch::Approx (57.0f).margin (0.15f));
        REQUIRE (editor.moveNotes ({ 0 }, 2.0f));
    }
    REQUIRE (waitFor ([&] { return session.getTuner().getStatus (secondClip).state == Tuner::State::ready; }, 60000));
    REQUIRE (f.mixer->setInsertSolo (secondInsert, true));
    const auto tunedClip = renderSession (session, 1.6);
    CHECK (std::abs (cents (pitchAt (tunedClip, 1.0, engineRate), 246.94)) < 10.0);
    REQUIRE (f.mixer->setInsertSolo (secondInsert, false));

    // 7. Export: the tuned vocal is in the mix; the frozen drums too (rendered from their plugin).
    const auto exported = f.exportSong ("creative-ai");
    CHECK (rmsOf (exported, 0.0, 2.0) > 0.01f);

    // 8. Saved and opened again: the drums are still frozen (no render, no plugin), the bass line and the
    //    hummed lead are there, Spinus Tune still follows the guide, and the tuned clip plays its rendering.
    const auto freezesBefore = session.getFreezer().getRenderCount();
    const auto tunesBefore = session.getTuner().getRenderCount();
    REQUIRE (f.document->saveAs (f.root.getChildFile ("CreativeAi")).wasOk());
    f.document->newProject();
    REQUIRE (f.document->open (f.root.getChildFile ("CreativeAi")).wasOk());
    session.setMode (Session::Mode::song);
    CHECK (session.getProject().findChannel (drums)->frozen);
    CHECK (session.getFreezer().getRenderCount() == freezesBefore);
    CHECK (session.getFreezer().getStatus (drums).state == Freezer::State::frozen);
    CHECK (session.getProject().findPattern (pattern)->notes.at (bass).size() == 4);
    CHECK (session.getProject().findPattern (pattern)->notes.at (lead).size() == 2);
    CHECK (session.getProject().findInsert (vocalInsert)->notesFrom == guide);
    CHECK (session.getProject().settings.key == model::Key { 9, "minor" });
    session.dispatchPendingMessages();
    CHECK (session.getTuner().getStatus (secondClip).state == Tuner::State::ready);
    CHECK (session.getTuner().getRenderCount() == tunesBefore);
    CHECK (! suggestions.isShowing());

    // 9. A manual page for everything.
    const auto docs = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user");
    CHECK (docs.getChildFile ("freeze.md").existsAsFile());
    CHECK (docs.getChildFile ("spinus-tune.md").loadFileAsString().contains ("Editing a vocal note by note"));
    for (auto section : { "## Ghost notes", "## Suggestions", "## Record an idea", "## Generate" })
        CHECK (docs.getChildFile ("piano-roll.md").loadFileAsString().contains (section));
    CHECK (docs.getChildFile ("assistant.md").loadFileAsString().contains ("Notes from the assistant"));

    settings.setAiOn (false);
}

#endif
