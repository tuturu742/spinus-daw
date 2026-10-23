#include "AiSettingsDialog.h"

#include "Theme.h"
#include "WindowStacking.h"

namespace spinus::ui
{

namespace
{
/** 75213 -> "75,213". */
juce::String formatTokens (juce::int64 n)
{
    auto digits = juce::String (n);
    for (int i = digits.length() - 3; i > 0; i -= 3)
        digits = digits.substring (0, i) + "," + digits.substring (i);
    return digits;
}

constexpr int margin = 14;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

const char* const providers[] = { "anthropic", "openrouter", "openai", "local" };
const char* const providerNames[] = { "Anthropic (Claude)", "OpenRouter", "OpenAI-compatible service",
                                      "Local server (Ollama, llama.cpp)" };

juce::String providerName (const juce::String& id)
{
    for (size_t i = 0; i < std::size (providers); ++i)
        if (id == providers[i])
            return providerNames[i];
    return id;
}
} // namespace

/** Stands for a model job until the service answers with its id. */
static constexpr auto startingJob = "starting";

/** The embedding models: name, size, license and state. */
class AiSettingsDialog::ModelList final : public juce::ListBoxModel
{
public:
    explicit ModelList (AiSettingsDialog& d) : dialog (d) {}

    int getNumRows() override { return (int)dialog.models.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int)dialog.models.size()))
            return;
        const auto& theme = themeOf (dialog);
        const auto& m = dialog.models[(size_t)row];
        if (selected)
            g.fillAll (theme.get ("accent").withAlpha (0.25f));
        const auto state = m.active                                 ? juce::String ("In use")
                           : m.downloaded                           ? juce::String ("Downloaded")
                           : m.own && m.source.startsWithChar ('/') ? juce::String ("Your folder")
                                                                    : juce::String ("Not downloaded");
        g.setColour (theme.get (m.active ? "accent" : "text"));
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (m.name, 10, 2, width - 130, height / 2, juce::Justification::bottomLeft, true);
        g.drawText (state, width - 120, 2, 110, height / 2, juce::Justification::bottomRight, true);
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (11.0f));
        juce::String details;
        if (m.size > 0)
            details << juce::String (juce::roundToInt ((double)m.size / 1.0e6)) << " MB - ";
        details << m.license << " - " << m.languages << " - " << m.description;
        if (! m.missing.isEmpty())
            details = "Needs " + m.missing.joinIntoString (", ") + ". " + details;
        g.drawText (details, 10, height / 2, width - 20, height / 2 - 2, juce::Justification::topLeft, true);
    }

    // Where a model comes from, to check its licence: in the tooltip, and opened from a right-click.
    juce::String getTooltipForRow (int row) override
    {
        return juce::isPositiveAndBelow (row, (int)dialog.models.size()) ? dialog.models[(size_t)row].source
                                                                         : juce::String();
    }
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() || ! juce::isPositiveAndBelow (row, (int)dialog.models.size()))
            return;
        const auto source = dialog.models[(size_t)row].source;
        if (! source.startsWith ("https://"))
            return;
        juce::PopupMenu menu;
        menu.addItem ("Open its page (" + source.fromFirstOccurrenceOf ("://", false, false) + ")",
                      [source] { juce::URL (source).launchInDefaultBrowser(); });
        menu.showMenuAsync ({});
    }

private:
    AiSettingsDialog& dialog;
};

AiSettingsDialog::AiSettingsDialog (ai::AiService& s) : service (s)
{
    onOff.setToggleState (service.isEnabled(), juce::dontSendNotification);
    onOff.onClick = [this] { setAiOn (onOff.getToggleState()); };
    experimentalToggle.setToggleState (service.showsExperimentalFeatures(), juce::dontSendNotification);
    experimentalToggle.onClick = [this] { setShowExperimental (experimentalToggle.getToggleState()); };
    experimentalNote.setText ("Shows features that are still being worked on and may change, be slow or not work "
                              "well. For now that is the Analyzer in the View menu, which finds a song's tempo, key "
                              "and stems. Turning this off closes them.",
                              juce::dontSendNotification);
    experimentalNote.setJustificationType (juce::Justification::topLeft);
    experimentalNote.setFont (juce::FontOptions (13.0f));
    explanation.setText (
        "AI features are off until you turn them on. When you ask the assistant, Spinus sends your question, "
        "the layout of your song (channel, pattern and mixer names, notes, effect names, no audio) and matching "
        "parts of the manual to the connection you choose. With a local server (Ollama, llama.cpp) everything "
        "stays on this computer. Keys are kept in your system's keyring, never in projects.",
        juce::dontSendNotification);
    explanation.setJustificationType (juce::Justification::topLeft);
    status.setJustificationType (juce::Justification::topLeft);
    connectionsLabel.setText ("Connections", juce::dontSendNotification);
    modelLabel.setText ("Assistant model", juce::dontSendNotification);
    usage.setJustificationType (juce::Justification::topLeft);
    usage.setFont (juce::FontOptions (13.0f));
    list.setRowHeight (40);
    modelBox.setEditableText (true);
    modelBox.setTooltip ("The model the assistant uses on the selected connection (type one, or test the "
                         "connection to list them)");

    addButton.onClick = [this] { showAddForm (true); };
    testButton.onClick = [this] { testConnection (list.getSelectedRow()); };
    removeButton.onClick = [this] { removeConnection (list.getSelectedRow()); };
    localButton.onClick = [this] { findLocalServers(); };
    useButton.onClick = [this] { useModel (list.getSelectedRow(), modelBox.getText()); };
    patternsButton.setTooltip ("The pattern generator uses this model (until then it uses the assistant's)");
    patternsButton.onClick = [this] { useModel (list.getSelectedRow(), modelBox.getText(), "patterns"); };

    for (size_t i = 0; i < std::size (providers); ++i)
        providerBox.addItem (providerNames[i], (int)i + 1);
    providerBox.onChange = [this]
    {
        const auto index = providerBox.getSelectedItemIndex();
        const bool local = index == 3;
        keyField.setEnabled (! local);
        keyField.setTextToShowWhenEmpty (local ? "No key needed" : "Paste your API key", juce::Colours::grey);
        urlField.setText (index == 0   ? "https://api.anthropic.com"
                          : index == 1 ? "https://openrouter.ai/api/v1"
                          : index == 2 ? "https://api.openai.com/v1"
                                       : "http://localhost:11434/v1",
                          false);
        if (nameField.isEmpty() || nameField.getProperties().getWithDefault ("auto", false))
        {
            nameField.setText (index == 0   ? "Anthropic"
                               : index == 1 ? "OpenRouter"
                               : index == 2 ? "OpenAI"
                                            : "Local",
                               false);
            nameField.getProperties().set ("auto", true);
        }
    };
    nameField.onTextChange = [this] { nameField.getProperties().set ("auto", false); };
    keyField.setPasswordCharacter ((juce::juce_wchar)0x2022);
    providerLabel.setText ("Provider", juce::dontSendNotification);
    nameLabel.setText ("Name", juce::dontSendNotification);
    keyLabel.setText ("Key", juce::dontSendNotification);
    urlLabel.setText ("Address", juce::dontSendNotification);
    saveButton.onClick = [this]
    {
        addConnection (providers[juce::jlimit (0, 3, providerBox.getSelectedItemIndex())], nameField.getText(),
                       keyField.getText(), urlField.getText());
        keyField.clear();
        showAddForm (false);
    };
    cancelButton.onClick = [this] { showAddForm (false); };

    for (auto* c : std::initializer_list<juce::Component*> {
             &onOff,         &experimentalToggle, &experimentalNote, &explanation,
             &status,        &connectionsLabel,   &modelLabel,       &usage,
             &list,          &addButton,          &testButton,       &removeButton,
             &localButton,   &useButton,          &patternsButton,   &modelBox,
             &providerBox,   &nameField,          &keyField,         &urlField,
             &providerLabel, &nameLabel,          &keyLabel,         &urlLabel,
             &saveButton,    &cancelButton })
        addChildComponent (c);

    modelListModel = std::make_unique<ModelList> (*this);
    modelList = std::make_unique<juce::ListBox> ("Embedding models", modelListModel.get());
    modelList->setRowHeight (40);
    modelsIntro.setText ("The assistant searches the Spinus manual to answer how-to questions. With a small local "
                         "model it also finds answers by meaning, not only by matching words. The model runs on this "
                         "computer; nothing is sent anywhere. Downloads come from Hugging Face.",
                         juce::dontSendNotification);
    modelsIntro.setJustificationType (juce::Justification::topLeft);
    modelsStatus.setJustificationType (juce::Justification::topLeft);
    linkField.setTextToShowWhenEmpty ("Hugging Face link, e.g. https://huggingface.co/BAAI/bge-small-en-v1.5",
                                      juce::Colours::grey);
    manualSearchButton.setHelpText ("Local models that help the assistant find answers in the manual");
    manualSearchButton.onClick = [this] { showModels (true); };
    engineButton.setHelpText ("PyTorch, which song analysis needs: on the graphics card or the CPU");
    engineButton.onClick = [this] { showAnalysisEngine (true); };
    backButton.onClick = [this] { showModels (false); };
    downloadModelButton.onClick = [this] { downloadModel (modelList->getSelectedRow()); };
    useModelButton.onClick = [this] { useEmbeddingModel (modelList->getSelectedRow()); };
    keywordsButton.onClick = [this] { useEmbeddingModel (-1); };
    removeModelButton.onClick = [this] { removeEmbeddingModel (modelList->getSelectedRow()); };
    cancelDownloadButton.onClick = [this]
    {
        if (downloadJob.isNotEmpty())
            service.call ("ai.v1.job.cancel", object ({ { "job", downloadJob } }), {});
    };
    addLinkButton.onClick = [this] { addModelLink (linkField.getText()); };
    addFolderButton.onClick = [this]
    {
        chooser =
            std::make_unique<core::FileDialog> ("Choose a folder with model.onnx and vocab.txt or tokenizer.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                              {
                                  if (safe != nullptr && c.getResult() != juce::File())
                                      safe->addModelFolder (c.getResult(), c.getResult().getFileName());
                              });
    };
    for (auto* c : std::initializer_list<juce::Component*> {
             modelList.get(), &modelsIntro, &modelsStatus, &manualSearchButton, &downloadModelButton, &useModelButton,
             &keywordsButton, &removeModelButton, &addFolderButton, &addLinkButton, &backButton, &cancelDownloadButton,
             &linkField, &downloadBar, &engineButton })
        addChildComponent (c);

    jobListener = service.addListener (
        [this] (const juce::String& method, const juce::var& params)
        {
            if (method != "ai.v1.job.update" || params["job"].toString() != downloadJob)
                return;
            const auto kind = params["kind"].toString();
            if (kind == "progress")
            {
                const auto total = (double)params["total"];
                downloadProgress = total > 0.0 ? (double)params["done"] / total : -1.0;
                modelsStatus.setText (
                    "Downloading: " + juce::String (juce::roundToInt ((double)params["done"] / 1.0e6)) + " of " +
                        juce::String (juce::roundToInt (total / 1.0e6)) + " MB",
                    juce::dontSendNotification);
            }
            else if (kind == "done" || kind == "error")
            {
                downloadJob.clear();
                downloadProgress = -1.0;
                modelsStatus.setText (kind == "done" ? juce::String ("Downloaded. Select it and click Use.")
                                                     : params["message"].toString(),
                                      juce::dontSendNotification);
                refreshModels();
                updateVisibility();
            }
        });

    stateListener = service.addStateListener ([this] { updateVisibility(); });
    setSize (680, 640);
    updateVisibility();
    refresh();
}

AiSettingsDialog::~AiSettingsDialog()
{
    *alive = false;
    service.removeListener (jobListener);
    modelList->setModel (nullptr);
    service.removeStateListener (stateListener);
}

//==============================================================================
void AiSettingsDialog::call (const juce::String& method, const juce::var& params,
                             std::function<void (const juce::var&)> onResult)
{
    ++waiting;
    service.call (
        method, params,
        [this, a = alive, onResult = std::move (onResult)] (const juce::var& result, const juce::String& error)
        {
            if (! *a)
                return;
            --waiting;
            if (error.isNotEmpty())
            {
                status.setText (error, juce::dontSendNotification);
                if (modelsVisible)
                {
                    modelsStatus.setText (error, juce::dontSendNotification);
                    if (downloadJob == startingJob) // the service refused to start it
                        downloadJob.clear();
                    updateVisibility();
                }
                return;
            }
            if (onResult)
                onResult (result);
        });
}

void AiSettingsDialog::setAiOn (bool on)
{
    service.setEnabled (on);
    onOff.setToggleState (on, juce::dontSendNotification);
    status.setText (on ? "AI is on." : "AI is off. Nothing is sent anywhere.", juce::dontSendNotification);
    updateVisibility();
    if (on)
        refresh();
}

void AiSettingsDialog::setShowExperimental (bool show)
{
    service.setShowExperimentalFeatures (show);
    experimentalToggle.setToggleState (show, juce::dontSendNotification);
}

void AiSettingsDialog::refresh()
{
    if (! service.isEnabled())
        return;
    call ("ai.v1.connections.list", {},
          [this] (const juce::var& result)
          {
              const auto selected = list.getSelectedRow();
              connections.clear();
              if (auto* items = result["connections"].getArray())
                  for (auto& c : *items)
                  {
                      ConnectionInfo info { c["id"], c["name"], c["provider"], c["baseUrl"], (bool)c["hasKey"], {} };
                      if (auto* found = c["models"].getArray())
                          for (auto& m : *found)
                              info.models.add (m.toString());
                      connections.push_back (info);
                  }
              list.updateContent();
              if (! connections.empty())
                  list.selectRow (juce::jlimit (0, (int)connections.size() - 1, juce::jmax (0, selected)));
              list.repaint();
              updateVisibility();
          });
    call ("ai.v1.routing.get", {},
          [this] (const juce::var& result)
          {
              auto routeOf = [this] (const juce::var& chain)
              {
                  if (chain.isArray() && chain.size() > 0)
                      for (auto& c : connections)
                          if (c.id == chain[0]["connection"].toString())
                              return c.name + ": " + chain[0]["model"].toString();
                  return juce::String();
              };
              assistantRoute = routeOf (result["effective"]["assistant"]);
              patternsRoute = routeOf (result["chosen"]["patterns"]);
              modelLabel.setText ("Assistant model" +
                                      (assistantRoute.isNotEmpty() ? " (now " + assistantRoute + ")" : juce::String()) +
                                      (patternsRoute.isNotEmpty() ? "; patterns: " + patternsRoute : juce::String()),
                                  juce::dontSendNotification);
          });
    call ("ai.v1.usage.get", {},
          [this] (const juce::var& result)
          {
              const auto month = juce::Time::getCurrentTime().formatted ("%Y-%m");
              const auto byConnection = result["usage"][juce::Identifier (month)];
              juce::StringArray lines;
              if (auto* o = byConnection.getDynamicObject())
                  for (auto& [connectionId, features] : o->getProperties())
                  {
                      juce::int64 in = 0, out = 0;
                      double cost = 0.0;
                      bool known = false;
                      if (auto* f = features.getDynamicObject())
                          for (auto& [feature, totals] : f->getProperties())
                          {
                              in += (juce::int64)totals["inputTokens"];
                              out += (juce::int64)totals["outputTokens"];
                              if (! totals["cost"].isVoid())
                              {
                                  cost += (double)totals["cost"];
                                  known = true;
                              }
                          }
                      juce::String name = connectionId.toString();
                      for (auto& c : connections)
                          if (c.id == name)
                              name = c.name;
                      lines.add (name + ": " + formatTokens (in + out) + " tokens" +
                                 (known ? ", $" + juce::String (cost, 4) : juce::String()));
                  }
              usage.setText ("Used this month: " +
                                 (lines.isEmpty() ? juce::String ("nothing yet.") : lines.joinIntoString ("; ")),
                             juce::dontSendNotification);
          });
}

void AiSettingsDialog::addConnection (const juce::String& provider, const juce::String& name, const juce::String& key,
                                      const juce::String& baseUrl)
{
    status.setText ("Saving " + name + "...", juce::dontSendNotification);
    call ("ai.v1.connections.save",
          object ({ { "provider", provider }, { "name", name }, { "key", key }, { "baseUrl", baseUrl } }),
          [this, name] (const juce::var& result)
          {
              const auto id = result["id"].toString();
              status.setText ("Testing " + name + "...", juce::dontSendNotification);
              call ("ai.v1.connections.test", object ({ { "id", id } }),
                    [this, name] (const juce::var& tested)
                    {
                        status.setText (name + ": " + tested["message"].toString(), juce::dontSendNotification);
                        refresh();
                    });
          });
}

void AiSettingsDialog::testConnection (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)connections.size()))
        return;
    const auto name = connections[(size_t)row].name;
    status.setText ("Testing " + name + "...", juce::dontSendNotification);
    call ("ai.v1.connections.test", object ({ { "id", connections[(size_t)row].id } }),
          [this, name] (const juce::var& tested)
          {
              status.setText (name + ": " + tested["message"].toString(), juce::dontSendNotification);
              refresh();
          });
}

void AiSettingsDialog::removeConnection (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)connections.size()))
        return;
    call ("ai.v1.connections.remove", object ({ { "id", connections[(size_t)row].id } }),
          [this] (const juce::var&)
          {
              status.setText ("Removed. Its key was deleted from the keyring.", juce::dontSendNotification);
              refresh();
          });
}

void AiSettingsDialog::findLocalServers()
{
    status.setText ("Looking for Ollama and llama.cpp on this computer...", juce::dontSendNotification);
    call ("ai.v1.connections.findLocal", {},
          [this] (const juce::var& result)
          {
              const auto* servers = result["servers"].getArray();
              if (servers == nullptr || servers->isEmpty())
              {
                  status.setText ("No local model server is running. Install Ollama (sudo pacman -S ollama), start "
                                  "it (ollama serve) and get a model (for example: ollama pull qwen3), then try "
                                  "again.",
                                  juce::dontSendNotification);
                  return;
              }
              for (auto& server : *servers)
              {
                  const auto url = server["baseUrl"].toString();
                  const bool known =
                      std::any_of (connections.begin(), connections.end(), [&] (auto& c) { return c.baseUrl == url; });
                  if (! known)
                      addConnection ("local", url.contains ("11434") ? "Ollama" : "Local server", {}, url);
              }
              status.setText ("Found " + juce::String (servers->size()) + " local server(s).",
                              juce::dontSendNotification);
          });
}

void AiSettingsDialog::useModel (int row, const juce::String& model, const juce::String& feature)
{
    if (! juce::isPositiveAndBelow (row, (int)connections.size()) || model.trim().isEmpty())
        return;
    // The chosen one first; the other connections follow as fallbacks, in the order they were added.
    juce::Array<juce::var> chain;
    chain.add (object ({ { "connection", connections[(size_t)row].id }, { "model", model.trim() } }));
    for (size_t i = 0; i < connections.size(); ++i)
        if ((int)i != row)
            chain.add (object ({ { "connection", connections[i].id } }));
    call ("ai.v1.routing.set", object ({ { "feature", feature }, { "chain", chain } }),
          [this, model, feature] (const juce::var&)
          {
              status.setText ((feature == "patterns" ? "The pattern generator now uses " : "The assistant now uses ") +
                                  model.trim() + ".",
                              juce::dontSendNotification);
              refresh();
          });
}

//==============================================================================
void AiSettingsDialog::showAnalysisEngine (bool show)
{
    if (show && enginePage == nullptr && service.isEnabled())
    {
        enginePage = std::make_unique<AnalysisEnginePage> (service);
        enginePage->onBack = [this] { showAnalysisEngine (false); };
        addAndMakeVisible (*enginePage);
    }
    else if (! show && enginePage != nullptr)
    {
        removeChildComponent (enginePage.get());
        enginePage = nullptr;
    }
    formVisible = modelsVisible = false;
    updateVisibility();
}

void AiSettingsDialog::showModels (bool show)
{
    modelsVisible = show;
    formVisible = false;
    if (show)
        refreshModels();
    updateVisibility();
}

void AiSettingsDialog::refreshModels()
{
    if (! service.isEnabled())
        return;
    call ("ai.v1.embeddings.status", {},
          [this] (const juce::var& result)
          {
              const auto selected = modelList->getSelectedRow();
              models.clear();
              if (auto* items = result["models"].getArray())
                  for (auto& m : *items)
                  {
                      ModelInfo info;
                      info.id = m["id"];
                      info.name = m["name"];
                      info.description = m["description"];
                      info.license = m["license"];
                      info.languages = m["languages"];
                      info.source = m["source"];
                      info.size = (juce::int64)m["size"];
                      info.downloaded = m["downloaded"];
                      info.own = m["own"];
                      info.pinned = m["pinned"];
                      info.active = m["active"];
                      if (auto* missing = m["missing"].getArray())
                          for (auto& x : *missing)
                              info.missing.add (x.toString());
                      models.push_back (info);
                  }
              const auto state = result["state"].toString();
              const auto active = std::find_if (models.begin(), models.end(), [] (auto& m) { return m.active; });
              modelsState = state == "keywords"  ? juce::String ("Manual search: keywords only.")
                            : state == "loading" ? juce::String ("Preparing the model...")
                            : state == "ready" && active != models.end()
                                ? "Manual search: keywords and " + active->name + "."
                                : state;
              juce::StringArray missing;
              if (auto* m = result["missing"].getArray())
                  for (auto& x : *m)
                      missing.add (x.toString());
              if (! missing.isEmpty())
                  modelsState << " Local models need: sudo pacman -S " << missing.joinIntoString (" ");
              if (downloadJob.isEmpty())
                  modelsStatus.setText (modelsState, juce::dontSendNotification);
              modelList->updateContent();
              const auto activeRow = (int)std::distance (models.begin(), active);
              modelList->selectRow (selected < 0 && active != models.end()
                                        ? activeRow
                                        : juce::jlimit (0, juce::jmax (0, (int)models.size() - 1), selected));
              modelList->repaint();
          });
}

void AiSettingsDialog::startModelJob (const juce::String& method, const juce::var& params, const juce::String& what)
{
    if (downloadJob.isNotEmpty())
        return;
    modelsStatus.setText (what, juce::dontSendNotification);
    downloadProgress = 0.0;
    downloadJob = startingJob;
    updateVisibility();
    call (method, params,
          [this] (const juce::var& result)
          {
              downloadJob = result["job"].toString();
              updateVisibility();
          });
}

void AiSettingsDialog::downloadModel (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)models.size()) || models[(size_t)row].downloaded)
        return;
    startModelJob ("ai.v1.embeddings.download", object ({ { "id", models[(size_t)row].id } }),
                   "Downloading " + models[(size_t)row].name + "...");
}

void AiSettingsDialog::addModelLink (const juce::String& link)
{
    if (link.trim().isEmpty())
        return;
    startModelJob ("ai.v1.embeddings.addFromHuggingFace", object ({ { "link", link.trim() } }),
                   "Downloading from " + link.trim() + " (its files are not checked against a known checksum)...");
    linkField.clear();
}

void AiSettingsDialog::addModelFolder (const juce::File& folder, const juce::String& name)
{
    call ("ai.v1.embeddings.addFolder", object ({ { "folder", folder.getFullPathName() }, { "name", name } }),
          [this] (const juce::var&)
          {
              modelsStatus.setText ("Added. Select it and click Use.", juce::dontSendNotification);
              refreshModels();
          });
}

void AiSettingsDialog::useEmbeddingModel (int row)
{
    const auto id = juce::isPositiveAndBelow (row, (int)models.size()) ? models[(size_t)row].id : juce::String();
    modelsStatus.setText (id.isEmpty() ? "Using keywords only."
                                       : "Preparing " + models[(size_t)row].name +
                                             " (the first time it reads the whole manual)...",
                          juce::dontSendNotification);
    call ("ai.v1.embeddings.use", object ({ { "id", id } }), [this] (const juce::var&) { refreshModels(); });
}

void AiSettingsDialog::removeEmbeddingModel (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)models.size()))
        return;
    call ("ai.v1.embeddings.remove", object ({ { "id", models[(size_t)row].id } }),
          [this] (const juce::var&)
          {
              modelsStatus.setText ("Removed.", juce::dontSendNotification);
              refreshModels();
          });
}

void AiSettingsDialog::showAddForm (bool show)
{
    formVisible = show;
    if (show)
    {
        nameField.clear();
        providerBox.setSelectedItemIndex (0, juce::sendNotificationSync);
    }
    updateVisibility();
}

void AiSettingsDialog::updateVisibility()
{
    const bool on = service.isEnabled();
    if (! on && enginePage != nullptr)
    {
        removeChildComponent (enginePage.get());
        enginePage = nullptr;
    }
    const bool subPage = modelsVisible || enginePage != nullptr;
    onOff.setToggleState (on, juce::dontSendNotification);
    onOff.setVisible (true);
    experimentalToggle.setToggleState (service.showsExperimentalFeatures(), juce::dontSendNotification);
    experimentalToggle.setVisible (! formVisible && ! (subPage && on));
    experimentalNote.setVisible (experimentalToggle.isVisible());
    explanation.setVisible (! subPage || ! on);
    status.setVisible (! subPage || ! on);
    for (auto* c : std::initializer_list<juce::Component*> { &connectionsLabel, &list, &addButton, &testButton,
                                                             &removeButton, &localButton, &modelLabel, &modelBox,
                                                             &useButton, &patternsButton, &usage })
        c->setVisible (on && ! formVisible && ! subPage);
    manualSearchButton.setVisible (on && ! formVisible && ! subPage);
    engineButton.setVisible (on && ! formVisible && ! subPage);
    for (auto* c : std::initializer_list<juce::Component*> {
             modelList.get(), &modelsIntro, &modelsStatus, &downloadModelButton, &useModelButton, &keywordsButton,
             &removeModelButton, &addFolderButton, &addLinkButton, &backButton, &linkField })
        c->setVisible (on && modelsVisible);
    downloadBar.setVisible (on && modelsVisible && downloadJob.isNotEmpty());
    cancelDownloadButton.setVisible (on && modelsVisible && downloadJob.isNotEmpty());
    for (auto* b : { &downloadModelButton, &addLinkButton, &useModelButton, &removeModelButton })
        b->setEnabled (downloadJob.isEmpty());
    for (auto* c :
         std::initializer_list<juce::Component*> { &providerBox, &nameField, &keyField, &urlField, &providerLabel,
                                                   &nameLabel, &keyLabel, &urlLabel, &saveButton, &cancelButton })
        c->setVisible (on && formVisible && ! subPage);
    if (on && service.getState() == ai::AiService::State::failed)
        status.setText (service.getProblem(), juce::dontSendNotification);
    resized();
}

void AiSettingsDialog::selectedRowsChanged (int row)
{
    modelBox.clear (juce::dontSendNotification);
    if (! juce::isPositiveAndBelow (row, (int)connections.size()))
        return;
    const auto& c = connections[(size_t)row];
    for (int i = 0; i < c.models.size(); ++i)
        modelBox.addItem (c.models[i], i + 1);
    const auto current = assistantRoute.fromFirstOccurrenceOf (": ", false, false);
    modelBox.setText (assistantRoute.startsWith (c.name + ":") ? current
                      : c.models.isEmpty() ? juce::String (c.provider == "anthropic" ? "claude-sonnet-5-5" : "")
                                           : c.models[0],
                      juce::dontSendNotification);
}

void AiSettingsDialog::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int)connections.size()))
        return;
    const auto& theme = themeOf (*this);
    if (selected)
        g.fillAll (theme.get ("accent").withAlpha (0.25f));
    const auto& c = connections[(size_t)row];
    g.setColour (theme.get ("text"));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (c.name, 10, 2, width - 20, height / 2, juce::Justification::bottomLeft, true);
    g.setColour (theme.get ("textDim"));
    g.setFont (juce::FontOptions (11.0f));
    const auto keyText = c.provider == "local" ? juce::String ("no key needed")
                         : c.hasKey            ? juce::String ("key saved")
                                               : juce::String ("no key");
    const auto modelText =
        c.models.isEmpty() ? juce::String ("not tested yet") : juce::String (c.models.size()) + " models";
    g.drawText (providerName (c.provider) + " - " + keyText + " - " + modelText, 10, height / 2, width - 20,
                height / 2 - 2, juce::Justification::topLeft, true);
}

void AiSettingsDialog::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    for (auto* l : { &explanation, &connectionsLabel, &modelLabel, &providerLabel, &nameLabel, &keyLabel, &urlLabel })
        l->setColour (juce::Label::textColourId, theme.get ("text"));
    for (auto* l : std::initializer_list<juce::Label*> { &status, &usage, &experimentalNote })
        l->setColour (juce::Label::textColourId, theme.get ("textDim"));
    for (auto* t : { &onOff, &experimentalToggle })
        t->setColour (juce::ToggleButton::textColourId, theme.get ("text"));
}

void AiSettingsDialog::layOutModels (juce::Rectangle<int> area)
{
    modelsIntro.setBounds (area.removeFromTop (60));
    modelsStatus.setBounds (area.removeFromTop (40));
    if (downloadBar.isVisible())
    {
        auto row = area.removeFromTop (26);
        cancelDownloadButton.setBounds (row.removeFromRight (80));
        row.removeFromRight (6);
        downloadBar.setBounds (row);
    }
    area.removeFromTop (6);
    auto bottom = area.removeFromBottom (32);
    backButton.setBounds (bottom.removeFromRight (80));
    addFolderButton.setBounds (bottom.removeFromLeft (130));
    area.removeFromBottom (6);
    auto linkRow = area.removeFromBottom (30);
    addLinkButton.setBounds (linkRow.removeFromRight (60).reduced (0, 2));
    linkField.setBounds (linkRow.reduced (0, 2).withTrimmedRight (6));
    area.removeFromBottom (6);
    auto buttons = area.removeFromBottom (32);
    for (auto* b : { &downloadModelButton, &useModelButton, &keywordsButton, &removeModelButton })
    {
        b->setBounds (buttons.removeFromLeft (b == &keywordsButton ? 120 : 90));
        buttons.removeFromLeft (8);
    }
    area.removeFromBottom (6);
    modelList->setBounds (area);
}

void AiSettingsDialog::resized()
{
    auto area = getLocalBounds().reduced (margin);
    onOff.setBounds (area.removeFromTop (28));
    if (enginePage != nullptr)
    {
        enginePage->setBounds (area.withTrimmedTop (6));
        return;
    }
    if (modelsVisible && service.isEnabled())
    {
        layOutModels (area.withTrimmedTop (6));
        return;
    }
    explanation.setBounds (area.removeFromTop (92));
    area.removeFromTop (6);
    status.setBounds (area.removeFromBottom (52));
    area.removeFromBottom (6);
    if (experimentalToggle.isVisible())
    {
        experimentalNote.setBounds (area.removeFromBottom (36));
        experimentalToggle.setBounds (area.removeFromBottom (24));
        area.removeFromBottom (8);
    }

    if (formVisible)
    {
        area.removeFromTop (10);
        auto row = [&area] (juce::Label& label, juce::Component& field)
        {
            auto r = area.removeFromTop (30);
            label.setBounds (r.removeFromLeft (90));
            field.setBounds (r.reduced (0, 2));
            area.removeFromTop (6);
        };
        row (providerLabel, providerBox);
        row (nameLabel, nameField);
        row (keyLabel, keyField);
        row (urlLabel, urlField);
        auto buttons = area.removeFromTop (32);
        saveButton.setBounds (buttons.removeFromLeft (140));
        buttons.removeFromLeft (8);
        cancelButton.setBounds (buttons.removeFromLeft (90));
        return;
    }

    connectionsLabel.setBounds (area.removeFromTop (22));
    // The tools on the right; this month's use on a row of its own above them, the whole width.
    auto toolsRow = area.removeFromBottom (36);
    engineButton.setBounds (toolsRow.removeFromRight (140).reduced (0, 3));
    toolsRow.removeFromRight (8);
    manualSearchButton.setBounds (toolsRow.removeFromRight (140).reduced (0, 3));
    usage.setBounds (area.removeFromBottom (36));
    auto modelRow = area.removeFromBottom (30);
    modelLabel.setBounds (area.removeFromBottom (22));
    useButton.setBounds (modelRow.removeFromRight (160).reduced (0, 2));
    modelRow.removeFromRight (6);
    patternsButton.setBounds (modelRow.removeFromRight (130).reduced (0, 2));
    modelRow.removeFromRight (8);
    modelBox.setBounds (modelRow.reduced (0, 2));
    area.removeFromBottom (8);
    auto buttons = area.removeFromBottom (32);
    for (auto* b : { &addButton, &testButton, &removeButton })
    {
        b->setBounds (buttons.removeFromLeft (b == &addButton ? 90 : 80));
        buttons.removeFromLeft (8);
    }
    localButton.setBounds (buttons.removeFromRight (160));
    area.removeFromBottom (6);
    list.setBounds (area);
}

//==============================================================================
namespace
{
juce::Component::SafePointer<juce::DocumentWindow>& openWindow()
{
    static juce::Component::SafePointer<juce::DocumentWindow> window;
    return window;
}

class Window final : public juce::DocumentWindow
{
public:
    Window (AiSettingsDialog* dialog, juce::Component* owner)
        : DocumentWindow ("AI", juce::Colour (0xff1e2227), DocumentWindow::closeButton)
    {
        if (owner != nullptr)
            if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&owner->getLookAndFeel()))
            {
                setBackgroundColour (lf->getTheme().get ("panel"));
                dialog->setLookAndFeel (lf);
            }
        setUsingNativeTitleBar (true);
        setContentOwned (dialog, true);
        setResizable (false, false);
        if (owner != nullptr)
            centreAroundComponent (owner->getTopLevelComponent(), getWidth(), getHeight());
        else
            centreWithSize (getWidth(), getHeight());
        setVisible (true);
        if (owner != nullptr)
            keepAboveOwner (*this, *owner->getTopLevelComponent());
    }

    ~Window() override
    {
        if (auto* content = getContentComponent())
            content->setLookAndFeel (nullptr);
        clearContentComponent();
    }

    void closeButtonPressed() override { delete this; }
};
} // namespace

void AiSettingsDialog::show (ai::AiService& service, juce::Component* centreAround)
{
    if (auto* existing = openWindow().getComponent())
    {
        existing->toFront (true);
        return;
    }
    openWindow() = new Window (new AiSettingsDialog (service), centreAround);
}

void AiSettingsDialog::showAnalysisEngineIfOpen()
{
    if (auto* window = openWindow().getComponent())
        if (auto* dialog = dynamic_cast<AiSettingsDialog*> (window->getContentComponent()))
            dialog->showAnalysisEngine (true);
}

void AiSettingsDialog::closeWindow()
{
    if (auto* window = openWindow().getComponent())
        delete window;
}

} // namespace spinus::ui
