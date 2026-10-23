#pragma once

#include "ai/AiService.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "MessageLabel.h"

namespace spinus::ui
{

/** Options › AI › Analysis engine: PyTorch, which song analysis needs.

    Shows the graphics card found, the PyTorch build that suits it (and the CPU-only build), whether each
    is tested on that kind of GPU, its download and disk size, and installs or removes it. Nothing is
    downloaded until the user agrees to the size. "Use the CPU only" and "Test" are here too. */
class AnalysisEnginePage final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit AnalysisEnginePage (ai::AiService&);
    ~AnalysisEnginePage() override;

    std::function<void()> onBack;

    /** Shows a question (an AlertWindow unless replaced, e.g. by tests); `done` gets 0 for the first
        button, 1 for the second. */
    using MessageBoxHandler = std::function<void (const juce::MessageBoxOptions&, std::function<void (int)>)>;
    void setMessageBoxHandler (MessageBoxHandler h) { messageBox = std::move (h); }

    struct BuildInfo
    {
        juce::String id, label, vendor;
        juce::int64 download = 0, disk = 0, size = 0;
        bool tested = false, installed = false, outdated = false, installing = false;
    };

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.
    void refresh();
    void askToInstall (int row); // asks the user to agree to the size first
    void install (int row);
    void cancelInstall();
    void remove (int row);
    void setForceCpu (bool);
    void runTest();

    const std::vector<BuildInfo>& getBuilds() const noexcept { return builds; }
    juce::String getGpuText() const { return gpuText.getText(); }
    juce::String getUsesText() const { return usesText.getText(); }
    juce::String getStatusText() const { return statusText.getText(); }
    bool isInstalling() const noexcept { return job.isNotEmpty() && ! testing; }
    bool isBusy() const noexcept { return job.isNotEmpty() || waiting > 0; }
    juce::String getButtonText (int row) const;

    /** "1.8 GB" / "220 MB". */
    static juce::String formatSize (juce::int64 bytes);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void call (const juce::String& method, const juce::var& params, std::function<void (const juce::var&)> onResult);
    void show (const juce::var& status);
    void updateButtons();

    int getNumRows() override { return (int)builds.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override { updateButtons(); }

    ai::AiService& service;
    MessageBoxHandler messageBox;
    int listener = 0;
    std::vector<BuildInfo> builds;
    juce::String job; // the install or test running ("starting" until the service answers)
    bool testing = false;
    bool hasUv = true;
    int waiting = 0;
    double progress = -1.0;

    MessageLabel statusText;
    juce::Label intro, gpuText, usesText, folderText;
    juce::ListBox list { "PyTorch builds", this };
    juce::TextButton installButton { "Install" }, removeButton { "Remove" }, cancelButton { "Cancel" },
        testButton { "Test" }, backButton { "Back" };
    juce::ToggleButton forceCpu { "Use the CPU only" };
    juce::ProgressBar bar { progress };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace spinus::ui
