#pragma once

#include "core/FileDialog.h"
#include "AnalysisEnginePage.h"
#include "ai/AiService.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "MessageLabel.h"

namespace spinus::ui
{

/** Options → AI: turns AI on or off, and manages the connections to model
    providers (Anthropic, OpenRouter, any OpenAI-compatible service, a local server such as Ollama),
    which model the assistant uses, and this month's usage.

    AI is off until the user turns it on here; the window says plainly what is sent where. Keys go to the
    system keyring through the AI service and are never shown again. */
class AiSettingsDialog final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit AiSettingsDialog (ai::AiService&);
    ~AiSettingsDialog() override;

    static void show (ai::AiService&, juce::Component* centreAround);
    /** Turns the open Options > AI window to its Analysis engine page. */
    static void showAnalysisEngineIfOpen();
    static void closeWindow();

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously; refresh() reloads everything.
    void setAiOn (bool);
    /** Show experimental features: the Analyzer appears (or closes) in the main window at once. */
    void setShowExperimental (bool);
    juce::String getExperimentalNote() const { return experimentalNote.getText(); }
    void addConnection (const juce::String& provider, const juce::String& name, const juce::String& key,
                        const juce::String& baseUrl);
    void testConnection (int row);
    void removeConnection (int row);
    void findLocalServers();
    void useModel (int row, const juce::String& model, const juce::String& feature = "assistant");
    void refresh();

    //==============================================================================
    // Manual search: local embedding models.
    struct ModelInfo
    {
        juce::String id, name, description, license, languages, source;
        juce::int64 size = 0;
        bool downloaded = false, own = false, pinned = true, active = false;
        juce::StringArray missing; // packages to install first
    };
    void showModels (bool);
    void refreshModels();
    void downloadModel (int row);
    void useEmbeddingModel (int row); // -1: keywords only
    void removeEmbeddingModel (int row);
    void addModelFolder (const juce::File& folder, const juce::String& name);
    void addModelLink (const juce::String& link);
    const std::vector<ModelInfo>& getModels() const noexcept { return models; }
    juce::String getModelsState() const { return modelsState; }
    bool isDownloading() const noexcept { return downloadJob.isNotEmpty(); }

    //==============================================================================
    // The analysis engine: PyTorch for song analysis.
    void showAnalysisEngine (bool);
    AnalysisEnginePage* getAnalysisEnginePage() const noexcept { return enginePage.get(); }

    struct ConnectionInfo
    {
        juce::String id, name, provider, baseUrl;
        bool hasKey = false;
        juce::StringArray models;
    };
    const std::vector<ConnectionInfo>& getConnections() const noexcept { return connections; }
    int getSelectedConnection() const { return list.getSelectedRow(); }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getAssistantRoute() const { return assistantRoute; }
    /** The pattern generator's own model ("connection: model"), or empty when it uses the assistant's. */
    juce::String getPatternsRoute() const { return patternsRoute; }
    bool isWaiting() const noexcept { return waiting > 0; }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void call (const juce::String& method, const juce::var& params, std::function<void (const juce::var&)> onResult);
    void updateVisibility();
    void showAddForm (bool);

    int getNumRows() override { return (int)connections.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override;

    ai::AiService& service;
    int stateListener = 0;
    std::vector<ConnectionInfo> connections;
    juce::String selectAfterRefresh; // a connection just added, selected once the list has it
    juce::String assistantRoute, patternsRoute;
    int waiting = 0;

    juce::ToggleButton onOff { "Turn on AI features" };
    juce::ToggleButton experimentalToggle { "Show experimental features" };
    juce::Label experimentalNote;
    MessageLabel status;
    juce::Label explanation, connectionsLabel, modelLabel, usage;
    juce::ListBox list { "AI connections", this };
    juce::TextButton addButton { "Add..." }, testButton { "Test" }, removeButton { "Remove" },
        localButton { "Find local servers" }, useButton { "Use for the assistant" },
        patternsButton { "Use for patterns" };
    juce::ComboBox modelBox;

    // The "Add" form.
    juce::ComboBox providerBox;
    juce::TextEditor nameField, keyField, urlField;
    juce::Label providerLabel, nameLabel, keyLabel, urlLabel;
    juce::TextButton saveButton { "Save and test" }, cancelButton { "Cancel" };
    bool formVisible = false;

    // The manual search page.
    class ModelList;
    void layOutModels (juce::Rectangle<int>);
    void startModelJob (const juce::String& method, const juce::var& params, const juce::String& what);
    bool modelsVisible = false;
    std::vector<ModelInfo> models;
    juce::String modelsState, downloadJob;
    int jobListener = 0;
    double downloadProgress = -1.0;
    std::unique_ptr<ModelList> modelListModel;
    std::unique_ptr<juce::ListBox> modelList;
    juce::Label modelsIntro, modelsStatus;
    std::unique_ptr<AnalysisEnginePage> enginePage;
    juce::TextButton engineButton { "Analysis engine..." };
    juce::TextButton manualSearchButton { "Manual search..." }, downloadModelButton { "Download" },
        useModelButton { "Use" }, keywordsButton { "Keywords only" }, removeModelButton { "Remove" },
        addFolderButton { "Add a folder..." }, addLinkButton { "Add" }, backButton { "Back" },
        cancelDownloadButton { "Cancel" };
    juce::TextEditor linkField;
    juce::ProgressBar downloadBar { downloadProgress };
    std::unique_ptr<core::FileDialog> chooser;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace spinus::ui
