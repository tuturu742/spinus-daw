#include "MainView.h"

#include "AskForName.h"
#include "ColourMenu.h"

#include "Analyzer.h"
#include "AudioTracks.h"
#include "AutomationEditor.h"
#include "Automixer.h"
#include "Launcher.h"

#include "AudioSettings.h"
#include "engine/EngineHost.h"
#include "session/RecordingTime.h"
#include "Browser.h"
#include "ChannelRack.h"
#include "ExportDialog.h"
#include "HistoryPanel.h"
#include "RoutingView.h"
#include "Logo.h"
#include "Mixer.h"
#include "PianoRoll.h"
#include "PatternGenerator.h"
#include "RecordIdea.h"
#include "model/Scales.h"
#include "Shortcuts.h"
#include "SpinusTune.h"
#include "Playlist.h"
#include "PluginDownloadDialog.h"
#include "WindowsPluginsDialog.h"
#include "About.h"
#include "AiSettingsDialog.h"
#include "Manual.h"
#include "AssistantPanel.h"
#include "PluginWindow.h"
#include "TempoControl.h"
#include "commands/CommandRegistry.h"
#include "core/Log.h"
#include "model/StockDevices.h"
#include "session/DemoSong.h"
#include "session/ProblemReport.h"
#include "session/SystemInfo.h"
#include "session/ProgramFolder.h"

#include <SpinusVersion.h>

namespace spinus::ui
{

namespace
{
/** Options > Keyboard shortcuts: JUCE's editor, without the commands of hidden experimental features. */
class ShortcutsEditor final : public juce::KeyMappingEditorComponent
{
public:
    ShortcutsEditor (juce::KeyPressMappingSet& keys, std::function<bool (juce::CommandID)> include)
        : KeyMappingEditorComponent (keys, true), included (std::move (include))
    {
    }
    bool shouldCommandBeIncluded (juce::CommandID id) override
    {
        return included (id) && KeyMappingEditorComponent::shouldCommandBeIncluded (id);
    }

private:
    std::function<bool (juce::CommandID)> included;
};

constexpr int menuBarHeight = 24, topBarHeight = 40, hintBarHeight = 22, browserWidth = 220;
constexpr int recentMenuBase = 0x2000, clearRecentMenuId = 0x2100;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}
} // namespace

//==============================================================================
class MainView::TopBar final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit TopBar (MainView& v) : view (v)
    {
        setHelpText ("Top bar: transport, pattern, tempo, panels");

        for (auto [button, id, text] :
             { std::tuple { &playButton, (int)playStop, "" }, std::tuple { &recordButton, (int)toggleRecord, "" },
               std::tuple { &modeButton, (int)togglePlayMode, "" },
               std::tuple { &metronomeButton, (int)toggleMetronome, "" },
               std::tuple { &typingButton, (int)toggleTypingKeyboard, "" },
               std::tuple { &settingsButton, (int)showAudioSettings, "" },
               std::tuple { &playlistButton, (int)showPlaylist, "Playlist" },
               std::tuple { &rackButton, (int)showChannelRack, "Rack" },
               std::tuple { &audioButton, (int)showAudioTracks, "Audio" },
               std::tuple { &pianoButton, (int)showPianoRoll, "Piano Roll" },
               std::tuple { &mixerButton, (int)showMixer, "Mixer" },
               std::tuple { &browserButton, (int)toggleBrowser, "Browser" },
               std::tuple { &assistantButton, (int)toggleAssistant, "Assistant" } })
        {
            button->setCommandToTrigger (&view.commandManager, id, true);
            button->setClickingTogglesState (false);
            if (juce::String (text).isNotEmpty())
                button->setButtonText (text);
            addAndMakeVisible (button);
        }

        playButton.setHelpText ("Play or stop (Space)");
        recordButton.setHelpText ("Record: when on, playing records your MIDI keyboard (or the computer keyboard, "
                                  "Ctrl+T) into the current pattern (R)");
        modeButton.setHelpText ("PAT plays the current pattern over and over; SONG plays the playlist (L)");
        metronomeButton.setHelpText ("Metronome: a click on every beat while playing (Ctrl+M)");
        typingButton.setHelpText ("Typing keyboard to piano: the computer keyboard plays the selected channel, Z-M "
                                  "and Q-U (Ctrl+T)");
        settingsButton.setHelpText ("Audio settings: sound card, sample rate, buffer size (F10)");
        playlistButton.setHelpText ("Playlist: arrange patterns and audio into the song (F5)");
        rackButton.setHelpText ("Channel Rack: the instruments and their steps (F6)");
        audioButton.setHelpText ("Audio tracks: vocals, stems and other long audio (Shift+F6)");
        pianoButton.setHelpText ("Piano Roll: the selected channel's notes (F7)");
        mixerButton.setHelpText ("Mixer: volumes, effects and routing (F9)");
        browserButton.setHelpText ("Browser: instruments, effects and plugins to add (F8)");
        metronomeButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x99\xa9")); // ♩
        typingButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x8c\xa8"));    // ⌨
        recordButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x97\x8f"));    // ●
        settingsButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x9a\x99"));  // ⚙

        patternBox.setHelpText ("Current pattern: what pattern mode plays and the Channel Rack edits. Its list also "
                                "renames, colours and deletes it");
        patternBox.setTextWhenNoChoicesAvailable ("No patterns");
        patternBox.setTextWhenNothingSelected ("No pattern");
        patternBox.onChange = [this]
        {
            if (auto id = patternBox.getSelectedId(); id > 0)
                view.session.setCurrentPattern (model::PatternId { (uint32_t)id });
        };
        addAndMakeVisible (patternBox);

        newPatternButton.setButtonText ("+");
        newPatternButton.setHelpText ("Add a new pattern");
        newPatternButton.onClick = [this] { view.addPattern(); };
        addAndMakeVisible (newPatternButton);

        tempo.onCommit = [this] (double bpm)
        {
            juce::String error;
            auto command = commands::CommandRegistry::get().create (
                "project.setTempo", juce::JSON::parse ("{\"bpm\": " + juce::String (bpm, 2) + "}"), error);
            if (command != nullptr)
                view.session.getHistory().perform (std::move (command));
        };
        addAndMakeVisible (tempo);

        signatureButton.setHelpText ("Time signature: click to set the song's, or give the current pattern its own");
        signatureButton.onClick = [this] { showSignatureMenu(); };
        addAndMakeVisible (signatureButton);

        keyButton.setHelpText ("The song's key (click to choose): Spinus Tune, Generate, Record idea and the Piano "
                               "Roll's scale highlight use it. It does not change any notes");
        keyButton.onClick = [this] { showKeyMenu(); };
        addAndMakeVisible (keyButton);

        view.session.addChangeListener (this);
        refresh();
        startTimerHz (30);
    }

    ~TopBar() override { view.session.removeChangeListener (this); }

    /** Updates everything that mirrors the project or the session. */
    void refresh()
    {
        const auto& project = view.session.getProject();
        tempo.setTempo (project.settings.tempoBpm);
        static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        keyButton.setButtonText (project.settings.key
                                     ? "Key: " + juce::String (noteNames[project.settings.key->tonic]) + " " +
                                           project.settings.key->scale
                                     : juce::String ("Key: none"));
        // The time signature playing now: the current pattern's in pattern mode (it may have its own).
        const auto sig = view.session.getMode() == Session::Mode::pattern
                             ? project.getTimeSignature (view.session.getCurrentPattern())
                             : project.settings.timeSignature;
        signatureButton.setButtonText (juce::String (sig.numerator) + "/" + juce::String (sig.denominator));
        const bool aiOn = view.aiService != nullptr && view.aiService->isEnabled();
        assistantButton.setAlpha (aiOn ? 1.0f : 0.45f);
        assistantButton.setHelpText (aiOn ? "Assistant: ask about your song, get proposals to apply (Ctrl+Shift+A)"
                                          : "Assistant: AI is off. Turn it on in Options > AI to use the assistant");

        patternBox.clear (juce::dontSendNotification);
        for (auto& pattern : project.patterns)
            patternBox.addItem (pattern.name.isNotEmpty() ? pattern.name : "Pattern", (int)pattern.id.value);
        patternBox.setSelectedId ((int)view.session.getCurrentPattern().value, juce::dontSendNotification);

        const bool playing = view.session.isPlaying();
        playButton.setButtonText (playing ? juce::CharPointer_UTF8 ("\xe2\x96\xa0")
                                          : juce::CharPointer_UTF8 ("\xe2\x96\xb6")); // ■ ▶
        modeButton.setButtonText (view.session.getMode() == Session::Mode::pattern ? "PAT" : "SONG");
        const auto armed = view.session.isRecordArmed();
        recordButton.setColour (juce::TextButton::textColourOffId,
                                armed ? juce::Colour (0xffff5050) : themeOf (*this).get ("textDim"));
        recordButton.setColour (juce::TextButton::buttonColourId,
                                armed ? juce::Colour (0xff5a2020) : themeOf (*this).get ("panel"));
        const auto typing = view.typingKeyboard.isOn();
        typingButton.setColour (juce::TextButton::buttonColourId, typing
                                                                      ? themeOf (*this).get ("accent").withAlpha (0.45f)
                                                                      : themeOf (*this).get ("panel"));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panelHeader"));
        g.setColour (theme.get ("border"));
        g.fillRect (getLocalBounds().removeFromBottom (1));

        if (! logoArea.isEmpty())
        {
            auto area = logoArea;
            const auto mark = area.removeFromLeft (area.getHeight()).toFloat();
            if (logo != nullptr)
                logo->drawWithin (g, mark, juce::RectanglePlacement::centred, 1.0f);
            area.removeFromLeft (6);
            g.setColour (theme.get ("accent"));
            g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
            g.drawText ("Spinus", area, juce::Justification::centredLeft);
        }

        g.setColour (theme.get ("background"));
        g.fillRoundedRectangle (positionArea.toFloat(), 3.0f);
        g.setColour (theme.get ("accent"));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::bold));
        g.drawText (position, positionArea, juce::Justification::centred);

        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("CPU " + juce::String (juce::roundToInt (cpu * 100.0f)) + "%", cpuArea,
                    juce::Justification::centred);
    }

    /** The song's time signature, and the current pattern's own (or the song's). */
    void showSignatureMenu()
    {
        static constexpr std::pair<int, int> common[] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 },
                                                          { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } };
        const auto& project = view.session.getProject();
        const auto song = project.settings.timeSignature;
        const auto patternId = view.session.getCurrentPattern();
        const auto* pattern = project.findPattern (patternId);
        auto run = [this] (const juce::String& name, const juce::var& params)
        {
            juce::String error;
            if (auto command = commands::CommandRegistry::get().create (name, params, error))
                view.session.getHistory().perform (std::move (command));
        };
        auto params = [] (int n, int d)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("numerator", n);
            o->setProperty ("denominator", d);
            return o;
        };

        juce::PopupMenu menu;
        menu.addSectionHeader ("The song");
        for (auto [n, d] : common)
            menu.addItem (juce::String (n) + "/" + juce::String (d), true, song == model::TimeSignature { n, d },
                          [run, params, n, d] { run ("project.setTimeSignature", juce::var (params (n, d))); });
        if (pattern != nullptr)
        {
            menu.addSectionHeader ("This pattern (" + pattern->name + ")");
            menu.addItem ("The song's (" + juce::String (song.numerator) + "/" + juce::String (song.denominator) + ")",
                          true, ! pattern->timeSignature,
                          [run, patternId]
                          {
                              auto* o = new juce::DynamicObject();
                              o->setProperty ("pattern", (int)patternId.value);
                              run ("pattern.setTimeSignature", juce::var (o));
                          });
            for (auto [n, d] : common)
                menu.addItem (juce::String (n) + "/" + juce::String (d), true,
                              pattern->timeSignature == model::TimeSignature { n, d },
                              [run, params, patternId, n, d]
                              {
                                  auto* o = params (n, d);
                                  o->setProperty ("pattern", (int)patternId.value);
                                  run ("pattern.setTimeSignature", juce::var (o));
                              });
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&signatureButton));
    }

    void showKeyMenu()
    {
        static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto current = view.session.getProject().settings.key;
        auto setKey = [this] (const juce::var& params)
        {
            juce::String error;
            if (auto command = commands::CommandRegistry::get().create ("project.setKey", params, error))
                view.session.getHistory().perform (std::move (command));
        };

        juce::PopupMenu notes, scales;
        for (int tonic = 0; tonic < 12; ++tonic)
            notes.addItem (noteNames[tonic], true, current && current->tonic == tonic,
                           [setKey, tonic, current]
                           {
                               auto* o = new juce::DynamicObject();
                               o->setProperty ("tonic", tonic);
                               o->setProperty ("scale", current ? current->scale : juce::String ("major"));
                               setKey (juce::var (o));
                           });
        for (auto& scale : model::getScaleNames())
            scales.addItem (scale, true, current && current->scale == scale,
                            [setKey, scale, current]
                            {
                                auto* o = new juce::DynamicObject();
                                o->setProperty ("tonic", current ? current->tonic : 0);
                                o->setProperty ("scale", scale);
                                setKey (juce::var (o));
                            });

        juce::PopupMenu menu;
        menu.addSubMenu ("Note", notes);
        menu.addSubMenu ("Scale", scales);
        menu.addItem ("No key", current.has_value(), ! current,
                      [setKey] { setKey (juce::var (new juce::DynamicObject())); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyButton));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 6);
        // Narrow windows: no logo, shorter boxes and panel buttons, so every button still fits.
        const bool narrow = getWidth() < 1440;
        logoArea = area.removeFromLeft (narrow ? 0 : 116);

        auto place = [&] (juce::Component& c, int width)
        {
            c.setBounds (area.removeFromLeft (width));
            area.removeFromLeft (4);
        };

        place (playButton, 40);
        place (recordButton, 32);
        place (modeButton, 54);
        place (patternBox, narrow ? 110 : 140);
        place (newPatternButton, 26);
        place (metronomeButton, 32);
        place (typingButton, 32);
        place (tempo, 96);
        place (signatureButton, 44);
        place (keyButton, narrow ? 104 : 124);
        positionArea = area.removeFromLeft (92);
        area.removeFromLeft (4);
        cpuArea = area.removeFromLeft (narrow ? 48 : 60);
        place (settingsButton, 30);

        // Panel buttons as wide as their names, so no name is squeezed narrower than the others.
        for (auto* b : { &assistantButton, &browserButton, &mixerButton, &pianoButton, &audioButton, &rackButton,
                         &playlistButton })
        {
            const auto font = getLookAndFeel().getTextButtonFont (*b, area.getHeight());
            const auto text = juce::GlyphArrangement::getStringWidthInt (font, b->getButtonText());
            b->setBounds (area.removeFromRight (text + (narrow ? 14 : 22)));
            area.removeFromRight (4);
        }
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void timerCallback() override
    {
        const auto newPosition = view.session.getPositionText();
        const auto newCpu = view.session.getCpuUsage();
        if (newPosition != position || std::abs (newCpu - cpu) > 0.005f)
        {
            position = newPosition;
            cpu = newCpu;
            repaint (positionArea.getUnion (cpuArea));
        }
    }

    MainView& view;
    juce::TextButton playButton, recordButton, modeButton, metronomeButton, typingButton, newPatternButton,
        settingsButton, keyButton;
    juce::TextButton playlistButton, rackButton, audioButton, pianoButton, mixerButton, browserButton, assistantButton;
    /** The pattern list: the patterns, each in its colour, then what can be done with the current one. */
    struct PatternList final : public juce::ComboBox
    {
        explicit PatternList (MainView& v) : view (v) {}

        void showPopup() override
        {
            juce::PopupMenu menu;
            const auto& project = view.history.getProject();
            for (const auto& pattern : project.patterns)
                menu.addColouredItem ((int)pattern.id.value, pattern.name.isNotEmpty() ? pattern.name : "Pattern",
                                      juce::Colour (pattern.colour), true, getSelectedId() == (int)pattern.id.value);
            if (! project.patterns.empty())
                menu.addSeparator();
            view.addPatternActions (menu);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetComponent (this)
                                    .withMinimumWidth (getWidth())
                                    .withItemThatMustBeVisible (getSelectedId()),
                                [safe = juce::Component::SafePointer<PatternList> (this)] (int chosen)
                                {
                                    if (safe == nullptr)
                                        return;
                                    safe->hidePopup(); // the list can open again
                                    if (chosen > 0)
                                        safe->setSelectedId (chosen); // its onChange makes it current
                                });
        }

        MainView& view;
    };
    PatternList patternBox { view };
    TempoControl tempo;
    std::unique_ptr<juce::Drawable> logo = createLogo();
    juce::Rectangle<int> logoArea, positionArea, cpuArea;
    juce::TextButton signatureButton;
    juce::String position = "1:01:01";
    float cpu = 0.0f;
};

class MainView::Dock final : public juce::Component
{
public:
    Dock (juce::String t, const juce::String& hint, juce::String body) : title (std::move (t)), text (std::move (body))
    {
        setHelpText (hint);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panel"));
        auto area = getLocalBounds();
        g.setColour (theme.get ("panelHeader"));
        g.fillRect (area.removeFromTop (24));
        g.setColour (theme.get ("text"));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (title, area.withY (0).withHeight (24).reduced (8, 0), juce::Justification::centredLeft);
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (text, area.reduced (10), juce::Justification::topLeft, 8);
        g.setColour (theme.get ("border"));
        g.drawRect (getLocalBounds());
    }

private:
    juce::String title, text;
};

//==============================================================================
MainView::MainView (Session& s, ProjectDocument* d) : session (s), document (d), history (s.getHistory())
{
    setLookAndFeel (&lookAndFeel);
    PluginWindow::install (session, this);

    // Questions: an AlertWindow, whose result codes are 1, 2, ..., 0 for the last button.
    messageBox = [this] (const juce::MessageBoxOptions& options, std::function<void (int)> done)
    {
        const auto n = options.getNumButtons();
        juce::AlertWindow::showAsync (options.withAssociatedComponent (this),
                                      [n, done = std::move (done)] (int result)
                                      {
                                          if (done)
                                              done ((result + n - 1) % juce::jmax (1, n));
                                      });
    };

    chooseFile =
        [this] (const juce::String& title, const juce::File& start, bool save, std::function<void (juce::File)> done)
    {
        using FileBrowser = juce::FileBrowserComponent;
        // Projects are folders opened through the .spn file in them (or the project.json of the older format); a
        // save names the new folder. The desktop's dialog chooses files or folders, not both.
        activeChooser = std::make_unique<core::FileDialog> (
            title, start, save ? juce::String() : juce::String ("*") + project::projectFileExtension + ";project.json",
            this);
        const auto chooserFlags = save ? (int)((unsigned)FileBrowser::saveMode | (unsigned)FileBrowser::canSelectFiles)
                                       : (int)((unsigned)FileBrowser::openMode | (unsigned)FileBrowser::canSelectFiles);
        activeChooser->launchAsync (chooserFlags,
                                    [done = std::move (done)] (const core::FileDialog& fc)
                                    {
                                        if (fc.getResult() != juce::File())
                                            done (fc.getResult());
                                    });
    };

    menuBar.setModel (this);
    addAndMakeVisible (menuBar);

    topBar = std::make_unique<TopBar> (*this);
    browser = std::make_unique<Browser> (session);
    browser->onUse = [this] (const plugins::DeviceList::Entry& e) { useDevice (e); };
    browser->onUseSample = [this] (const juce::File& file)
    {
        // As if dropped on the Channel Rack: a Sampler channel, or an audio track for long audio.
        if (auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack")))
        {
            workspace.showPanel ("channelRack");
            rack->filesDropped ({ file.getFullPathName() }, 0, 0);
        }
    };
    browser->onAddSampleAsTrack = [this] (const juce::File& file) { addAudioTracks ({ file.getFullPathName() }); };
    browser->onSliceSample = [this] (const juce::File& file) { openInBeatSlicer (file); };
    browser->onDownload = [this] (const juce::String& pack) { showPluginDownloads (pack); };
    // Projects in the Browser open as File › Open recent does: after asking to save changes.
    browser->onOpenProject = [this] (const juce::File& folder)
    {
        askToSaveChanges (
            [safe = juce::Component::SafePointer<MainView> (this), folder]
            {
                if (safe != nullptr)
                    safe->openProjectFile (folder);
            });
    };
    browser->onOpenDemo = [this] (const juce::File& song)
    {
        askToSaveChanges (
            [safe = juce::Component::SafePointer<MainView> (this), song]
            {
                if (safe != nullptr)
                    safe->openDemo (song);
            });
    };
    if (document != nullptr)
        browser->setDemos (getDemoSongs());
    assistant = std::make_unique<Dock> (
        "Assistant", "Assistant: ask about your project",
        "The assistant needs Spinus's AI service, which is not running. It can only propose changes; you decide.");

    addAndMakeVisible (*topBar);
    addAndMakeVisible (*browser);
    addChildComponent (*assistant);
    addAndMakeVisible (workspace);
    addAndMakeVisible (hintBar);
    addChildComponent (assistantResizer);

    auto rack = std::make_unique<ChannelRack> (session);
    rack->onOpenPianoRoll = [this] { workspace.showPanel ("pianoRoll"); };
    rack->onLongAudio = [this] (const juce::StringArray& files) { addAudioTracks (files, true); };
    workspace.addPanel (std::move (rack));
    {
        auto playlist = std::make_unique<Playlist> (session);
        playlist->onOpenAudioClip = [this] (model::ClipId clip) { openInSpinusTune (clip); };
        playlist->onOpenAutomationClip = [this] (model::ClipId clip) { openAutomationClip (clip); };
        playlist->onProblem = [this] (const juce::String& text) { hintBar.showMessage (text); };
        workspace.addPanel (std::move (playlist));
    }
    workspace.addPanel (std::make_unique<PianoRoll> (session));
    workspace.addPanel (std::make_unique<Mixer> (session));
    workspace.addPanel (std::make_unique<HistoryPanel> (session));
    {
        auto routing = std::make_unique<RoutingView> (session);
        routing->onInsertClicked = [this] (model::InsertId insert)
        {
            if (auto* mixer = dynamic_cast<Mixer*> (workspace.getPanel ("mixer")))
            {
                workspace.showPanel ("mixer");
                mixer->selectInsert (insert);
            }
        };
        workspace.addPanel (std::move (routing));
    }
    auto analyzer = std::make_unique<Analyzer> (session, nullptr);
    analyzer->onSetUpEngine = [this]
    {
        showAiSettings();
        if (aiService != nullptr)
            AiSettingsDialog::showAnalysisEngineIfOpen();
    };
    workspace.addPanel (std::move (analyzer));
    workspace.setPanelAvailable ("analyzer", false); // experimental: until Options > AI says otherwise
    workspace.addPanel (std::make_unique<Automixer> (session, nullptr));
    workspace.addPanel (std::make_unique<Launcher> (session));
    workspace.addPanel (std::make_unique<AutomationEditor> (session));
    {
        auto audioTracks = std::make_unique<AudioTracks> (session);
        audioTracks->onAddFiles = [this] (const juce::StringArray& files) { addAudioTracks (files); };
        audioTracks->onOpenAudioClip = [this] (model::ClipId clip) { openInSpinusTune (clip); };
        audioTracks->onMoveToRack = [this] (model::ChannelId track, const juce::File& file)
        { moveAudioTrackToRack (track, file); };
        audioTracks->onSlice = [this] (const juce::File& file) { openInBeatSlicer (file); };
        workspace.addPanel (std::move (audioTracks));
    }
    // A recorded take goes on a new track where it was played; with armed tracks, each take on its track,
    // all in one undo step.
    session.getAudioRecorder().onFinished = [this] (const std::vector<AudioRecorder::Take>& takes)
    {
        auto* playlist = dynamic_cast<Playlist*> (workspace.getPanel ("playlist"));
        auto* tracks = dynamic_cast<AudioTracks*> (workspace.getPanel ("audioTracks"));
        auto& log = session.getHistory();
        const auto stepsBefore = log.getUndoDescriptions().size();
        juce::String message;
        juce::StringArray failed, placedOn;
        bool placedAny = false;
        for (const auto& take : takes)
        {
            const auto into = take.track ? take.track : (tracks != nullptr ? tracks->getRecordTarget() : std::nullopt);
            const auto before = log.getUndoDescriptions();
            const bool placed = playlist != nullptr && playlist->addRecordedTake (take.file, take.startSeconds, into);
            placedAny = placedAny || placed;
            const auto* target = into ? session.getProject().findChannel (*into) : nullptr;
            if (! placed)
            {
                failed.add (take.file.getFullPathName());
                continue;
            }
            placedOn.add (target != nullptr ? target->name : take.file.getFileNameWithoutExtension());
            const auto steps = log.getUndoDescriptions();
            const bool asTake = steps.size() > before.size() && steps[steps.size() - 1].startsWith ("Record take");
            message = asTake ? steps[steps.size() - 1] + " of " + (target != nullptr ? target->name : juce::String()) +
                                   ": it plays where it was recorded; Alt+click a take on the Playlist to use it "
                                   "instead (Ctrl+Z takes it away)"
                      : target != nullptr ? take.file.getFileNameWithoutExtension() + " is on " + target->name +
                                                ", where it was played (Ctrl+Z takes it away)"
                                          : take.file.getFileNameWithoutExtension() +
                                                " is on a new audio track, where it was played (Ctrl+Z takes it away)";
        }
        const auto added = log.getUndoDescriptions().size() - stepsBefore;
        if (placedOn.size() > 1)
        {
            log.joinLastSteps (added, "Record " + juce::String (placedOn.size()) + " tracks");
            message = "Recorded " + juce::String (placedOn.size()) + " tracks (" + placedOn.joinIntoString (", ") +
                      "), each where it was played (Ctrl+Z takes them away)";
        }
        if (! failed.isEmpty())
            message = "Could not place the recording " + failed.joinIntoString (", ") +
                      (message.isNotEmpty() ? ". " + message : juce::String());
        if (tracks != nullptr)
            tracks->setRecordStatus (message);
        hintBar.showMessage (message);
        if (placedAny)
            workspace.showPanel ("playlist");
    };
    session.getAudioRecorder().onStopped = [this] (const juce::String& why)
    {
        if (auto* tracks = dynamic_cast<AudioTracks*> (workspace.getPanel ("audioTracks")))
            tracks->setRecordStatus (why);
    };
    workspace.showPanel ("playlist");
    workspace.showPanel ("channelRack");

    commandManager.registerAllCommandsForTarget (this);
    commandManager.setFirstCommandTarget (this);
    addKeyListener (commandManager.getKeyMappings());
    shortcuts::setCommandManager (commandManager);
    setWantsKeyboardFocus (true);

    hintBar.showMessage ("Welcome to Spinus. F5 Playlist, F6 Channel Rack, F7 Piano Roll, F9 Mixer, F8 Browser.");

    historyListener = history.addListener ([this] (const commands::ChangeSet&) { historyChanged(); });
    if (document != nullptr)
    {
        document->addChangeListener (this);
        listOpenProject();
    }

    pluginWatch = std::make_unique<juce::TimedCallback> (
        [this]
        {
            for (auto id : session.getAdapter().takeFailedDevices())
            {
                // Its window shows controls that no longer do anything.
                if (auto* plugin = session.getAdapter().getDevicePlugin (id))
                    plugin->windowState->closeWindowExplicitly();
                showPluginFailure (id);
            }
        });
    pluginWatch->startTimer (250);

    setSize (1280, 800);
}

MainView::~MainView()
{
    shortcuts::forget (commandManager);
    layoutKeeper = nullptr;
    if (keepLastLayout)
        layouts->setLast (getCurrentLayout());
    menuBar.setModel (nullptr);
    if (shortcutsSaver != nullptr)
        commandManager.getKeyMappings()->removeChangeListener (shortcutsSaver.get());
    session.getAudioRecorder().onFinished = nullptr;
    session.getAudioRecorder().onStopped = nullptr;
    if (aiService != nullptr)
        aiService->removeStateListener (aiStateListener);
    if (auto* roll = dynamic_cast<PianoRoll*> (workspace.getPanel ("pianoRoll")))
    {
        roll->setRecordIdea (nullptr);
        roll->setGenerate (nullptr);
    }
    spinusTune.reset();
    session.getSuggestions().discard(); // its Regenerate may call the generator
    recordIdea.reset();
    patternGenerator.reset();
    if (document != nullptr)
        document->removeChangeListener (this);

    PluginWindow::uninstall (session);
    for (auto* plugin : tracktion::getAllPlugins (session.getEdit(), false))
        plugin->hideWindowForShutdown();

    history.removeListener (historyListener);
    setLookAndFeel (nullptr);
}

void MainView::setTheme (Theme theme)
{
    lookAndFeel.setTheme (std::move (theme));
    sendLookAndFeelChange();
    repaint();
}

void MainView::historyChanged()
{
    topBar->refresh();
    commandManager.commandStatusChanged();
}

void MainView::addPattern()
{
    juce::String error;
    // One bar, like FL's new patterns; the Channel Rack shows one step per sixteenth.
    const auto& sig = history.getProject().settings.timeSignature;
    const auto bar = model::ticksPerQuarterNote * 4 * sig.numerator / juce::jmax (1, sig.denominator);
    auto command = commands::CommandRegistry::get().create (
        "pattern.add", juce::JSON::parse ("{\"length\": " + juce::String (bar) + "}"), error);
    auto* raw = command.get();
    if (command != nullptr && history.perform (std::move (command)).wasOk())
    {
        session.setCurrentPattern (model::PatternId { (uint32_t)(int)raw->getResult()["pattern"] });
        hintBar.showMessage ("Added " + history.getProject().findPattern (session.getCurrentPattern())->name);
    }
}

void MainView::setShortcutsFile (const juce::File& file)
{
    shortcutsFile = file;
    auto* mappings = commandManager.getKeyMappings();
    if (auto xml = juce::XmlDocument::parse (file))
        mappings->restoreFromXml (*xml);
    shortcutsSaver = std::make_unique<ShortcutsSaver> (*this);
    mappings->addChangeListener (shortcutsSaver.get());
}

void MainView::setLayoutsFile (const juce::File& file)
{
    layouts = std::make_unique<WindowLayouts> (file);
    keepLastLayout = true;
    keptLayout = layouts->getLast();
    layoutKeeper = std::make_unique<juce::TimedCallback> (
        [this]
        {
            // Written only when the windows changed (a few seconds after).
            if (auto now = getCurrentLayout(); ! now.isEquivalentTo (keptLayout))
            {
                layouts->setLast (now);
                keptLayout = now;
            }
        });
    layoutKeeper->startTimer (3000);
    // Once the workspace has its size.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainView> (this)]
        {
            if (safe != nullptr)
                if (auto last = safe->layouts->getLast(); last.isValid())
                    safe->applyLayout (last);
        });
}

juce::ValueTree MainView::getCurrentLayout() const
{
    auto layout = workspace.getLayout();
    layout.setProperty ("browser", browserVisible, nullptr);
    layout.setProperty ("assistant", assistantVisible, nullptr);
    return layout;
}

void MainView::applyLayout (const juce::ValueTree& layout)
{
    browserVisible = layout.getProperty ("browser", browserVisible);
    assistantVisible = layout.getProperty ("assistant", assistantVisible);
    resized(); // the workspace gets its size first: the panels are placed in it
    workspace.setLayout (layout);
    commandManager.commandStatusChanged();
}

int MainView::layoutIndexOf (juce::CommandID id)
{
    if (id >= layout1 && id <= layout3)
        return id - layout1;
    if (id >= layout4 && id <= layout9)
        return 3 + id - layout4;
    return -1;
}

juce::CommandID MainView::layoutCommand (int index) { return index < 3 ? layout1 + index : layout4 + index - 3; }

bool MainView::useLayout (const juce::String& name)
{
    const auto layout = layouts->get (name);
    if (! layout.isValid())
        return false;
    applyLayout (layout);
    hintBar.showMessage ("Layout: " + name);
    return true;
}

void MainView::addLayoutsMenu (juce::PopupMenu& menu)
{
    juce::PopupMenu sub;
    const auto names = layouts->getNames();
    for (int i = 0; i < names.size(); ++i)
    {
        if (i < 9)
            sub.addCommandItem (&commandManager, layoutCommand (i));
        else
            sub.addItem (names[i], [this, name = names[i]] { useLayout (name); });
    }
    sub.addSeparator();
    sub.addItem ("Save layout...",
                 [this]
                 {
                     askForName ("Save the windows as a layout", "My layout",
                                 [safe = juce::Component::SafePointer<MainView> (this)] (const juce::String& name)
                                 {
                                     if (safe == nullptr)
                                         return;
                                     safe->layouts->save (name, safe->getCurrentLayout());
                                     safe->hintBar.showMessage ("Saved the layout " + name);
                                     safe->commandManager.commandStatusChanged();
                                 });
                 });
    juce::PopupMenu remove;
    for (auto& name : layouts->getSavedNames())
        remove.addItem (layouts->isBuiltIn (name) ? "Reset " + name + " to Spinus's" : "Delete " + name,
                        [this, name]
                        {
                            layouts->remove (name);
                            commandManager.commandStatusChanged();
                        });
    sub.addSubMenu ("Delete layout", remove, remove.getNumItems() > 0);
    menu.addSubMenu ("Layouts", sub);
}

void MainView::ShortcutsSaver::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Only the keys that differ from Spinus's: new defaults in later versions still arrive.
    if (auto xml = view.commandManager.getKeyMappings()->createXml (true)) [[maybe_unused]]
        const auto saved = xml->writeTo (view.shortcutsFile);
}

void MainView::addPatternActions (juce::PopupMenu& menu)
{
    const auto id = session.getCurrentPattern();
    const auto* pattern = history.getProject().findPattern (id);
    menu.addItem ("New pattern", [this] { addPattern(); });
    if (pattern != nullptr)
    {
        const auto name = pattern->name;
        auto set = [this, id] (const juce::String& key, const juce::var& value)
        {
            auto* params = new juce::DynamicObject();
            params->setProperty ("pattern", (int)id.value);
            params->setProperty (key, value);
            juce::String error;
            if (auto command = commands::CommandRegistry::get().create ("pattern.set", juce::var (params), error))
                history.perform (std::move (command));
        };
        menu.addSeparator();
        menu.addItem (
            "Clone " + name,
            [this, id]
            {
                juce::String error;
                auto command = commands::CommandRegistry::get().create (
                    "pattern.clone", juce::JSON::parse ("{\"pattern\": " + juce::String (id.value) + "}"), error);
                auto* raw = command.get();
                if (command != nullptr && history.perform (std::move (command)).wasOk())
                    session.setCurrentPattern (model::PatternId { (uint32_t)(int)raw->getResult()["pattern"] });
            });
        menu.addItem ("Rename " + name + "...", [name, set]
                      { askForName ("Rename pattern", name, [set] (const juce::String& n) { set ("name", n); }); });
        ColourMenu::addTo (menu, pattern->colour, [set] (juce::uint32 c) { set ("colour", ColourMenu::toText (c)); });
        menu.addSeparator();
        int clips = 0;
        for (auto& clip : history.getProject().playlist.clips)
            if (const auto* p = std::get_if<model::PatternClip> (&clip.content); p != nullptr && p->pattern == id)
                ++clips;
        menu.addItem (
            "Delete " + name + (clips > 0 ? " (and its " + juce::String (clips) + " Playlist clips)" : juce::String()),
            [this, id]
            {
                juce::String error;
                auto command = commands::CommandRegistry::get().create (
                    "pattern.remove", juce::JSON::parse ("{\"pattern\": " + juce::String (id.value) + "}"), error);
                if (command != nullptr && history.perform (std::move (command)).wasOk())
                {
                    // Another pattern becomes the current one, if there is one.
                    const auto& patterns = history.getProject().patterns;
                    session.setCurrentPattern (patterns.empty() ? model::PatternId {} : patterns.front().id);
                    hintBar.showMessage ("Deleted the pattern. Ctrl+Z brings it back.");
                }
            });
    }
}

//==============================================================================
juce::String MainView::getWindowTitle() const
{
    if (document == nullptr)
        return productName;
    return document->getName() + (document->hasUnsavedEdits() ? "*" : "") + " - " + productName;
}

void MainView::updateTitle()
{
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName (getWindowTitle());
}

void MainView::parentHierarchyChanged() { updateTitle(); }

void MainView::listOpenProject()
{
    // Every project opened or saved (File › Open, Open recent, the command line, Save As, the Browser) is listed
    // in the Browser's Projects (session/ProjectList).
    if (document == nullptr || document->isUntitled() || document->getFolder() == listedFolder)
        return;
    listedFolder = document->getFolder();
    browser->getProjects().addOpened (listedFolder);
}

void MainView::setDemoFolders (juce::File demoFolder, juce::File copyParent)
{
    demoBundled = std::move (demoFolder);
    demoCopyParent = std::move (copyParent);
    browser->setDemos (document != nullptr ? getDemoSongs() : juce::Array<juce::File>());
}

juce::Array<juce::File> MainView::getDemoSongs() const
{
    return demoBundled != juce::File() ? demo::listDemos (demoBundled) : demo::findBundledDemos();
}

void MainView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    listOpenProject();
    updateTitle();
    commandManager.commandStatusChanged();
    menuItemsChanged();
}

void MainView::makeProblemReport()
{
    auto logs = core::Logs::getFolder();
    if (logs == juce::File())
        logs = getLogFolder();
    juce::File zip;
    if (auto made = session::problemReport::make (
            reportFolder != juce::File() ? reportFolder : session::problemReport::getFolder(), logs,
            juce::StringArray::fromLines (AboutSpinus::getVersionInfo()), zip);
        made.failed())
    {
        showProblem ("Could not make the report file", made.getErrorMessage());
        return;
    }
    core::Logs::info ("report", "Made " + zip.getFileName());

    const auto url = session::problemReport::getIssueUrl (juce::String (productName) + " " + versionString,
                                                          session::getSystemName() + ", " + session::getPackageKind());
    messageBox (juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::InfoIcon)
                    .withTitle ("Report a problem")
                    .withMessage ("Spinus made a report file with its logs of its last runs, the version and your "
                                  "system (no keys, passwords, email addresses or user names):\n\n" +
                                  zip.getFullPathName() +
                                  "\n\nNext, GitHub's page for a new bug report opens (you need a GitHub account), "
                                  "and the file is shown: describe what happened and drag the file into the page. "
                                  "Spinus sends nothing itself.")
                    .withButton ("Open the page")
                    .withButton ("Cancel"),
                [safe = juce::Component::SafePointer<MainView> (this), url, zip] (int button)
                {
                    if (safe == nullptr || button != 0)
                        return;
                    safe->openUrl (url);
                    safe->showFile (zip);
                });
}

void MainView::showProblem (const juce::String& title, const juce::String& message)
{
    core::Logs::warning ("shown", title + ": " + message);
    messageBox (juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon, title, message), {});
}

juce::File MainView::getDefaultProjectsFolder() const
{
    if (document != nullptr && ! document->isUntitled())
        return document->getFolder().getParentDirectory();
    if (document != nullptr && ! document->getRecentProjects().isEmpty())
        return document->getRecentProjects().getFirst().getParentDirectory();
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

bool MainView::openTheDemoSong()
{
    const auto demos = getDemoSongs();
    return openDemo (
        demos.isEmpty()
            ? (demoBundled != juce::File() ? demoBundled : demo::findDemoFolder()).getChildFile (demo::demoSongName)
            : demos.getFirst());
}

bool MainView::openDemo (const juce::File& bundled)
{
    if (document == nullptr)
        return false;

    const auto copy =
        demo::copyDemo (bundled, demoCopyParent != juce::File() ? demoCopyParent : demo::getDemoCopyParent());
    if (copy.result.failed())
    {
        showProblem ("Could not open the demo song", copy.result.getErrorMessage());
        return false;
    }
    if (! openProjectFile (copy.projectFile))
        return false;

    const auto folder = copy.projectFile.getParentDirectory().getFullPathName();
    if (! offeredDownloads)
        hintBar.showMessage (copy.alreadyThere
                                 ? "Opened your copy of the demo song " + bundled.getFileName() + ", in " + folder +
                                       ". For a fresh copy, rename or delete that folder."
                                 : "Opened the demo song " + bundled.getFileName() + ": your own copy, in " + folder +
                                       ". Press Space to play it.");
    return true;
}

bool MainView::openProjectFile (const juce::File& file)
{
    if (document == nullptr)
        return false;

    // Its last session crashed with unsaved work: offer the autosave first.
    const auto folder = project::projectFolderFor (file);
    if (folder != document->getFolder())
        if (auto recovery = ProjectDocument::findRecovery (folder))
        {
            askToRecover (*recovery,
                          [safe = juce::Component::SafePointer<MainView> (this), folder]
                          {
                              if (safe != nullptr)
                                  safe->openProjectFile (folder);
                          });
            return true;
        }

    if (auto r = document->open (file); r.failed())
    {
        showProblem ("Could not open the project", r.getErrorMessage());
        return false;
    }

    hintBar.showMessage ("Opened " + document->getName());
    offeredDownloads = offerMissingDownloads();
    return true;
}

//==============================================================================
void MainView::setPluginDownloads (const plugins::PluginDownloads* d, std::vector<juce::File> searchFolders)
{
    downloads = d;
    downloadSearchFolders = std::move (searchFolders);
    commandManager.commandStatusChanged();
}

void MainView::showPluginDownloads (const juce::String& pack)
{
    if (downloads == nullptr)
        return;

    PluginDownloadDialog::show (
        *downloads, downloadSearchFolders, this, pack,
        [safe = juce::Component::SafePointer<MainView> (this)]
        {
            if (safe == nullptr)
                return;
            // Installed or removed: the Browser, the Analyzer and the device menus follow the device list.
            safe->session.getDeviceList().refresh();
            safe->session.retryMissingDevices();
        },
        [safe = juce::Component::SafePointer<MainView> (this)] (const plugins::DownloadPack& toRemove) -> juce::String
        {
            // In use by the open project: not removed (its files would vanish under a running plugin; on
            // Windows they cannot be deleted at all).
            if (safe == nullptr || safe->downloads == nullptr)
                return {};
            return PluginDownloadDialog::describeUseInProject (safe->session.getProject(), toRemove,
                                                               safe->downloads->getPackFolder (toRemove));
        });
}

bool MainView::keyPressed (const juce::KeyPress& key)
{
    // Before the shortcuts (this component's key listener): while the typing keyboard is on, R and L
    // play notes.
    if (typingKeyboard.keyPressed (key))
    {
        if (key.getTextCharacter() == '-' || key.getTextCharacter() == '=')
            hintBar.showMessage (typingKeyboard.describe());
        return true;
    }
    return false;
}

bool MainView::keyStateChanged (bool) { return typingKeyboard.keyStateChanged(); }

void MainView::setAiService (ai::AiService* service)
{
    if (auto* analyzer = dynamic_cast<Analyzer*> (workspace.getPanel ("analyzer")))
        analyzer->setService (service);
    if (auto* automixer = dynamic_cast<Automixer*> (workspace.getPanel ("automixer")))
        automixer->setService (service);
    if (aiService != nullptr)
        aiService->removeStateListener (aiStateListener);
    aiService = service;
    if (aiService != nullptr)
        aiStateListener = aiService->addStateListener ([this] { updateAiButtons(); });
    if (service != nullptr)
    {
        removeChildComponent (assistant.get());
        auto panel = std::make_unique<AssistantPanel> (session, *service);
        panel->onSetUpAi = [this] { showAiSettings(); };
        panel->getSelectedInsert = [this]
        {
            auto* mixer = dynamic_cast<Mixer*> (workspace.getPanel ("mixer"));
            return mixer != nullptr ? mixer->getSelectedInsert() : model::masterInsertId;
        };
        assistant = std::move (panel);
        addChildComponent (*assistant);
        resized();

        recordIdea = std::make_unique<RecordIdea> (session, *service);
        patternGenerator = std::make_unique<PatternGenerator> (session, *service);
    }
    updateAiButtons();
    commandManager.commandStatusChanged();
}

juce::String MainView::getTopBarHelpText (const juce::String& buttonText) const
{
    if (topBar != nullptr)
        for (auto* child : topBar->getChildren())
            if (auto* button = dynamic_cast<juce::Button*> (child);
                button != nullptr && button->getButtonText() == buttonText)
                return button->getHelpText();
    return {};
}

bool MainView::getTopBarButtonsOverlap() const
{
    if (topBar == nullptr)
        return false;
    std::vector<juce::Rectangle<int>> boxes;
    for (auto* child : topBar->getChildren())
        if (child->isVisible())
            boxes.push_back (child->getBounds());
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        if (boxes[i].getWidth() < 20 || ! topBar->getLocalBounds().contains (boxes[i]))
            return true;
        for (size_t j = i + 1; j < boxes.size(); ++j)
            if (boxes[i].intersects (boxes[j]))
                return true;
    }
    return false;
}

void MainView::updateExperimentalFeatures()
{
    const bool show = aiService != nullptr && aiService->showsExperimentalFeatures();
    if (show == experimentalShown)
        return;
    experimentalShown = show;
    workspace.setPanelAvailable ("analyzer", show); // turned off: an open Analyzer closes
    menuItemsChanged();
    commandManager.commandStatusChanged();
}

bool MainView::isCommandAvailable (juce::CommandID id) const { return id != showAnalyzer || experimentalShown; }

void MainView::updateAiButtons()
{
    updateExperimentalFeatures();
    if (topBar != nullptr)
        topBar->refresh();
    auto* roll = dynamic_cast<PianoRoll*> (workspace.getPanel ("pianoRoll"));
    if (roll == nullptr)
        return;
    const bool on = aiService != nullptr && aiService->isEnabled() && recordIdea != nullptr;
    roll->setRecordIdea (on ? std::function<void (juce::Component&)> ([this] (juce::Component& button)
                                                                      { recordIdea->showMenu (button); })
                            : nullptr);
    roll->setGenerate (on ? std::function<void (juce::Component&)> ([this] (juce::Component& button)
                                                                    { patternGenerator->showDialog (button); })
                          : nullptr);
    roll->resized();
}

void MainView::addAudioTracks (const juce::StringArray& files, bool fromRack)
{
    auto* playlist = dynamic_cast<Playlist*> (workspace.getPanel ("playlist"));
    if (playlist == nullptr)
        return;
    const auto added = playlist->addAudioFiles (files, playlist->getFirstFreeRow(), 0);
    if (added == 0)
        return;
    workspace.showPanel ("playlist");
    workspace.showPanel ("audioTracks");
    const auto what =
        added == 1 ? juce::File (files[0]).getFileNameWithoutExtension() + " is" : juce::String (added) + " files are";
    hintBar.showMessage (fromRack ? what + " longer than a sample: added as an audio track on the Playlist (see Audio "
                                           "tracks). To make a Sampler channel instead, hold Shift while dropping"
                                  : what + " on the Playlist from the song's start, each on its own audio track");
}

bool MainView::openInBeatSlicer (const juce::File& file)
{
    auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
    if (rack == nullptr)
        return false;
    juce::MouseCursor::showWaitCursor();
    const bool added = rack->addBeatSlicerChannel (file);
    juce::MouseCursor::hideWaitCursor();
    workspace.showPanel ("channelRack");
    hintBar.showMessage (added ? file.getFileNameWithoutExtension() +
                                     " is in a Beat Slicer: each slice plays from its own key (right-click a slice to "
                                     "choose its key), ready for the Piano Roll"
                               : "Could not open " + file.getFileName() + " in a Beat Slicer");
    return added;
}

bool MainView::moveAudioTrackToRack (model::ChannelId track, const juce::File& file)
{
    auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
    auto* tracks = dynamic_cast<AudioTracks*> (workspace.getPanel ("audioTracks"));
    if (rack == nullptr || tracks == nullptr)
        return false;
    juce::MouseCursor::showWaitCursor();
    const bool added = rack->addSamplerChannel (file);
    juce::MouseCursor::hideWaitCursor();
    if (! added)
    {
        hintBar.showMessage ("Could not load " + file.getFileName() + " into a Sampler");
        return false;
    }
    tracks->removeTrack (track);
    workspace.showPanel ("channelRack");
    hintBar.showMessage (file.getFileNameWithoutExtension() +
                         " is now a Sampler channel in the Channel Rack, and its audio track is gone from the "
                         "Playlist (Ctrl+Z twice brings the track back)");
    return true;
}

void MainView::openAutomationClip (model::ClipId clip)
{
    workspace.showPanel ("playlist");
    if (auto* editor = dynamic_cast<AutomationEditor*> (workspace.getPanel ("automation")))
    {
        workspace.showPanel ("automation");
        editor->setClip (clip);
    }
}

void MainView::openInSpinusTune (model::ClipId clip)
{
    auto window = std::make_unique<SpinusTuneWindow> (session, clip, aiService);
    window->setLookAndFeel (&getLookAndFeel());
    window->onClose = [this]
    {
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer (this)]
            {
                if (safe != nullptr)
                    safe->spinusTune.reset();
            });
    };
    window->setVisible (true);
    spinusTune = std::move (window);
}

void MainView::showAiSettings()
{
    if (aiService != nullptr)
        AiSettingsDialog::show (*aiService, this);
}

void MainView::setWindowsPlugins (plugins::WindowsPlugins* w, juce::File scannerExecutable)
{
    windows = w;
    pluginScanner = std::move (scannerExecutable);
    commandManager.commandStatusChanged();
}

void MainView::showWindowsPlugins (bool retestNow)
{
    if (windows == nullptr)
        return;
    WindowsPluginsDialog::show (
        *windows, pluginScanner, this,
        [safe = juce::Component::SafePointer<MainView> (this)]
        {
            // New, changed or removed bridged plugins: scan them into the Browser (the bridged folders are
            // scanned once Windows plugins are set up).
            if (safe == nullptr)
                return;
            if (auto* catalog = safe->session.getDeviceList().getCatalog(); catalog != nullptr && safe->windows)
            {
                using Format = plugins::WindowsPlugins::Format;
                catalog->addSearchFolder ("VST", safe->windows->getBridgedFolder (Format::vst2));
                catalog->addSearchFolder ("VST3", safe->windows->getBridgedFolder (Format::vst3));
                catalog->addSearchFolder ("CLAP", safe->windows->getBridgedFolder (Format::clap));
            }
            safe->browser->startScan();
        },
        retestNow,
        [safe = juce::Component::SafePointer<MainView> (this)] (const juce::Array<juce::File>& files)
        {
            // The channels and mixer tracks whose plugin is one of these files.
            juce::StringArray users;
            if (safe == nullptr)
                return users;
            auto uses = [&files] (const model::Device& d)
            {
                return d.kind == model::Device::Kind::plugin &&
                       std::any_of (files.begin(), files.end(),
                                    [&d] (const juce::File& f)
                                    {
                                        return toPortablePluginPath (f.getFullPathName()) ==
                                               toPortablePluginPath (d.plugin.fileOrIdentifier);
                                    });
            };
            const auto& project = safe->session.getProject();
            for (const auto& channel : project.channels)
                if (channel.instrument && uses (*channel.instrument))
                    users.addIfNotAlreadyThere (channel.name);
            for (const auto& slot : project.mixer.master.effects)
                if (uses (slot.device))
                    users.addIfNotAlreadyThere (project.mixer.master.name);
            for (const auto& insert : project.mixer.inserts)
                for (const auto& slot : insert.effects)
                    if (uses (slot.device))
                        users.addIfNotAlreadyThere (insert.name);
            return users;
        });
}

bool MainView::arePluginDownloadsMissing() const
{
    if (downloads == nullptr)
        return false;
    for (auto& pack : plugins::PluginDownloads::getStandardPacks())
        for (auto& file : pack.files)
            if (plugins::PluginDownloads::findFile (downloadSearchFolders, file) == juce::File())
                return true;
    return false;
}

bool MainView::offerMissingDownloads()
{
    // A project using free plugins that are not downloaded here: offer them, each project's named once.
    if (downloads == nullptr)
        return false;

    juce::StringArray projects, packs;
    for (auto id : session.getAdapter().getMissingDevices())
    {
        const auto* device = session.getProject().findDevice (id);
        const auto* bundled = device != nullptr ? model::findBundledDevice (device->stockType) : nullptr;
        if (bundled != nullptr &&
            plugins::PluginDownloads::findFile (downloadSearchFolders, bundled->file) == juce::File())
        {
            projects.addIfNotAlreadyThere (bundled->project);
            packs.addIfNotAlreadyThere (bundled->pack);
        }
    }
    if (projects.isEmpty())
        return false;

    const auto list = projects.size() == 1 ? projects[0]
                                           : projects.joinIntoString (", ", 0, projects.size() - 1) + " and " +
                                                 projects[projects.size() - 1];
    hintBar.showMessage ("This project uses " + list + ", not downloaded yet: get " +
                         (projects.size() == 1 ? "it" : "them") +
                         " here, or later in Options > Get free plugins. Until then those sounds are silent.");
    showPluginDownloads (packs.joinIntoString (","));
    return true;
}

bool MainView::saveProjectTo (const juce::File& folder)
{
    if (document == nullptr)
        return false;

    if (auto r = folder == juce::File() ? document->save() : document->saveAs (folder); r.failed())
    {
        showProblem ("Could not save the project", r.getErrorMessage());
        return false;
    }

    hintBar.showMessage ("Saved " + document->getFolder().getFullPathName());
    return true;
}

void MainView::saveThen (const std::function<void()>& then)
{
    if (document == nullptr)
        return;

    if (document->isUntitled())
        saveAsThen (then);
    else if (saveProjectTo ({}) && then)
        then();
}

void MainView::saveAsThen (const std::function<void()>& then)
{
    if (document == nullptr)
        return;

    const auto start = getDefaultProjectsFolder().getChildFile (document->isUntitled() ? juce::String ("Untitled")
                                                                                       : document->getName());

    chooseFile ("Save project as", start, true,
                [safe = juce::Component::SafePointer<MainView> (this), then] (juce::File chosen)
                {
                    if (safe == nullptr)
                        return;

                    if (chosen.hasFileExtension (project::projectFileExtension))
                        chosen = chosen.withFileExtension ({});
                    auto save = [safe, chosen, then]
                    {
                        if (safe != nullptr && safe->saveProjectTo (chosen) && then)
                            then();
                    };

                    // The system dialog only warns about replacing files; a project is a folder.
                    if (chosen != safe->document->getFolder() && project::isProjectFolder (chosen))
                    {
                        safe->messageBox (juce::MessageBoxOptions::makeOptionsOkCancel (
                                              juce::MessageBoxIconType::QuestionIcon, "Replace project?",
                                              chosen.getFileName() + " already exists. Replace the project in it?",
                                              "Replace", "Cancel"),
                                          [save] (int button)
                                          {
                                              if (button == 0)
                                                  save();
                                          });
                        return;
                    }
                    save();
                });
}

void MainView::askToSaveChanges (const std::function<void()>& then)
{
    if (document == nullptr || ! document->hasUnsavedChanges())
    {
        if (then)
            then();
        return;
    }

    messageBox (juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::QuestionIcon)
                    .withTitle ("Save changes?")
                    .withMessage ("Save the changes to " + document->getName() + " before closing it?")
                    .withButton ("Save")
                    .withButton ("Don't save")
                    .withButton ("Cancel"),
                [safe = juce::Component::SafePointer<MainView> (this), then] (int button)
                {
                    if (safe == nullptr)
                        return;
                    if (button == 0)
                        safe->saveThen (then);
                    else if (button == 1 && then)
                        then();
                });
}

void MainView::quitApp()
{
    askToSaveChanges (
        [this]
        {
            if (onQuit)
                onQuit();
        });
}

void MainView::chooseProjectToOpen()
{
    askToSaveChanges (
        [this]
        {
            chooseFile ("Open project", getDefaultProjectsFolder(), false,
                        [safe = juce::Component::SafePointer<MainView> (this)] (const juce::File& chosen)
                        {
                            if (safe != nullptr)
                                safe->openProjectFile (chosen);
                        });
        });
}

void MainView::offerToRunSeparately (const juce::String& key, const juce::String& name,
                                     const std::function<void()>& then)
{
    messageBox (juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Run " + name + " in a separate process?")
                    .withMessage ("Spinus closed unexpectedly last time while the plugin " + name +
                                  " was working.\n\nIn a separate process, if it crashes again, only the plugin "
                                  "stops: Spinus and the rest of the song carry on, and you can reload it. It costs "
                                  "a little more CPU and one audio block of extra delay.\n\nYou can change this "
                                  "later in the plugin's menu (in the Channel Rack or the Mixer).")
                    .withButton ("Run separately")
                    .withButton ("Not now"),
                [safe = juce::Component::SafePointer<MainView> (this), key, name, then] (int button)
                {
                    if (safe != nullptr && button == 0 && safe->session.getSandboxList() != nullptr)
                        safe->session.getSandboxList()->set (key, true, name); // "Run separately"
                    if (then)
                        then();
                });
}

void MainView::showPluginFailure (model::DeviceId id)
{
    messageBox (juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("A plugin stopped working")
                    .withMessage (session.getDeviceTitle (id) +
                                  " stopped working in its own process, so Spinus switched it off: as an effect it "
                                  "lets the sound through, as an instrument it is silent. The rest of the song plays "
                                  "on.\n\nReload starts it again with its settings.")
                    .withButton ("Reload")
                    .withButton ("Leave it off"),
                [safe = juce::Component::SafePointer<MainView> (this), id] (int button)
                {
                    if (safe != nullptr && button == 0) // "Reload"
                        safe->session.reloadDevice (id);
                });
}

void MainView::offerRecovery()
{
    if (document == nullptr)
        return;

    const auto recoveries = document->findRecoveries();
    if (recoveries.empty())
        return;

    askToRecover (recoveries.front(), {});
}

void MainView::askToRecover (const ProjectDocument::Recovery& recovery, const std::function<void()>& otherwise)
{
    const auto when = recovery.time.toString (true, true, false);
    messageBox (juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Recover your work?")
                    .withMessage ("Spinus closed unexpectedly while " +
                                  (recovery.untitled ? juce::String ("an unsaved project") : recovery.name) +
                                  " was open. It was autosaved at " + when + ".\n\nRecover opens the autosave; " +
                                  (recovery.untitled ? "Discard deletes it."
                                                     : "Discard keeps the last saved version (the autosave "
                                                       "stays in the project's .autosave folder)."))
                    .withButton ("Recover")
                    .withButton ("Discard"),
                [safe = juce::Component::SafePointer<MainView> (this), recovery, otherwise] (int button)
                {
                    if (safe == nullptr)
                        return;
                    if (button != 0)
                    {
                        ProjectDocument::discard (recovery);
                        if (otherwise)
                            otherwise();
                        return;
                    }
                    if (auto r = safe->document->recover (recovery); r.failed())
                        safe->showProblem ("Could not recover the project", r.getErrorMessage());
                    else
                        safe->hintBar.showMessage ("Recovered " + safe->document->getName() +
                                                   " from the autosave. Save to keep it.");
                });
}

//==============================================================================
juce::StringArray MainView::getMenuBarNames() { return { "File", "Edit", "View", "Options", "Help" }; }

juce::PopupMenu MainView::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu menu;
    auto* cm = &commandManager;

    switch (index)
    {
    case 0:
    {
        menu.addCommandItem (cm, newProject);
        menu.addCommandItem (cm, openProject);

        juce::PopupMenu recent;
        if (document != nullptr)
        {
            const auto files = document->getRecentProjects();
            for (int i = 0; i < files.size(); ++i)
                recent.addItem (recentMenuBase + i, files[i].getFileNameWithoutExtension() + "   (" +
                                                        files[i].getParentDirectory().getFullPathName() + ")");
            if (! files.isEmpty())
            {
                recent.addSeparator();
                recent.addItem (clearRecentMenuId, "Clear recent projects");
            }
        }
        menu.addSubMenu ("Open recent", recent, recent.getNumItems() > 0);

        menu.addSeparator();
        menu.addCommandItem (cm, saveProject);
        menu.addCommandItem (cm, saveProjectAs);
        menu.addSeparator();
        menu.addCommandItem (cm, exportAudio);
        menu.addSeparator();
        menu.addCommandItem (cm, quit);
        break;
    }
    case 1:
        menu.addCommandItem (cm, undo);
        menu.addCommandItem (cm, redo);
        menu.addCommandItem (cm, showHistory);
        menu.addSeparator();
        menu.addCommandItem (cm, automateLastMoved);
        break;
    case 2:
        for (auto id :
             { showPlaylist, showLauncher, showChannelRack, showAudioTracks, showPianoRoll, showMixer, showRouting,
               showAutomationEditor, showAnalyzer, showAutomixer, toggleBrowser, toggleAssistant })
            if (isCommandAvailable (id))
                menu.addCommandItem (cm, id);
        menu.addSeparator();
        addLayoutsMenu (menu);
        break;
    case 3:
        menu.addCommandItem (cm, showAudioSettings);
        menu.addCommandItem (cm, getFreePlugins);
#if JUCE_LINUX || JUCE_BSD
        menu.addCommandItem (cm, windowsPlugins); // elsewhere Windows plugins are native ones
#endif
        menu.addCommandItem (cm, aiSettings);
        menu.addCommandItem (cm, editShortcuts);
        menu.addSeparator();
        menu.addCommandItem (cm, toggleMetronome);
        menu.addCommandItem (cm, toggleRecordQuantize);
        menu.addCommandItem (cm, toggleTypingKeyboard);
        break;
    case 4:
        menu.addCommandItem (cm, showManual);
        menu.addSeparator();
        menu.addCommandItem (cm, reportProblem);
        menu.addCommandItem (cm, openLogFolder);
        menu.addCommandItem (cm, showAbout);
        break;
    default:
        break;
    }

    return menu;
}

void MainView::menuItemSelected (int itemId, int)
{
    if (document == nullptr)
        return;

    if (itemId == clearRecentMenuId)
    {
        document->clearRecentProjects();
        return;
    }

    const auto files = document->getRecentProjects();
    if (const auto i = itemId - recentMenuBase; juce::isPositiveAndBelow (i, files.size()))
        askToSaveChanges (
            [safe = juce::Component::SafePointer<MainView> (this), file = files[i]]
            {
                if (safe != nullptr)
                    safe->openProjectFile (file);
            });
}

//==============================================================================
void MainView::useDevice (const plugins::DeviceList::Entry& entry)
{
    if (entry.instrument)
    {
        auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
        if (rack != nullptr && rack->addChannelWith (entry.device, entry.name))
        {
            workspace.showPanel ("channelRack");
            // A plugin that did not load is said so at once, not left as a channel that stays silent.
            const auto* channel = session.getProject().findChannel (session.getSelectedChannel());
            const auto missing = session.getAdapter().getMissingDevices();
            if (channel != nullptr && channel->instrument &&
                std::find (missing.begin(), missing.end(), channel->instrument->id) != missing.end())
                hintBar.showMessage (entry.name + " could not be loaded: its channel shows it as Missing. " +
                                     (entry.badge == "WIN"
                                          ? juce::String ("Check Options > Windows plugins.")
                                          : juce::String ("See Problem plugins in the Browser, or scan again.")));
            else
                hintBar.showMessage ("Added a channel with " + entry.name);
        }
        else
        {
            hintBar.showMessage ("Could not add " + entry.name);
        }
        return;
    }

    auto* mixer = dynamic_cast<Mixer*> (workspace.getPanel ("mixer"));
    if (mixer == nullptr)
        return;

    const auto insert = mixer->getSelectedInsert();
    workspace.showPanel ("mixer");
    if (mixer->addEffect (insert, entry.device))
        hintBar.showMessage ("Added " + entry.name + " to the selected mixer insert");
    else
        hintBar.showMessage ("Could not add " + entry.name + ": " + mixer->getLastError());
}

void MainView::paint (juce::Graphics& g) { g.fillAll (lookAndFeel.getTheme().get ("background")); }

void MainView::resized()
{
    auto area = getLocalBounds();
    menuBar.setBounds (area.removeFromTop (menuBarHeight));
    topBar->setBounds (area.removeFromTop (topBarHeight));
    hintBar.setBounds (area.removeFromBottom (hintBarHeight));

    browser->setVisible (browserVisible);
    if (browserVisible)
        browser->setBounds (area.removeFromLeft (browserWidth));

    assistant->setVisible (assistantVisible);
    assistantResizer.setVisible (assistantVisible);
    if (assistantVisible)
    {
        assistantWidth = juce::jlimit (220, juce::jmax (220, getWidth() / 2), assistantWidth);
        assistant->setBounds (area.removeFromRight (assistantWidth));
        assistantResizer.setBounds (area.removeFromRight (5));
    }

    workspace.setBounds (area);
}

//==============================================================================
bool MainView::chooseTool (int tool)
{
    if (! juce::isPositiveAndBelow (tool, 4))
        return false;
    auto* roll = dynamic_cast<PianoRoll*> (workspace.getPanel ("pianoRoll"));
    auto* playlist = dynamic_cast<Playlist*> (workspace.getPanel ("playlist"));
    const auto front = workspace.getFrontPanelId();
    const bool rollShown = roll != nullptr && workspace.isPanelVisible ("pianoRoll");
    const bool playlistShown = playlist != nullptr && workspace.isPanelVisible ("playlist");
    static const char* const names[] = { "Draw", "Paint", "Slice", "Select" };
    if (rollShown && (front == "pianoRoll" || ! playlistShown))
    {
        roll->setTool ((PianoRoll::Tool)tool);
        hintBar.showMessage (juce::String ("Piano Roll: ") + names[tool] + " tool");
        return true;
    }
    if (playlistShown)
    {
        playlist->setTool ((Playlist::Tool)tool);
        hintBar.showMessage (juce::String ("Playlist: ") + names[tool] + " tool");
        return true;
    }
    return false;
}

void MainView::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    ids.addArray ({ showPlaylist,
                    showChannelRack,
                    showPianoRoll,
                    showMixer,
                    showHistory,
                    showAnalyzer,
                    showAutomixer,
                    showLauncher,
                    showAutomationEditor,
                    showAudioTracks,
                    toggleBrowser,
                    toggleAssistant,
                    undo,
                    redo,
                    playStop,
                    togglePlayMode,
                    toggleMetronome,
                    toggleTypingKeyboard,
                    automateLastMoved,
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
                    editShortcuts,
                    layout1,
                    layout2,
                    layout3,
                    showRouting,
                    toolDraw,
                    toolPaint,
                    toolSlice,
                    toolSelect,
                    showManual,
                    showAbout,
                    openLogFolder,
                    layout4,
                    layout5,
                    layout6,
                    layout7,
                    layout8,
                    layout9,
                    reportProblem });
}

void MainView::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using KP = juce::KeyPress;
    const auto ctrl = juce::ModifierKeys::ctrlModifier, shift = juce::ModifierKeys::shiftModifier;

    switch (id)
    {
    case showPlaylist:
        info.setInfo ("Playlist", "Show or hide the playlist", "View", 0);
        info.addDefaultKeypress (KP::F5Key, 0);
        info.setTicked (workspace.isPanelVisible ("playlist"));
        break;
    case showChannelRack:
        info.setInfo ("Channel Rack", "Show or hide the channel rack", "View", 0);
        info.addDefaultKeypress (KP::F6Key, 0);
        info.setTicked (workspace.isPanelVisible ("channelRack"));
        break;
    case showPianoRoll:
        info.setInfo ("Piano Roll", "Show or hide the piano roll", "View", 0);
        info.addDefaultKeypress (KP::F7Key, 0);
        info.setTicked (workspace.isPanelVisible ("pianoRoll"));
        break;
    case showMixer:
        info.setInfo ("Mixer", "Show or hide the mixer", "View", 0);
        info.addDefaultKeypress (KP::F9Key, 0);
        info.setTicked (workspace.isPanelVisible ("mixer"));
        break;
    case showAnalyzer:
        info.setInfo ("Analyzer (experimental)", "Drop a song in to find its tempo, key and stems", "View", 0);
        info.setTicked (workspace.isPanelVisible ("analyzer"));
        info.setActive (experimentalShown);
        break;
    case showAutomixer:
        info.setInfo ("Automixer", "Suggest mix changes with reasons, apply them and compare", "View", 0);
        info.setTicked (workspace.isPanelVisible ("automixer"));
        break;
    case showAudioTracks:
        info.setInfo ("Audio tracks", "Show or hide the audio tracks: vocals, stems and other long audio", "View", 0);
        info.addDefaultKeypress (KP::F6Key, juce::ModifierKeys::shiftModifier);
        info.setTicked (workspace.isPanelVisible ("audioTracks"));
        break;
    case showAutomationEditor:
        info.setInfo ("Automation editor", "Edit an automation clip in detail (double-click one on the Playlist)",
                      "View", 0);
        info.setTicked (workspace.isPanelVisible ("automation"));
        break;
    case showLauncher:
        info.setInfo ("Launcher", "Launch patterns and audio clips live, in scenes (Ableton-style)", "View", 0);
        info.setTicked (workspace.isPanelVisible ("launcher"));
        break;
    case showHistory:
        info.setInfo ("History", "Show or hide the undo history", "View", 0);
        info.addDefaultKeypress ('h', ctrl);
        info.setTicked (workspace.isPanelVisible ("history"));
        break;
    case toggleBrowser:
        info.setInfo ("Browser", "Show or hide the browser", "View", 0);
        info.addDefaultKeypress (KP::F8Key, 0);
        info.setTicked (browserVisible);
        break;
    case toggleAssistant:
        info.setInfo ("Assistant", "Show or hide the assistant", "View", 0);
        info.addDefaultKeypress ('a', ctrl | shift);
        info.setTicked (assistantVisible);
        break;
    case undo:
        info.setInfo (
            "Undo", history.canUndo() ? "Undo: " + history.getUndoDescriptions().strings.getLast() : "Nothing to undo",
            "Edit", 0);
        info.addDefaultKeypress ('z', ctrl);
        info.setActive (history.canUndo());
        break;
    case redo:
        info.setInfo ("Redo", history.canRedo() ? "Redo: " + history.getRedoDescriptions()[0] : "Nothing to redo",
                      "Edit", 0);
        info.addDefaultKeypress ('z', ctrl | shift);
        info.setActive (history.canRedo());
        break;
    case playStop:
        info.setInfo ("Play / Stop", session.isPlaying() ? "Stop (Space)" : "Play (Space)", "Transport", 0);
        info.addDefaultKeypress (KP::spaceKey, 0);
        info.setTicked (session.isPlaying());
        break;
    case togglePlayMode:
        info.setInfo ("Pattern / Song",
                      session.getMode() == Session::Mode::pattern
                          ? "Pattern mode: plays the current pattern (L switches to song)"
                          : "Song mode: plays the playlist (L switches to pattern)",
                      "Transport", 0);
        info.addDefaultKeypress ('l', 0);
        info.setTicked (session.getMode() == Session::Mode::song);
        break;
    case automateLastMoved:
    {
        const auto what = session.describeLastMovedControl();
        info.setInfo (what.isNotEmpty() ? "Automate the last moved control: " + what
                                        : juce::String ("Automate the last moved control (move one in a plugin first)"),
                      "Create an automation clip for the plugin control you moved last", "Edit", 0);
        info.setActive (what.isNotEmpty());
        break;
    }
    case toggleTypingKeyboard:
        info.setInfo ("Typing keyboard to piano",
                      "Play the selected channel with the computer keyboard: Z-M and Q-U, like a piano (Ctrl+T)",
                      "Options", 0);
        info.addDefaultKeypress ('t', ctrl);
        info.setTicked (typingKeyboard.isOn());
        break;
    case toggleMetronome:
        info.setInfo ("Metronome", "Metronome on or off (Ctrl+M)", "Transport", 0);
        info.addDefaultKeypress ('m', ctrl);
        info.setTicked (session.isMetronomeOn());
        break;
    case editShortcuts:
        info.setInfo ("Keyboard shortcuts...", "Choose your own keys for Spinus's commands", "Options", 0);
        break;
    case showRouting:
        info.setInfo ("Routing", "See where every channel and insert plays into, its sends and sidechains", "View", 0);
        info.setTicked (workspace.isPanelVisible ("routing"));
        break;
    case toolDraw:
    case toolPaint:
    case toolSlice:
    case toolSelect:
    {
        static const char* const names[] = { "Draw tool", "Paint tool", "Slice tool", "Select tool" };
        info.setInfo (names[id - toolDraw], "Choose this mouse tool in the Piano Roll (if in front) or the Playlist",
                      "Tools", 0);
        info.addDefaultKeypress ("pbce"[id - toolDraw], 0);
        break;
    }
    case layout1:
    case layout2:
    case layout3:
    case layout4:
    case layout5:
    case layout6:
    case layout7:
    case layout8:
    case layout9:
    {
        // The user's own layouts take the keys after Spinus's three, in the order they were saved.
        const auto index = layoutIndexOf (id);
        const auto name = layouts->getNames()[index];
        if (name.isEmpty())
            info.setInfo ("Layout " + juce::String (index + 1),
                          "No saved layout here yet (View › Layouts › Save layout)", "View",
                          juce::ApplicationCommandInfo::isDisabled);
        else
            info.setInfo (name, "Open the windows of the " + name + " layout", "View", 0);
        info.addDefaultKeypress ('1' + index, ctrl | juce::ModifierKeys::altModifier);
        break;
    }
    case showAudioSettings:
        info.setInfo ("Audio settings...", "Sound card, sample rate and buffer size (F10)", "Options", 0);
        info.addDefaultKeypress (KP::F10Key, 0);
        break;
    case showManual:
        info.setInfo ("Manual", "Read Spinus's manual, and search it (works without AI)", "Help", 0);
        info.addDefaultKeypress (KP::F1Key, 0);
        break;
    case showAbout:
        info.setInfo ("About Spinus", "Spinus's version, license and the projects it is built with", "Help", 0);
        break;
    case reportProblem:
        info.setInfo ("Report a problem...",
                      "Make a report file with Spinus's logs and open GitHub's page for a new bug report, with the "
                      "version and system filled in",
                      "Help", 0);
        break;
    case openLogFolder:
        info.setInfo ("Open log folder",
                      "The folder with Spinus's logs of its last runs, to attach to a bug report (they hold no keys "
                      "or personal paths)",
                      "Help", 0);
        break;
    case getFreePlugins:
        info.setInfo (
            "Get free plugins...",
            "Download the free open-source plugins Spinus works with: Surge XT, LSP Plugins, Dragonfly Reverb",
            "Options", 0);
        info.setActive (downloads != nullptr);
        break;
    case aiSettings:
        info.setInfo ("AI...", "Turn AI features on or off; connections to model providers or a local model", "Options",
                      0);
        info.setActive (aiService != nullptr);
        break;
    case windowsPlugins:
        info.setInfo ("Windows plugins...", "Set up and add Windows plugins (VST2, VST3, CLAP) through Wine", "Options",
                      0);
        info.setActive (windows != nullptr);
        break;
    case exportAudio:
        info.setInfo ("Export audio...",
                      "Export the song or pattern as WAV, FLAC, MP3, Opus or AAC, with stems (Ctrl+R)", "File", 0);
        info.addDefaultKeypress ('r', ctrl);
        break;
    case newProject:
        info.setInfo ("New", "Start a new, empty project (Ctrl+N)", "File", 0);
        info.addDefaultKeypress ('n', ctrl);
        info.setActive (document != nullptr);
        break;
    case openProject:
        info.setInfo ("Open...", "Open a project: its folder, or the .spn file in it (Ctrl+O)", "File", 0);
        info.addDefaultKeypress ('o', ctrl);
        info.setActive (document != nullptr);
        break;
    case saveProject:
        info.setInfo ("Save", "Save the project (Ctrl+S)", "File", 0);
        info.addDefaultKeypress ('s', ctrl);
        info.setActive (document != nullptr);
        break;
    case saveProjectAs:
        info.setInfo ("Save as...", "Save the project in a new folder (Ctrl+Shift+S)", "File", 0);
        info.addDefaultKeypress ('s', ctrl | shift);
        info.setActive (document != nullptr);
        break;
    case toggleRecord:
        info.setInfo ("Record",
                      session.isRecordArmed() ? "Recording is armed: playing records your MIDI keyboard (R)"
                                              : "Arm recording: playing then records your MIDI keyboard into "
                                                "the current pattern (R)",
                      "Transport", 0);
        info.addDefaultKeypress ('r', 0);
        info.setTicked (session.isRecordArmed());
        break;
    case toggleRecordQuantize:
        info.setInfo ("Quantize recorded notes", "Snap the start of recorded notes to sixteenths", "Options", 0);
        info.setTicked (session.getRecorder().isQuantizing());
        break;
    case quit:
        info.setInfo ("Quit", "Close Spinus (Ctrl+Q)", "File", 0);
        info.addDefaultKeypress ('q', ctrl);
        break;
    default:
        break;
    }
}

bool MainView::perform (const InvocationInfo& info)
{
    // Buttons show which panels are open; refresh them after any view change.
    const juce::ScopeGuard refreshButtons { [this] { commandManager.commandStatusChanged(); } };

    switch (info.commandID)
    {
    case showPlaylist:
        workspace.togglePanel ("playlist");
        return true;
    case showChannelRack:
        workspace.togglePanel ("channelRack");
        return true;
    case showPianoRoll:
        workspace.togglePanel ("pianoRoll");
        return true;
    case showMixer:
        workspace.togglePanel ("mixer");
        return true;
    case showHistory:
        workspace.togglePanel ("history");
        return true;
    case showAnalyzer:
        if (! experimentalShown)
            return false;
        workspace.togglePanel ("analyzer");
        return true;
    case showAutomixer:
        workspace.togglePanel ("automixer");
        return true;
    case showAutomationEditor:
        workspace.togglePanel ("automation");
        return true;
    case showAudioTracks:
        workspace.togglePanel ("audioTracks");
        return true;
    case showLauncher:
        workspace.togglePanel ("launcher");
        return true;

    case toggleBrowser:
        browserVisible = ! browserVisible;
        resized();
        return true;

    case toggleAssistant:
        assistantVisible = ! assistantVisible;
        resized();
        return true;

    case undo:
    {
        const auto what = history.canUndo() ? history.getUndoDescriptions().strings.getLast() : juce::String();
        if (history.undo())
            hintBar.showMessage ("Undone: " + what);
        return true;
    }

    case redo:
    {
        const auto what = history.canRedo() ? history.getRedoDescriptions()[0] : juce::String();
        if (history.redo())
            hintBar.showMessage ("Redone: " + what);
        return true;
    }

    case playStop:
    {
        const bool wasRecording = session.isRecording();
        session.togglePlay();
        if (wasRecording)
            hintBar.showMessage (session.getLastRecordedCount() > 0
                                     ? "Recorded " + juce::String (session.getLastRecordedCount()) +
                                           " notes (Ctrl+Z undoes them)"
                                     : juce::String ("Nothing was recorded"));
        return true;
    }

    case toggleRecord:
    {
        const bool wasRecording = session.isRecording();
        session.toggleRecord();
        if (session.isRecordArmed())
            hintBar.showMessage (session.getRecorder().getInputNames().isEmpty() && ! typingKeyboard.isOn()
                                     ? juce::String ("Recording armed, but no MIDI keyboard is connected (Ctrl+T "
                                                     "plays notes with the computer keyboard)")
                                     : "Recording armed: press Space and play (into " +
                                           (session.getProject().findChannel (session.getSelectedChannel()) != nullptr
                                                ? session.getProject().findChannel (session.getSelectedChannel())->name
                                                : juce::String ("no channel")) +
                                           ")");
        else if (wasRecording)
            hintBar.showMessage ("Recorded " + juce::String (session.getLastRecordedCount()) + " notes");
        return true;
    }

    case toggleRecordQuantize:
        session.getRecorder().setQuantizing (! session.getRecorder().isQuantizing());
        return true;

    case togglePlayMode:

        session.toggleMode();

        return true;

    case toggleMetronome:

        session.setMetronome (! session.isMetronomeOn());

        return true;

    case automateLastMoved:
        if (const auto target = session.getLastMovedControl())
            if (const auto clip = session.createAutomationClip (*target); clip.isValid())
            {
                openAutomationClip (clip);
                hintBar.showMessage ("Automation clip for " + session.describeLastMovedControl() +
                                     " added as a flat line: shape it in the Automation editor (click to add points, "
                                     "drag them), and play in song mode");
            }
        return true;

    case toggleTypingKeyboard:

        typingKeyboard.setOn (! typingKeyboard.isOn());
        hintBar.showMessage (typingKeyboard.isOn() ? typingKeyboard.describe()
                                                   : juce::String ("Typing keyboard off: keys are shortcuts again"));
        commandManager.commandStatusChanged();
        if (topBar != nullptr)
            topBar->refresh();
        return true;

    case showRouting:
        workspace.togglePanel ("routing");
        return true;

    case toolDraw:
    case toolPaint:
    case toolSlice:
    case toolSelect:
        return chooseTool (info.commandID - toolDraw);

    case layout1:
    case layout2:
    case layout3:
    case layout4:
    case layout5:
    case layout6:
    case layout7:
    case layout8:
    case layout9:
        return useLayout (layouts->getNames()[layoutIndexOf (info.commandID)]);

    case editShortcuts:
    {
        // JUCE's editor of the command manager's keys: click a key to change or remove it, "+" to add one.
        auto editor = std::make_unique<ShortcutsEditor> (
            *commandManager.getKeyMappings(), [safe = juce::Component::SafePointer (this)] (juce::CommandID c)
            { return safe != nullptr && safe->isCommandAvailable (c); });
        editor->setSize (560, 620);
        editor->setLookAndFeel (&getLookAndFeel());
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (editor.release());
        options.dialogTitle = "Keyboard shortcuts";
        options.componentToCentreAround = this;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.dialogBackgroundColour = findColour (juce::ResizableWindow::backgroundColourId);
        options.launchAsync();
        return true;
    }
    case showAudioSettings:

        AudioSettings::show (
            session.getEdit().engine.getDeviceManager().deviceManager, this,
            EngineHost::getPipeWireInput (session.getEdit().engine),
            [this] { return recording::getDefaultInput (session.getEdit().engine); },
            [this] (int input) { recording::setDefaultInput (session.getEdit().engine, input); });

        return true;

    case exportAudio:
        ExportDialog::show (session, this);
        return true;

    case getFreePlugins:
        showPluginDownloads();
        return true;

    case showManual:
        ManualWindow::show (this, {}, manualFolder);
        return true;

    case showAbout:
        AboutSpinus::show (this);
        return true;

    case reportProblem:
        makeProblemReport();
        return true;

    case openLogFolder:
    {
        auto folder = core::Logs::getFolder();
        if (folder == juce::File())
            folder = getLogFolder();
        folder.createDirectory();
        if (! folder.startAsProcess())
            showProblem ("Could not open the log folder", "The logs are in " + folder.getFullPathName());
        return true;
    }

    case windowsPlugins:
        showWindowsPlugins();
        return true;

    case aiSettings:
        showAiSettings();
        return true;

    case newProject:
        askToSaveChanges (
            [this]
            {
                document->newProject();
                hintBar.showMessage ("New project");
            });
        return true;
    case openProject:
        chooseProjectToOpen();
        return true;
    case saveProject:
        saveThen ({});
        return true;
    case saveProjectAs:
        saveAsThen ({});
        return true;
    case quit:
        quitApp();
        return true;

    default:
        return false;
    }
}

} // namespace spinus::ui
