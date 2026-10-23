#pragma once

#include "core/FileDialog.h"
#include "plugins/windows/WindowsPlugins.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Options → Windows plugins: everything about Windows plugins in one window.

    Not set up yet: "Set up" finds the Wine and yabridge on this computer and tests them; if there are
    none or they fail, it offers Spinus's tested versions to download. Then Spinus's Windows folder is
    made and tested.

    Ready: "Add a plugin or installer..." takes an installer (.exe, .msi) or a plugin file (.dll, .vst3,
    .clap); the plugins found are listed, each with Remove; the health check shows the Wine in use, the last
    test (with Retest), the other runtime to switch to, drawing plugin windows without the graphics card,
    and real-time privileges.

    In the Flatpak, only Spinus's tested Wine can be used (the computer's own is outside the sandbox): Set up
    offers its download.

    Long steps run on a background thread with progress and Cancel; Spinus keeps working meanwhile. */
class WindowsPluginsDialog final : public juce::Component, private juce::Timer, private juce::ListBoxModel
{
public:
    WindowsPluginsDialog (plugins::WindowsPlugins&, juce::File scannerExecutable);
    ~WindowsPluginsDialog() override; // cancels a running step

    /** Opens the dialog in its own window (not modal), or brings it to the front. `onPluginsChanged`
        runs when plugins were added or the runtime changed (Spinus scans the bridged folders then). */
    static void show (plugins::WindowsPlugins&, juce::File scannerExecutable, juce::Component* centreAround,
                      std::function<void()> onPluginsChanged, bool retestNow = false,
                      std::function<juce::StringArray (const juce::Array<juce::File>&)> whoUses = {});
    static void closeWindow();

    std::function<void()> onPluginsChanged;
    /** What in the open project uses a plugin known by these files (bridged/ entries): names to show, empty
        if nothing. A plugin in use is not removed. */
    std::function<juce::StringArray (const juce::Array<juce::File>&)> whoUses;

    //==============================================================================
    // For the buttons and for tests.
    enum class Page
    {
        notSetUp,
        offerDownload,
        ready
    };
    Page getPage() const noexcept { return page; }
    bool isBusy() const;
    juce::String getStatusText() const { return status.getText(); }
    juce::StringArray getPluginNames() const;

    void setUp();
    void downloadRuntime();
    void retest();
    void switchRuntime();
    void addPlugin (const juce::File& installerOrPlugin);
    /** Remove: asks first (naming the plugin), then deletes it; `confirmed` skips the question (tests). A
        plugin the open project uses is refused, with a message. */
    void removePlugin (int row, bool confirmed = false);
    void setDrawingWithoutGraphicsCard (bool);
    void cancel();
    /** Waits for a running step and applies its result (tests). */
    void finishForTests();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Job;
    struct Outcome
    {
        juce::String error;                                  // empty: worked
        std::optional<plugins::WindowsPlugins::Runtime> use; // the runtime to switch to
        juce::String message;                                // what to tell the user
        bool offerDownload = false;
    };

    void start (const juce::String& firstStage, std::function<Outcome (Job&)>);
    void finished (const Outcome&);
    void refresh();
    void chooseFile();
    void timerCallback() override;

    int getNumRows() override { return (int)found.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;
    class Row;

    plugins::WindowsPlugins& windows;
    juce::File scanner;
    Page page = Page::notSetUp;
    std::vector<plugins::WindowsPlugins::Found> found;
    juce::String downloadReason;
    juce::String lastMessage; // the last step's result, shown until the next step

    juce::Label intro, status, health, realtime;
    juce::TextButton setUpButton { "Set up Windows plugins" }, downloadButton,
        addButton { "Add a plugin or installer..." }, retestButton { "Retest" }, switchButton,
        folderButton { "Open Windows folder" }, cancelButton { "Cancel" };
    juce::ToggleButton drawingToggle { "Draw plugin windows without the graphics card (try this if a plugin's "
                                       "window does not update)" };
    juce::ListBox list { "Windows plugins", this };
    double progress = -1.0;
    juce::ProgressBar progressBar { progress };
    std::unique_ptr<Job> job;
    std::unique_ptr<core::FileDialog> chooser;
};

} // namespace spinus::ui
