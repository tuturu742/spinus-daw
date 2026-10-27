#include <catch2/catch_test_macros.hpp>

#include "engine/EngineHost.h"
#include "model/ModelJson.h"

#include <iostream>
#include "FakeProvider.h"
#include "ui/AiSettingsDialog.h"
#include "ui/AssistantPanel.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;
using spinus::test::FakeProvider;
using spinus::test::waitFor;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-assistant-tests") + "-" + juce::Uuid().toString());
};

} // namespace

TEST_CASE ("The assistant panel is off until AI is turned on", "[ui][ai]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ai::AiService service (t.root);
    AssistantPanel panel (session, service);
    panel.setSize (300, 600);
    bool asked = false;
    panel.onSetUpAi = [&] { asked = true; };

    panel.ask ("Hello?");
    CHECK (panel.getEntries().empty()); // nothing is asked while AI is off (the input is hidden too)
    CHECK (! service.isEnabled());
}

TEST_CASE ("Ask, see the proposal, apply it as one undo step: through the service and a local model", "[ui][ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeProvider provider;
    REQUIRE (provider.ready);

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ai::AiService service (t.root);

    // Options > AI: turn it on, add the local server (no key) and test it.
    AiSettingsDialog settings (service);
    settings.setAiOn (true);
    settings.addConnection ("local", "My local model", {}, provider.url());
    REQUIRE (waitFor (
        [&]
        {
            return ! settings.isWaiting() && ! settings.getConnections().empty() &&
                   ! settings.getConnections()[0].models.isEmpty();
        }));
    CHECK (settings.getConnections()[0].models == juce::StringArray { "fake-model" });
    CHECK (settings.getStatusText().contains ("Works"));
    settings.useModel (0, "fake-model");
    REQUIRE (waitFor ([&] { return settings.getAssistantRoute() == "My local model: fake-model"; }));

    AssistantPanel panel (session, service);
    panel.setSize (300, 600);
    panel.ask ("Make the song slower");
    CHECK (panel.isAnswering());
    REQUIRE (waitFor ([&] { return ! panel.isAnswering(); }));

    const auto& entries = panel.getEntries();
    REQUIRE (entries.size() == 3);
    CHECK (entries[0].text == "Make the song slower");
    CHECK (entries[1].text == "If you apply the proposal, the song plays at 99 BPM.");
    CHECK (entries[1].model == "fake-model via My local model");

    // The answer can be selected and copied.
    auto* box = panel.getEntryTextBox (1);
    REQUIRE (box != nullptr);
    CHECK (box->getText() == entries[1].text);
    CHECK (box->isReadOnly());
    box->selectAll();
    CHECK (box->getHighlightedText() == entries[1].text);
    REQUIRE (entries[2].kind == AssistantPanel::Entry::Kind::proposal);
    REQUIRE (entries[2].proposal->canApply());
    CHECK (entries[2].proposal->steps[0].description.isNotEmpty());

    const auto tempo = session.getProject().settings.tempoBpm;
    CHECK (! juce::approximatelyEqual (tempo, 99.0));
    REQUIRE (panel.applyProposal (2).wasOk());
    CHECK (juce::approximatelyEqual (session.getProject().settings.tempoBpm, 99.0));
    CHECK (session.getHistory().getUndoDescriptions().strings.getLast() == "Assistant: Slow the song down to 99 BPM");
    CHECK (! panel.applyProposal (2).wasOk()); // once
    session.getHistory().undo();
    CHECK (juce::approximatelyEqual (session.getProject().settings.tempoBpm, tempo));

    // The context leaves out what the user switched off.
    CHECK (panel.makeContext().hasProperty ("project"));
    panel.setChip ("project", false);
    CHECK (! panel.makeContext().hasProperty ("project"));

    settings.setAiOn (false);
    CHECK (service.getState() == ai::AiService::State::off);
}

TEST_CASE ("AI settings: a key for another provider is refused, and a new connection is selected", "[ui][ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    FakeProvider provider;
    REQUIRE (provider.ready);
    ai::AiService service (t.root);
    AiSettingsDialog settings (service);
    settings.setAiOn (true);

    // An OpenAI key with the Provider left at Anthropic: nothing is saved, and the message says what to choose.
    settings.addConnection ("anthropic", "Mixed up", "sk-proj-not-a-real-key", {});
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }));
    CHECK (settings.getConnections().empty());
    CHECK (settings.getStatusText().contains ("OpenAI-compatible service"));

    // Two connections: the one just added is the one selected, so Test tests it.
    for (const auto* name : { "First", "Second" })
    {
        const auto before = settings.getConnections().size();
        settings.addConnection ("local", name, {}, provider.url());
        REQUIRE (waitFor (
            [&]
            {
                return ! settings.isWaiting() && settings.getConnections().size() == before + 1 &&
                       ! settings.getConnections().back().models.isEmpty();
            }));
    }
    CHECK (settings.getConnections()[(size_t)settings.getSelectedConnection()].name == "Second");
}

TEST_CASE ("Assistant and AI settings screenshots for the manual", "[.screenshot][ai]")
{
    TempFolder t;
    // Ollama with models a user would have pulled, so the manual shows real model names.
    FakeProvider provider ({}, { "qwen3:8b", "gemma3:12b", "llama3.1:8b" });
    REQUIRE (provider.ready);
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ai::AiService service (t.root);
    SpinusLookAndFeel lookAndFeel { Theme::dark() };

    AiSettingsDialog settings (service);
    settings.setLookAndFeel (&lookAndFeel);
    settings.setAiOn (true);
    settings.addConnection ("local", "Ollama on this computer", {}, provider.url());
    REQUIRE (waitFor (
        [&]
        {
            return ! settings.isWaiting() && ! settings.getConnections().empty() &&
                   ! settings.getConnections()[0].models.isEmpty();
        }));
    settings.useModel (0, "qwen3:8b");
    REQUIRE (waitFor ([&] { return ! settings.getAssistantRoute().isEmpty() && ! settings.isWaiting(); }));
    // The model box shows the model in use, as when the connection is selected again.
    for (auto* child : settings.getChildren())
        if (auto* list = dynamic_cast<juce::ListBox*> (child))
        {
            list->deselectAllRows();
            list->selectRow (0);
        }

    AssistantPanel panel (session, service);
    panel.setLookAndFeel (&lookAndFeel);
    panel.setSize (320, 640);
    panel.ask ("Make the song slower");
    REQUIRE (waitFor ([&] { return ! panel.isAnswering(); }));

    const auto folder = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images");
    for (auto [component, name] : { std::pair<juce::Component*, const char*> { &panel, "assistant.png" },
                                    std::pair<juce::Component*, const char*> { &settings, "ai-settings.png" } })
    {
        const auto image = component->createComponentSnapshot (component->getLocalBounds(), true, 1.0f);
        const auto file = folder.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    }
    panel.setLookAndFeel (nullptr);
    settings.setLookAndFeel (nullptr);
    settings.setAiOn (false);
}

TEST_CASE ("Manual search lists the local models and explains a model it cannot use", "[ui][ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    ai::AiService service (t.root);
    AiSettingsDialog settings (service);
    settings.setSize (620, 560);
    settings.setAiOn (true);
    settings.showModels (true);
    REQUIRE (waitFor ([&] { return settings.getModels().size() >= 4 && ! settings.isWaiting(); }));
    CHECK (settings.getModels()[0].name.contains ("bge-small"));
    CHECK (settings.getModels()[0].size > 100'000'000);
    CHECK (settings.getModels()[0].source.startsWith ("https://huggingface.co/")); // its page, to check its licence
    // Nothing is downloaded until the user asks (models are shared with the user's own Spinus, so this
    // computer's owner may have asked already).
    const auto data = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    const auto models =
        (data.isNotEmpty()
             ? juce::File (data)
             : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share"))
            .getChildFile ("spinus/models");
    CHECK (settings.getModels()[0].downloaded == models.getChildFile ("bge-small-en-v1.5").isDirectory());
    CHECK (settings.getModelsState().startsWith ("Manual search: keywords only."));

    // A folder of the user's own whose model is broken: added, but Use says why it cannot be used and
    // search stays with keywords.
    const auto folder = t.root.getChildFile ("my-model");
    folder.createDirectory();
    folder.getChildFile ("model.onnx").replaceWithText ("not a model");
    folder.getChildFile ("vocab.txt").replaceWithText ("[UNK]\n[CLS]\n[SEP]\n", false, false, "\n");
    settings.addModelFolder (folder, "My model");
    REQUIRE (waitFor ([&] { return settings.getModels().size() == 5 && ! settings.isWaiting(); }));
    const auto row = (int)settings.getModels().size() - 1;
    CHECK (settings.getModels()[(size_t)row].name == "My model");
    CHECK (settings.getModels()[(size_t)row].downloaded);
    CHECK (! settings.getModels()[(size_t)row].pinned);

    settings.useEmbeddingModel (row);
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }));
    CHECK (settings.getStatusText().contains ("Could not use the model"));
    settings.refreshModels();
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }));
    CHECK (settings.getModelsState().startsWith ("Manual search: keywords only."));
    CHECK (! settings.getModels()[(size_t)row].active);

    settings.removeEmbeddingModel (row);
    REQUIRE (waitFor ([&] { return settings.getModels().size() == 4 && ! settings.isWaiting(); }));
    CHECK (folder.getChildFile ("model.onnx").existsAsFile()); // the user's own folder is never deleted
    settings.setAiOn (false);
}

// A real model, by hand (a folder with a downloaded bge-small-en-v1.5; ai/tests/test_embeddings.py makes one):
//   SPINUS_TEST_EMBEDDING_FOLDER=.../models/bge-small-en-v1.5 spinus_tests "[.embeddings]"
TEST_CASE ("A local model is used for manual search", "[.embeddings]")
{
    const juce::File folder (juce::SystemStats::getEnvironmentVariable ("SPINUS_TEST_EMBEDDING_FOLDER", {}));
    if (! folder.isDirectory())
        SKIP ("no SPINUS_TEST_EMBEDDING_FOLDER");
    TempFolder t;
    ai::AiService service (t.root);
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    AiSettingsDialog settings (service);
    settings.setLookAndFeel (&lookAndFeel);
    settings.setSize (620, 560);
    settings.setAiOn (true);
    settings.showModels (true);
    REQUIRE (waitFor ([&] { return settings.getModels().size() >= 4 && ! settings.isWaiting(); }));
    settings.addModelFolder (folder, "bge-small (my folder)");
    REQUIRE (waitFor ([&] { return settings.getModels().size() == 5 && ! settings.isWaiting(); }));
    settings.useEmbeddingModel (4);
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }, 120000));
    settings.refreshModels();
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }));
    CHECK (settings.getModelsState().contains ("keywords and bge-small (my folder)"));
    CHECK (settings.getModels()[4].active);
    settings.setLookAndFeel (nullptr);
    settings.setAiOn (false);
}

// The manual's screenshot: the recommended model downloaded and in use, as a user sets it up. Uses the
// bge-small-en-v1.5 downloaded by this computer's Spinus if there is one (copied), otherwise downloads it
// (133 MB from Hugging Face). Needs onnxruntime and numpy in ai/.venv (uv sync in ai/).
TEST_CASE ("Manual search screenshot for the manual", "[.screenshot][ai]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");
    TempFolder t;
    const auto models = t.root.getChildFile ("models");
    if (const auto downloaded = ai::AiService::getDefaultModelsFolder().getChildFile ("bge-small-en-v1.5");
        downloaded.getChildFile ("model.onnx").existsAsFile() && ! downloaded.getChildFile (".partial").exists())
        REQUIRE (downloaded.copyDirectoryTo (models.getChildFile ("bge-small-en-v1.5")));
    ai::AiService service (t.root, models);
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    AiSettingsDialog settings (service);
    settings.setLookAndFeel (&lookAndFeel);
    settings.setSize (620, 560);
    settings.setAiOn (true);
    settings.showModels (true);
    REQUIRE (waitFor ([&] { return settings.getModels().size() >= 4 && ! settings.isWaiting(); }));
    REQUIRE (settings.getModels()[0].name == "bge-small-en-v1.5");
    if (! settings.getModels()[0].downloaded)
    {
        settings.downloadModel (0);
        REQUIRE (waitFor ([&] { return ! settings.isDownloading() && ! settings.isWaiting(); }, 600000));
        settings.refreshModels();
        REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }));
        REQUIRE (settings.getModels()[0].downloaded);
    }
    settings.useEmbeddingModel (0);
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }, 120000));
    settings.refreshModels();
    REQUIRE (waitFor ([&] { return ! settings.isWaiting(); }));
    INFO (settings.getModelsState());
    REQUIRE (settings.getModelsState().contains ("keywords and bge-small-en-v1.5"));

    const auto image = settings.createComponentSnapshot (settings.getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/manual-search.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    settings.setLookAndFeel (nullptr);
    settings.setAiOn (false);
}

// Real models, by hand (costs a little with Anthropic; free with a local model):
//   SPINUS_TEST_ANTHROPIC_KEY=... spinus_tests "[.ai-real]"
//   SPINUS_TEST_LOCAL_MODEL=qwen3:30b-a3b spinus_tests "[.ai-real]"      (Ollama on localhost:11434)
TEST_CASE ("A real model proposes a multi-step change that Spinus can apply", "[.ai-real]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    project::ProjectData data;
    REQUIRE (model::fromJsonText (
                 juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("model/files/example-v1.json").loadFileAsString(),
                 data.project)
                 .wasOk());
    session.loadProject (data);

    ai::AiService service (t.root);
    AiSettingsDialog settings (service);
    settings.setAiOn (true);
    const auto key = juce::SystemStats::getEnvironmentVariable ("SPINUS_TEST_ANTHROPIC_KEY", {});
    const auto localModel = juce::SystemStats::getEnvironmentVariable ("SPINUS_TEST_LOCAL_MODEL", {});
    if (key.isNotEmpty())
        settings.addConnection ("anthropic", "Anthropic", key, {});
    else if (localModel.isNotEmpty())
        settings.addConnection ("local", "Ollama", {}, "http://localhost:11434/v1");
    else
        SKIP ("no SPINUS_TEST_ANTHROPIC_KEY or SPINUS_TEST_LOCAL_MODEL");
    REQUIRE (waitFor (
        [&]
        {
            return ! settings.isWaiting() && ! settings.getConnections().empty() &&
                   ! settings.getConnections()[0].models.isEmpty();
        }));
    const auto model = key.isNotEmpty() ? juce::String ("claude-haiku-4-5-20251001") : localModel;
    settings.useModel (0, model);
    REQUIRE (waitFor ([&] { return settings.getAssistantRoute().endsWith (model); }));

    AssistantPanel panel (session, service);
    panel.setSize (320, 640);
    panel.ask ("Add a new mixer insert called Reverb Bus with the Hall Reverb on it, and send the Drums insert "
               "to it at -6 dB.");
    REQUIRE (waitFor ([&] { return ! panel.isAnswering(); }, 300000));

    for (auto& e : panel.getEntries())
    {
        std::cout << "[" << (int)e.kind << "] " << e.text
                  << (e.model.isNotEmpty() ? "  (" + e.model + ")" : juce::String()) << "\n";
        if (e.proposal != nullptr)
        {
            std::cout << "  Proposal: " << e.proposal->summary << "\n";
            for (auto& s : e.proposal->steps)
                std::cout << "   - " << s.command << ": " << s.description << "  "
                          << juce::JSON::toString (s.parameters, true) << "\n";
            if (e.proposal->error.isNotEmpty())
                std::cout << "  Error: " << e.proposal->error << "\n";
        }
    }

    const auto proposal = std::find_if (panel.getEntries().begin(), panel.getEntries().end(),
                                        [] (auto& e) { return e.proposal != nullptr; });
    REQUIRE (proposal != panel.getEntries().end());
    REQUIRE (proposal->proposal->canApply());
    const auto index = (int)(proposal - panel.getEntries().begin());
    REQUIRE (panel.applyProposal (index).wasOk());
    const auto& inserts = session.getProject().mixer.inserts;
    const auto bus = std::find_if (inserts.begin(), inserts.end(),
                                   [] (auto& i) { return i.name.containsIgnoreCase ("reverb bus"); });
    REQUIRE (bus != inserts.end());
    CHECK (bus->effects.size() == 1);
    const auto drums = std::find_if (inserts.begin(), inserts.end(), [] (auto& i) { return i.name == "Drums"; });
    REQUIRE (drums != inserts.end());
    CHECK (std::any_of (drums->sends.begin(), drums->sends.end(), [&] (auto& s) { return s.target == bus->id; }));
    settings.setAiOn (false);
}
