#pragma once

#include "core/FileDialog.h"
#include "TypingKeyboard.h"
#include "HintBar.h"
#include "Theme.h"
#include "WindowLayouts.h"
#include "Workspace.h"
#include "commands/CommandHistory.h"
#include "plugins/PluginDownloads.h"
#include "plugins/windows/WindowsPlugins.h"
#include "ai/AiService.h"
#include "session/ProjectDocument.h"

namespace spinus::ui
{

/** The main window's content: menu bar, top bar, browser (left), workspace with the
    floating panels (centre), assistant (right), hint bar (bottom). Owns the app's keyboard commands,
    with FL Studio's default shortcuts, and the File menu's project handling when given a
    ProjectDocument (without one, e.g. for snapshots, the project commands are disabled).
*/
class Browser;

class MainView : public juce::Component,
                 public juce::ApplicationCommandTarget,
                 public juce::DragAndDropContainer,
                 private juce::MenuBarModel,
                 private juce::ChangeListener
{
public:
    enum CommandIds
    {
        showPlaylist = 0x1001,
        showChannelRack,
        showPianoRoll,
        showMixer,
        showHistory,
        showAnalyzer,
        toggleBrowser,
        toggleAssistant,
        undo,
        redo,
        playStop,
        togglePlayMode,
        toggleMetronome,
        showAudioSettings,
        exportAudio,
        newProject,
        openProject,
        saveProject,
        saveProjectAs,
        quit,
        toggleRecord,
        toggleRecordQuantize,
        getFreePlugins,
        windowsPlugins,
        aiSettings,
        toggleTypingKeyboard,
        automateLastMoved,
        showAutomixer,
        showLauncher,
        showAutomationEditor,
        showAudioTracks,
        editShortcuts,
        layout1, // the first three layouts: Compose, Arrange, Mix
        layout2,
        layout3,
        showRouting,
        toolDraw, // the Piano Roll's or Playlist's mouse tool (FL's keys: P, B, C, E)
        toolPaint,
        toolSlice,
        toolSelect,
        showManual,    // Help › Manual (F1)
        showAbout,     // Help › About Spinus
        openLogFolder, // Help › Open log folder: the session logs (core::Logs)
        layout4,       // the user's own layouts, in the order saved (Ctrl+Alt+4 to 9)
        layout5,
        layout6,
        layout7,
        layout8,
        layout9,
    };

    /** Which layout a layout command opens: 0 for Compose (layout1) to 8 (layout9); -1 for other commands. */
    static int layoutIndexOf (juce::CommandID);
    static juce::CommandID layoutCommand (int index);

    explicit MainView (Session&, ProjectDocument* document = nullptr);
    ~MainView() override;

    juce::ApplicationCommandManager& getCommandManager() noexcept { return commandManager; }
    /** Where the user's own keyboard shortcuts are kept (only those that differ from Spinus's): loaded now,
        saved whenever one changes (Options › Keyboard shortcuts). */
    void setShortcutsFile (const juce::File&);

    /** Where window layouts are kept: loaded now, and the layout Spinus had when it last closed
        comes back. The current one is kept there when Spinus closes. */
    void setLayoutsFile (const juce::File&);
    WindowLayouts& getWindowLayouts() noexcept { return *layouts; }
    /** Opens and places the panels of a layout; false if there is none of that name. */
    bool useLayout (const juce::String& name);
    /** The windows as they are now, as a layout. */
    juce::ValueTree getCurrentLayout() const;
    Workspace& getWorkspace() noexcept { return workspace; }
    HintBar& getHintBar() noexcept { return hintBar; }
    Browser& getBrowser() noexcept { return *browser; }
    bool isBrowserVisible() const noexcept { return browserVisible; }
    bool isAssistantVisible() const noexcept { return assistantVisible; }
    /** The assistant's width (dragging the bar on its left), between 220 and half the window. */
    int getAssistantWidth() const noexcept { return assistantWidth; }
    void setAssistantWidth (int width)
    {
        assistantWidth = width;
        resized();
    }

    void setTheme (Theme);

    /** Picks a mouse tool (0 draw, 1 paint, 2 slice, 3 select) in the Piano Roll if it is in front (or the
        Playlist isn't), otherwise in the Playlist. False if neither is open. */
    bool chooseTool (int tool);

    //==============================================================================
    // Project handling (File menu). Questions and file choices go through replaceable handlers so tests
    // can answer them.

    /** Asks to save unsaved changes (Save / Don't save / Cancel), then calls `then` unless cancelled. */
    void askToSaveChanges (const std::function<void()>& then);

    /** Asks to save changes, then closes the app (the window's close button, Ctrl+Q). */
    void quitApp();

    /** Offers the newest autosave of a project whose last session crashed, if any (on startup). */
    void offerRecovery();

    /** Spinus closed unexpectedly last time while this plugin was working: offers to run it in
        a separate process from now on. `then` runs after the answer. */
    void offerToRunSeparately (const juce::String& key, const juce::String& name, const std::function<void()>& then);

    /** A plugin running in its own process stopped working: says so and offers to reload it.
        Called when the Engine Adapter reports one. */
    void showPluginFailure (model::DeviceId);

    /** Without questions: used by the menus after any questions, and by tests. Problems are shown in a
        message box. */
    bool openProjectFile (const juce::File&);
    bool saveProjectTo (const juce::File& folder);

    /** Options → Get free plugins: where the open-source plugins are downloaded to, and where the bundled
        devices look for them (PluginDownloads::getSearchFolders). Not set: the command is disabled. */
    void setPluginDownloads (const plugins::PluginDownloads*, std::vector<juce::File> searchFolders);

    /** Opens the Get free plugins window (with just `pack` ticked, if given). Downloaded plugins work at
        once: the Browser lists them and devices waiting for them are created. */
    void showPluginDownloads (const juce::String& pack = {});

    /** AI: the service behind Options > AI and the assistant panel. */
    void setAiService (ai::AiService*);
    void showAiSettings();

    /** Options > AI > Show experimental features (off without an AI service). While it is off the Analyzer is
        nowhere: not in the View menu, the keyboard shortcuts or a layout, and its command does nothing. */
    bool areExperimentalFeaturesShown() const noexcept { return experimentalShown; }
    /** False for the commands of experimental features while they are hidden. */
    bool isCommandAvailable (juce::CommandID) const;

    /** Windows plugins: the manager and the plugin scanner program it tests with. */
    void setWindowsPlugins (plugins::WindowsPlugins*, juce::File scannerExecutable);
    /** Options → Windows plugins. `retestNow`: the system's Wine changed since its last test. */
    void showWindowsPlugins (bool retestNow = false);

    /** True if some free plugin pack is neither downloaded nor installed on the system. */
    bool arePluginDownloadsMissing() const;

    /** The window title: "Song - Spinus", with "*" after the name while there are unsaved edits. */
    juce::String getWindowTitle() const;

    using MessageBoxHandler = std::function<void (const juce::MessageBoxOptions&, std::function<void (int)>)>;
    using FileChooserHandler = std::function<void (const juce::String& title, const juce::File& start, bool save,
                                                   std::function<void (juce::File)>)>;
    void setMessageBoxHandler (MessageBoxHandler h) { messageBox = std::move (h); }
    void setFileChooserHandler (FileChooserHandler h) { chooseFile = std::move (h); }
    std::function<void()> onQuit; // called when the user confirmed quitting

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

    /** The hint of the top bar button with this text (tests). */
    juce::String getTopBarHelpText (const juce::String& buttonText) const;
    /** True when two top bar buttons overlap or one is squeezed out (tests). */
    bool getTopBarButtonsOverlap() const;

    /** The computer keyboard as a piano (Ctrl+T), playing the selected channel. */
    TypingKeyboard& getTypingKeyboard() noexcept { return typingKeyboard; }

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int, int) override;

    /** The Browser's "<Name> (demo)": copies that shipped demo song to the Music folder (or finds the copy made
        before) and opens the copy (src/session/DemoSong.h); the Browser asks to save changes first. False if it
        could not be copied or opened (the problem is shown). */
    bool openDemo (const juce::File& bundledSong);
    /** The same with the first demo song (Siskin). */
    bool openTheDemoSong();
    /** The shipped demo songs, Siskin first. */
    juce::Array<juce::File> getDemoSongs() const;
    /** Where the shipped demo songs are (the folder holding them) and where the user's copies go (tests); empty:
        the usual ones. */
    void setDemoFolders (juce::File demoFolder, juce::File copyParent);

    /** Help › Manual opens the manual from this folder (tests); empty: the installed one (ui::Manual::findFolder). */
    void setManualFolder (juce::File folder) { manualFolder = std::move (folder); }

    /** The current pattern's actions (new, rename, colour, delete), added to a menu: the pattern list's. */
    void addPatternActions (juce::PopupMenu&);

private:
    class TopBar;
    class Dock;

    void historyChanged();
    void addPattern();
    void useDevice (const plugins::DeviceList::Entry&);
    /** Offers the free plugins an opened project uses that are not downloaded here; true if it did. */
    bool offerMissingDownloads();
    bool offeredDownloads = false;                                   // by the last project opened
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the document
    void parentHierarchyChanged() override;
    void updateTitle();
    void showProblem (const juce::String& title, const juce::String& message);
    void askToRecover (const ProjectDocument::Recovery&, const std::function<void()>& otherwise);
    void saveThen (const std::function<void()>& then);
    void saveAsThen (const std::function<void()>& then);
    void chooseProjectToOpen();
    juce::File getDefaultProjectsFolder() const;

    Session& session;
    ProjectDocument* document;
    commands::CommandHistory& history;
    int historyListener = 0;
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    juce::ApplicationCommandManager commandManager;
    struct ShortcutsSaver final : juce::ChangeListener
    {
        explicit ShortcutsSaver (MainView& v) : view (v) {}
        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        MainView& view;
    };
    juce::File shortcutsFile;
    std::unique_ptr<WindowLayouts> layouts = std::make_unique<WindowLayouts>();
    bool keepLastLayout = false;
    void addLayoutsMenu (juce::PopupMenu&);
    void applyLayout (const juce::ValueTree&);
    std::unique_ptr<ShortcutsSaver> shortcutsSaver;

    const plugins::PluginDownloads* downloads = nullptr;
    plugins::WindowsPlugins* windows = nullptr;
    juce::File pluginScanner;
    std::vector<juce::File> downloadSearchFolders;
    juce::File demoBundled, demoCopyParent, manualFolder;
    juce::File listedFolder; // the project last added to the Browser's projects (opened or saved)
    void listOpenProject();

    MessageBoxHandler messageBox;
    std::unique_ptr<juce::TimedCallback> pluginWatch; // plugins in their own process that stopped working
    /** Keeps the windows' layout for the next start while Spinus runs, not only when it closes: a Spinus that
        is ended (or hangs) still comes back with the windows as they were. */
    std::unique_ptr<juce::TimedCallback> layoutKeeper;
    juce::ValueTree keptLayout;
    FileChooserHandler chooseFile;
    std::unique_ptr<core::FileDialog> activeChooser;

    juce::MenuBarComponent menuBar;
    std::unique_ptr<TopBar> topBar;
    std::unique_ptr<Browser> browser;
    std::unique_ptr<juce::Component> assistant; // the AssistantPanel once AI is wired in, else a placeholder
    ai::AiService* aiService = nullptr;
    int aiStateListener = 0;
    /** Record idea and Generate show only while AI is on. */
    void updateAiButtons();
    void updateExperimentalFeatures();
    bool experimentalShown = false;
    std::unique_ptr<class RecordIdea> recordIdea;
    std::unique_ptr<class PatternGenerator> patternGenerator;
    std::unique_ptr<juce::DocumentWindow> spinusTune;

public:
    /** Opens an audio clip in Spinus Tune's window (graphical mode). */
    void openInSpinusTune (model::ClipId);

public:
    /** Shows an automation clip in the Automation editor (and the Playlist behind it). */
    void openAutomationClip (model::ClipId);

    /** Adds audio files as new audio tracks: each on the Playlist at the song's start, below the clips
        there. `fromRack`: they were dropped on the Channel Rack and are too long for a Sampler. */
    void addAudioTracks (const juce::StringArray&, bool fromRack = false);

    /** Makes a Sampler channel in the Channel Rack of an audio track's file, then removes the track and
        its clips (two undo steps). */
    bool moveAudioTrackToRack (model::ChannelId, const juce::File&);

    /** Adds a Beat Slicer channel with this file and opens it (the Browser's and Audio tracks' "Open in
        Beat Slicer"). */
    bool openInBeatSlicer (const juce::File&);

private:
private:
    Workspace workspace;
    HintBar hintBar;
    TypingKeyboard typingKeyboard { [this] (int note, bool down)
                                    { session.getRecorder().playComputerKey (note, down); } };
    bool browserVisible = true, assistantVisible = false;
    int assistantWidth = 300;

    /** The bar left of the assistant: drag it to make the assistant wider or narrower. */
    struct WidthHandle final : juce::Component
    {
        explicit WidthHandle (MainView& v) : view (v)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            setHelpText ("Drag to make the assistant wider or narrower");
        }
        void mouseDown (const juce::MouseEvent&) override { start = view.assistantWidth; }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            view.setAssistantWidth (start - e.getDistanceFromDragStartX());
        }
        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colours::grey.withAlpha (isMouseOverOrDragging() ? 0.6f : 0.2f));
            g.fillRect (getLocalBounds().reduced (1, 0));
        }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }
        MainView& view;
        int start = 300;
    };
    WidthHandle assistantResizer { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
};

} // namespace spinus::ui
