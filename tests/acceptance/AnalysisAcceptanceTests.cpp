#include "Acceptance.h"
#include "../ui/AnalysisFakes.h"
#include "FakeProvider.h"
#include "ui/Analyzer.h"
#include "ui/Automixer.h"

/** The analysis acceptance criteria in one song, done through the panels: the
    Analyzer finds tempo and key and splits the song into stems added as audio; the stems become notes with
    what plays them chosen per stem; then the Automixer measures the mix, applies its changes as one undo
    step and compares before and after at matched loudness; saved and opened again.

    The models are stand-ins here (ai/tests/fakes: PyTorch, Demucs, Beat This!, CLAP); the real models on
    a real graphics card are checked by tools/owner-checks/analysis.sh. Basic Pitch runs for real
    when SPINUS_TEST_BASIC_PITCH names its model file. */

#if defined(SPINUS_PLUGINS_DIR)

using namespace spinus;
using namespace spinus::acceptance;
using namespace spinus::test::analysis;
using spinus::test::waitFor;

TEST_CASE ("Analysis acceptance: analyze a song, notes and sounds per stem, then the automixer",
           "[acceptance][analysis]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    Fixture f ("spinus-analysis-acceptance");
    FakeAnalysis fake;
    auto& session = *f.session;
    const auto models = f.root.getChildFile ("models");
    ai::AiService service (f.root, models);
    service.setEnabled (true);
    service.call ("ai.v1.compute.install", juce::JSON::parse (R"({"build": "cpu"})"), {});
    REQUIRE (waitFor (
        [&] { return models.getSiblingFile ("python").getChildFile ("cpu/spinus-build.json").existsAsFile(); }, 60000));
    placeModel (models, "beat_this_small0", { { "small0.ckpt", 8451101 } });
    placeModel (models, "htdemucs", { { "htdemucs.yaml", 21 }, { "955717e8.safetensors", 84025440 } },
                "models: ['955717e8']\n");
    const bool realNotes = placeBasicPitch (models);

    // 1. The Analyzer: tempo, beats and key with an offer to use them, then stems as audio.
    ui::Analyzer analyzer (session, &service);
    analyzer.setSize (660, 560);
    REQUIRE (analyzer.analyze (makeSong (f.root)));
    REQUIRE (waitFor ([&] { return analyzer.getStep() == ui::Analyzer::Step::done || ! analyzer.isBusy(); }, 60000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getTempoText() == "120.0 BPM, 4 beats per bar");
    CHECK (analyzer.getKeyText().startsWith ("A minor"));
    REQUIRE (analyzer.useTempo());
    REQUIRE (analyzer.useKey());
    REQUIRE (analyzer.separate (4));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 60000));
    REQUIRE (analyzer.getStems().size() == 4);
    REQUIRE (analyzer.addStemsAsAudio ({ "bass", "drums", "other", "vocals" }));
    CHECK (session.getProject().playlist.lanes.size() == 4);

    // 2 and 3. Notes per stem, each with its own way of sounding: the drums as steps with the song's own
    // hits; with Basic Pitch's model, the bass on 3x Osc tuned to it and the vocals on 3x Osc.
    std::map<juce::String, ui::Analyzer::Sounds> choices { { "drums", ui::Analyzer::Sounds::song } };
    if (realNotes)
    {
        choices["bass"] = ui::Analyzer::Sounds::tunedThreeOsc;
        choices["vocals"] = ui::Analyzer::Sounds::spinus;
    }
    analyzer.limitTriesForTests (30);
    REQUIRE (analyzer.makeNotesWith (choices));
    REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 300000));
    INFO (analyzer.getStatusText());
    CHECK (analyzer.getStatusText().contains ("new channels"));
    CHECK (f.channel ("Drums (Test Song)").instrument.value_or (model::Device {}).stockType == "spinus.drum-machine");
    CHECK (analyzer.getPieces().contains ("drums"));
    if (realNotes)
        CHECK (analyzer.getTuned().contains ("bass"));
    const auto analysisSteps = session.getHistory().getUndoDescriptions().size();

    // 4. The Automixer: measures every insert, suggests changes with reasons, one undo step, A/B.
    ui::Automixer automixer (session, &service);
    automixer.setSize (620, 480);
    REQUIRE (automixer.analyze (ui::Automixer::Target::streaming));
    REQUIRE (waitFor ([&] { return ! automixer.isBusy(); }, 180000));
    INFO (automixer.getStatusText());
    REQUIRE (! automixer.getItems().empty());
    for (auto& item : automixer.getItems())
        CHECK (item.json["reason"].toString().isNotEmpty());
    REQUIRE (automixer.apply());
    REQUIRE (waitFor ([&] { return ! automixer.isBusy(); }, 180000));
    REQUIRE (automixer.canCompare());
    CHECK (session.getHistory().getUndoDescriptions().size() == analysisSteps + 1);
    REQUIRE (automixer.listen (true));
    REQUIRE (automixer.listen (false));
    automixer.keep();
    const auto mixed = session.getProject();

    // Saved and opened again: the automixer's settings come back with the project.
    REQUIRE (f.document->saveAs (f.root.getChildFile ("Analysis")).wasOk());
    session.getHistory().reset (model::Project());
    REQUIRE (f.document->open (f.root.getChildFile ("Analysis")).wasOk());
    CHECK (session.getProject().mixer == mixed.mixer);

    // 6. The manual has a page for each part.
    const auto manual = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user");
    for (auto page : { "analyzer.md", "automixer.md", "analysis-engine.md", "synth-profiles.md" })
        CHECK (manual.getChildFile (page).existsAsFile());
    service.setEnabled (false);
}

#endif
