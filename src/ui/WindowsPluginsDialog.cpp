#include "WindowsPluginsDialog.h"

#include "Theme.h"
#include "WindowStacking.h"

namespace spinus::ui
{

using plugins::WindowsPlugins;
using Kind = WindowsPlugins::Runtime::Kind;

namespace
{
constexpr int margin = 14;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::String megabytes (juce::int64 bytes) { return juce::String (juce::roundToInt ((double)bytes / 1.0e6)) + " MB"; }

juce::String formatName (WindowsPlugins::Format f)
{
    switch (f)
    {
    case WindowsPlugins::Format::vst2:
        return "VST2";
    case WindowsPlugins::Format::vst3:
        return "VST3";
    case WindowsPlugins::Format::clap:
        break;
    }
    return "CLAP";
}
} // namespace

//==============================================================================
/** One long step on a background thread; the dialog polls it. */
class WindowsPluginsDialog::Job final : public juce::Thread
{
public:
    Job (juce::String firstStage, std::function<Outcome (Job&)> w)
        : juce::Thread ("Spinus Windows plugins"), work (std::move (w)), stage (std::move (firstStage))
    {
    }

    ~Job() override
    {
        cancel = true;
        stopThread (-1);
    }

    void run() override
    {
        auto result = work (*this);
        const juce::ScopedLock sl (lock);
        outcome = std::move (result);
        done = true;
    }

    void say (const juce::String& s)
    {
        const juce::ScopedLock sl (lock);
        stage = s;
        progress = -1.0;
    }

    void setProgress (double p) { progress = p; }

    juce::String getStage() const
    {
        const juce::ScopedLock sl (lock);
        return stage;
    }

    Outcome getOutcome() const
    {
        const juce::ScopedLock sl (lock);
        return outcome;
    }

    std::atomic<bool> cancel { false }, done { false };
    std::atomic<double> progress { -1.0 };

private:
    std::function<Outcome (Job&)> work;
    juce::CriticalSection lock;
    juce::String stage;
    Outcome outcome;
};

//==============================================================================
/** A plugin in the list: its name and format, and Remove. */
class WindowsPluginsDialog::Row final : public juce::Component
{
public:
    explicit Row (WindowsPluginsDialog& d) : dialog (d)
    {
        remove.setHelpText ("Delete this Windows plugin from Spinus's Windows folder");
        // After the click returns: removing refreshes the list, which may delete this row.
        remove.onClick = [this]
        {
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<WindowsPluginsDialog> (&dialog), r = row]
                {
                    if (safe != nullptr)
                        safe->removePlugin (r);
                });
        };
        addAndMakeVisible (remove);
        setInterceptsMouseClicks (false, true);
    }

    void update (int newRow)
    {
        row = newRow;
        remove.setEnabled (! dialog.isBusy());
        repaint();
    }

    void resized() override { remove.setBounds (getLocalBounds().removeFromRight (74).reduced (2, 1)); }

private:
    WindowsPluginsDialog& dialog;
    juce::TextButton remove { "Remove" };
    int row = -1;
};

//==============================================================================
WindowsPluginsDialog::WindowsPluginsDialog (WindowsPlugins& w, juce::File scannerExecutable)
    : windows (w), scanner (std::move (scannerExecutable))
{
    intro.setText ("Windows plugins (VST2, VST3 and CLAP) run in Spinus through Wine and yabridge. Spinus "
                   "sets them up and keeps its own Windows folder, so nothing else on your computer changes.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    status.setJustificationType (juce::Justification::topLeft);
    health.setJustificationType (juce::Justification::topLeft);
    realtime.setJustificationType (juce::Justification::topLeft);
    realtime.setFont (juce::FontOptions (12.0f));
    list.setRowHeight (26);

    setUpButton.onClick = [this] { setUp(); };
    downloadButton.onClick = [this] { downloadRuntime(); };
    addButton.onClick = [this] { chooseFile(); };
    retestButton.onClick = [this] { retest(); };
    switchButton.onClick = [this] { switchRuntime(); };
    cancelButton.onClick = [this] { cancel(); };
    folderButton.onClick = [this] { windows.getPrefix().getChildFile ("drive_c").startAsProcess(); };
    drawingToggle.onClick = [this] { setDrawingWithoutGraphicsCard (drawingToggle.getToggleState()); };
    drawingToggle.setHelpText ("Wine's Direct3D off: plugins that draw with the graphics card (e.g. Direct2D) draw "
                               "the plain way instead. Close and reopen the plugin's window after changing it.");
    retestButton.setHelpText ("Tests again that Windows plugins work with the Wine in use");
    addButton.setHelpText ("Run a Windows plugin installer (.exe, .msi), or add a plugin file (.dll, .vst3, .clap)");

    for (auto* c : std::initializer_list<juce::Component*> {
             &intro, &status, &health, &realtime, &setUpButton, &downloadButton, &addButton, &retestButton,
             &switchButton, &folderButton, &cancelButton, &list, &progressBar, &drawingToggle })
        addChildComponent (c);
    intro.setVisible (true);
    status.setVisible (true);

    setSize (640, 560);
    refresh();
    startTimerHz (10);
}

WindowsPluginsDialog::~WindowsPluginsDialog() { job = nullptr; }

bool WindowsPluginsDialog::isBusy() const { return job != nullptr; }

juce::StringArray WindowsPluginsDialog::getPluginNames() const
{
    juce::StringArray names;
    for (auto& f : found)
        names.add (f.name + " (" + formatName (f.format) + ")");
    return names;
}

//==============================================================================
void WindowsPluginsDialog::refresh()
{
    const auto runtime = windows.getRuntime();
    const bool busy = isBusy();
    if (runtime && windows.isPrefixReady() && page != Page::offerDownload)
        page = Page::ready;
    else if (! runtime && page == Page::ready)
        page = Page::notSetUp;

    setUpButton.setVisible (page == Page::notSetUp && ! busy);
    downloadButton.setVisible (page == Page::offerDownload && ! busy);
    downloadButton.setButtonText ("Download (" + megabytes (WindowsPlugins::getRuntimePack().getDownloadSize()) +
                                  ") and set up");
    cancelButton.setVisible (busy);
    progressBar.setVisible (busy);

    const bool ready = page == Page::ready;
    for (auto* c : std::initializer_list<juce::Component*> { &addButton, &retestButton, &switchButton, &folderButton,
                                                             &list, &health, &realtime, &drawingToggle })
        c->setVisible (ready);
    for (auto* b : std::initializer_list<juce::Button*> { &addButton, &retestButton, &switchButton, &drawingToggle })
        b->setEnabled (! busy);
    drawingToggle.setToggleState (windows.isDrawingWithoutGraphicsCard(), juce::dontSendNotification);

    if (! busy && page == Page::notSetUp)
        status.setText (lastMessage.isNotEmpty() ? lastMessage
                        : windows.isSandboxed()  ? "Not set up yet. In the Flatpak version, Spinus cannot use the "
                                                   "Wine installed on this computer: it uses its own tested "
                                                   "Wine and yabridge, a download Set up offers."
                                                 : "Not set up yet. Setting up takes a minute or two; Spinus first "
                                                   "tries the Wine on this computer, if there is one.",
                        juce::dontSendNotification);
    else if (! busy && page == Page::offerDownload)
        // A download that failed says why first (the reason to download stays below it).
        status.setText ((lastMessage.isNotEmpty() ? lastMessage + "\n\n" : juce::String()) + downloadReason +
                            "\n\nSpinus can download a Wine and yabridge that are tested to work "
                            "together (Wine Staging 9.21 and yabridge 5.1.1, from their projects' "
                            "release pages). They are used only by Spinus.",
                        juce::dontSendNotification);

    if (ready && runtime)
    {
        found = windows.findWindowsPlugins();
        std::erase_if (found, [] (const WindowsPlugins::Found& f)
                       { return f.file.getFullPathName().containsIgnoreCase ("/drive_c/Spinus/Test/"); });
        list.updateContent();
        list.repaint();

        const auto version = WindowsPlugins::getWineVersion (*runtime);
        juce::String text =
            "Using " + runtime->describe() +
            (version.isNotEmpty() ? " (" + version + ")." : juce::String (", but its Wine does not start."));
        if (const auto last = windows.getLastTest())
            text << "\nLast test, " << last->when.formatted ("%d %b %Y %H:%M") << ": "
                 << (last->passed ? juce::String ("Windows plugins work.") : "failed - " + last->message);
        if (runtime->kind == Kind::system)
            if (const auto problem =
                    WindowsPlugins::getKnownProblem (version, WindowsPlugins::getYabridgeVersion (*runtime));
                problem.isNotEmpty())
                text << "\n" << problem << " Click Use Spinus's tested Wine.";
        health.setText (text, juce::dontSendNotification);
        resized(); // it may need more lines

        const auto other =
            runtime->kind == Kind::spinus ? windows.findUsableSystemRuntime() : windows.findSpinusRuntime();
        switchButton.setButtonText (runtime->kind == Kind::spinus ? "Use the Wine on this computer"
                                                                  : "Use Spinus's tested Wine");
        switchButton.setVisible (runtime->kind == Kind::system || other.has_value());

        const auto rt = WindowsPlugins::checkRealtime();
        realtime.setText (rt.advice.isEmpty() ? "Real-time privileges: OK." : rt.advice, juce::dontSendNotification);

        if (! busy)
            status.setText (lastMessage.isNotEmpty() ? lastMessage
                            : found.empty() ? juce::String ("Ready. Add a Windows plugin to start.")
                                            : "Ready: " + juce::String ((int)found.size()) + " Windows plugins.",
                            juce::dontSendNotification);
    }
    resized();
    repaint();
}

void WindowsPluginsDialog::start (const juce::String& firstStage, std::function<Outcome (Job&)> work)
{
    if (isBusy())
        return;
    lastMessage.clear();
    status.setText (firstStage, juce::dontSendNotification);
    job = std::make_unique<Job> (firstStage, std::move (work));
    job->startThread();
    refresh();
}

void WindowsPluginsDialog::timerCallback()
{
    if (job == nullptr)
        return;
    progress = job->progress.load();
    status.setText (job->getStage(), juce::dontSendNotification);
    if (job->done)
    {
        const auto outcome = job->getOutcome();
        job = nullptr;
        finished (outcome);
    }
}

void WindowsPluginsDialog::finished (const Outcome& outcome)
{
    if (outcome.use)
    {
        // Spinus's environment changes on the message thread, before any scan starts.
        windows.setChosenRuntime (outcome.use->kind);
        WindowsPlugins::applyEnvironment (*outcome.use);
        windows.syncBridges (*outcome.use);
        page = Page::ready;
    }
    if (outcome.offerDownload)
    {
        page = Page::offerDownload;
        downloadReason = outcome.error;
    }

    lastMessage = outcome.offerDownload ? juce::String() : outcome.error.isNotEmpty() ? outcome.error : outcome.message;
    refresh();
    if (outcome.error.isEmpty() && onPluginsChanged)
        onPluginsChanged();
}

void WindowsPluginsDialog::cancel()
{
    if (job != nullptr)
        job->cancel = true;
}

void WindowsPluginsDialog::finishForTests()
{
    if (job == nullptr)
        return;
    while (! job->done)
        juce::Thread::sleep (20);
    timerCallback();
}

//==============================================================================
void WindowsPluginsDialog::setUp()
{
    start (
        "Looking for Wine and yabridge on this computer",
        [this] (Job& j) -> Outcome
        {
            auto system = windows.findUsableSystemRuntime();
            if (! system)
            {
                if (auto own = windows.findSpinusRuntime())
                    system = std::move (own); // downloaded earlier
                else if (windows.isSandboxed())
                    return { "In the Flatpak version, Spinus cannot use the Wine installed on this computer (it is "
                             "outside the Flatpak's sandbox).",
                             {},
                             {},
                             true };
                else
                    return { "No Wine and yabridge were found on this computer.", {}, {}, true };
            }
            j.say ("Testing " + system->describe());
            const auto result = windows.test (*system, scanner, [&j] (const juce::String& s) { j.say (s); }, &j.cancel);
            if (j.cancel)
                return { "Cancelled.", {}, {}, false };
            if (! result.passed)
                return { "The Wine on this computer (" + result.wineVersion +
                             ") does not work with its yabridge: " + result.message,
                         {},
                         {},
                         true };
            if (system->kind == Kind::system)
                if (const auto problem = WindowsPlugins::getKnownProblem (result.wineVersion,
                                                                          WindowsPlugins::getYabridgeVersion (*system));
                    problem.isNotEmpty())
                {
                    // Spinus's own works around it: use it if downloaded, else offer it.
                    const auto own = windows.findSpinusRuntime();
                    if (! own)
                        return { problem, {}, {}, true };
                    j.say ("Testing Spinus's Wine and yabridge");
                    const auto ownResult =
                        windows.test (*own, scanner, [&j] (const juce::String& s) { j.say (s); }, &j.cancel);
                    if (ownResult.passed)
                        return {
                            {}, own, "Ready. Windows plugins use Spinus's tested Wine and yabridge. " + problem, false
                        };
                }
            return { {}, system, "Ready. Windows plugins use " + system->describe() + ".", false };
        });
}

void WindowsPluginsDialog::downloadRuntime()
{
    start ("Downloading Wine and yabridge",
           [this] (Job& j) -> Outcome
           {
               const auto& pack = WindowsPlugins::getRuntimePack();
               const auto downloads = windows.getDownloads();
               auto installed = downloads.install (
                   pack, [&j] (const plugins::PluginDownloads::Progress& p)
                   { j.setProgress (p.total > 0 ? (double)p.done / (double)p.total : -1.0); }, &j.cancel);
               if (installed.failed())
                   return { "Could not download Wine and yabridge: " + installed.getErrorMessage(), {}, {}, false };

               const auto own = windows.findSpinusRuntime();
               if (! own)
                   return { "The download is incomplete. Try again.", {}, {}, false };
               j.say ("Testing Spinus's Wine and yabridge");
               const auto result = windows.test (*own, scanner, [&j] (const juce::String& s) { j.say (s); }, &j.cancel);
               if (! result.passed)
                   return { "Spinus's Wine and yabridge did not work here: " + result.message, {}, {}, false };
               return { {}, own, "Ready. Windows plugins use Spinus's tested Wine and yabridge.", false };
           });
}

void WindowsPluginsDialog::retest()
{
    const auto runtime = windows.getRuntime();
    if (! runtime)
        return;
    start ("Testing " + runtime->describe(),
           [this, r = *runtime] (Job& j) -> Outcome
           {
               // The Wine in use: plugins open in the project keep running.
               const auto result =
                   windows.test (r, scanner, [&j] (const juce::String& s) { j.say (s); }, &j.cancel, false);
               if (! result.passed)
               {
                   const auto offer = r.kind == Kind::system;
                   return { "The test failed: " + result.message +
                                (offer ? juce::String ("\nUse Spinus's tested Wine instead.") : juce::String()),
                            {},
                            {},
                            false };
               }
               return { {}, r, "Windows plugins work.", false };
           });
}

void WindowsPluginsDialog::switchRuntime()
{
    const auto runtime = windows.getRuntime();
    if (! runtime)
        return;
    if (runtime->kind == Kind::system && ! windows.findSpinusRuntime())
    {
        downloadReason = "Spinus's tested Wine is not downloaded yet.";
        page = Page::offerDownload;
        refresh();
        return;
    }

    const auto other = runtime->kind == Kind::spinus ? windows.findUsableSystemRuntime() : windows.findSpinusRuntime();
    if (! other)
        return;
    start ("Testing " + other->describe(),
           [this, r = *other] (Job& j) -> Outcome
           {
               const auto result = windows.test (r, scanner, [&j] (const juce::String& s) { j.say (s); }, &j.cancel);
               if (! result.passed)
                   return { "Not switched: " + result.message, {}, {}, false };
               const auto problem =
                   r.kind == Kind::system
                       ? WindowsPlugins::getKnownProblem (result.wineVersion, WindowsPlugins::getYabridgeVersion (r))
                       : juce::String();
               return { {},
                        r,
                        "Windows plugins now use " + r.describe() +
                            ". Windows plugins already open in your project start working again when you reopen it." +
                            (problem.isNotEmpty() ? " " + problem : juce::String()),
                        false };
           });
}

void WindowsPluginsDialog::chooseFile()
{
    chooser =
        std::make_unique<core::FileDialog> ("Choose a Windows installer (a plugin, or its sounds) or a plugin file",
                                            juce::File(), "*.exe;*.msi;*.dll;*.vst3;*.clap");
    using Browser = juce::FileBrowserComponent;
    chooser->launchAsync ((int)((unsigned)Browser::openMode | (unsigned)Browser::canSelectFiles |
                                (unsigned)Browser::canSelectDirectories),
                          [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                          {
                              if (safe != nullptr && c.getResult() != juce::File())
                                  safe->addPlugin (c.getResult());
                          });
}

void WindowsPluginsDialog::addPlugin (const juce::File& file)
{
    const auto runtime = windows.getRuntime();
    if (! runtime)
        return;
    const bool installer = file.hasFileExtension ("exe;msi");
    if (const auto problem = installer ? windows.getInstallerProblem (file) : juce::String(); problem.isNotEmpty())
    {
        // Too long for the status line, and the command is easier pasted than typed.
        lastMessage = file.getFileName() + " was not started: it is a 32-bit Windows program, and this Flatpak "
                                           "lacks Flatpak's 32-bit support.";
        refresh();
        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::WarningIcon, "32-bit Windows programs need Flatpak's 32-bit support", problem,
            "Copy the command", "Close", this,
            juce::ModalCallbackFunction::create (
                [] (int choice)
                {
                    if (choice == 1)
                        juce::SystemClipboard::copyTextToClipboard (WindowsPlugins::getFlatpak32BitCommand());
                }));
        return;
    }
    start (installer ? "Installing " + file.getFileName() + ": finish the steps in the installer's window"
                     : "Adding " + file.getFileName(),
           [this, file, installer, r = *runtime] (Job& j) -> Outcome
           {
               const auto before = windows.findWindowsPlugins();
               const auto result =
                   installer ? windows.runInstaller (r, file, &j.cancel, [&j] (const juce::String& s) { j.say (s); })
                             : windows.addPluginFile (file);
               // Also after a cancelled or failed installer: what it installed until then is bridged and
               // scanned now, not only after a restart.
               if (result.failed() && ! installer)
                   return { result.getErrorMessage(), {}, {}, false };

               j.say ("Looking for the new plugins");
               juce::StringArray added;
               for (auto& f : windows.findWindowsPlugins())
                   if (std::find (before.begin(), before.end(), f) == before.end())
                       added.add (f.name + " (" + formatName (f.format) + ")");
               if (result.failed())
               {
                   if (added.isEmpty())
                       return { result.getErrorMessage(), {}, {}, false };
                   return { {},
                            r,
                            (j.cancel.load() ? "The installer was stopped. " : result.getErrorMessage() + " ") +
                                "It had added " + added.joinIntoString (", ") +
                                ": they appear in the Browser once the scan at the bottom of the Browser has "
                                "finished.",
                            false };
               }
               if (added.isEmpty())
                   return { {},
                            r,
                            installer ? "The installer finished and added no new plugin. If it was an extra one for "
                                        "a plugin you have (sounds, presets, a library), they are in place now. If "
                                        "you expected a plugin: run it again and keep the folders it suggests (only "
                                        "64-bit plugins count)."
                                      : "Nothing new to add.",
                            false };
               return { {},
                        r,
                        "Added " + added.joinIntoString (", ") +
                            ". They appear in the Browser under Instruments or Effects, Installed plugins, with a "
                            "WIN badge, once the scan at the bottom of the Browser has finished (a minute or two).",
                        false };
           });
}

void WindowsPluginsDialog::removePlugin (int row, bool confirmed)
{
    if (isBusy() || ! juce::isPositiveAndBelow (row, (int)found.size()))
        return;
    const auto plugin = found[(size_t)row];
    const auto label = plugin.name + " (" + formatName (plugin.format) + ")";

    // A plugin the open project uses keeps its files: deleting them under it would leave it half-gone.
    if (whoUses)
        if (const auto users = whoUses (windows.findBridgedFiles (plugin)); ! users.isEmpty())
        {
            lastMessage = label + " is used in this project (" + users.joinIntoString (", ") +
                          "). Remove it from the project first, then remove it here.";
            refresh();
            return;
        }

    if (! confirmed)
    {
        juce::AlertWindow::showOkCancelBox (
            juce::MessageBoxIconType::QuestionIcon, "Remove " + label + "?",
            "Spinus deletes " + plugin.name + " from its Windows folder:\n" + plugin.file.getFullPathName() +
                "\n\nProjects that use it will show it as Missing. Its sounds and presets stay (its own "
                "uninstaller removes those). To use it again, add it again.",
            "Remove", "Keep", this,
            juce::ModalCallbackFunction::create (
                [safe = juce::Component::SafePointer<WindowsPluginsDialog> (this), plugin] (int choice)
                {
                    if (safe == nullptr || choice != 1)
                        return;
                    // The list may have changed meanwhile: remove this plugin, wherever it is now.
                    for (size_t i = 0; i < safe->found.size(); ++i)
                        if (safe->found[i] == plugin)
                            safe->removePlugin ((int)i, true);
                }));
        return;
    }

    const auto result = windows.removePlugin (plugin);
    lastMessage = result.wasOk() ? "Removed " + label + "." : result.getErrorMessage();
    refresh();
    if (result.wasOk() && onPluginsChanged)
        onPluginsChanged(); // the Browser drops it
}

void WindowsPluginsDialog::setDrawingWithoutGraphicsCard (bool on)
{
    const auto runtime = windows.getRuntime();
    if (! runtime || on == windows.isDrawingWithoutGraphicsCard())
        return refresh();
    start (on ? "Turning drawing with the graphics card off" : "Turning drawing with the graphics card on",
           [this, on, r = *runtime] (Job&) -> Outcome
           {
               if (auto result = windows.setDrawingWithoutGraphicsCard (r, on); result.failed())
                   return { result.getErrorMessage(), {}, {}, false };
               return { {},
                        {},
                        on ? "Plugin windows now draw without the graphics card. Close and reopen a plugin's window "
                             "to see it; if a plugin then shows no picture, turn this off again."
                           : "Plugin windows draw with the graphics card again. Close and reopen a plugin's window.",
                        false };
           });
}

juce::Component* WindowsPluginsDialog::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* item = dynamic_cast<Row*> (existing);
    if (! juce::isPositiveAndBelow (row, (int)found.size()))
    {
        delete existing;
        return nullptr;
    }
    if (item == nullptr)
    {
        delete existing;
        item = new Row (*this);
    }
    item->update (row);
    return item;
}

//==============================================================================
void WindowsPluginsDialog::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, (int)found.size()))
        return;
    const auto& theme = themeOf (*this);
    if (selected)
        g.fillAll (theme.get ("accent").withAlpha (0.25f));
    const auto& f = found[(size_t)row];
    g.setColour (theme.get ("text"));
    g.setFont (juce::FontOptions (13.0f));
    // The row's Remove button takes the right end.
    g.drawText (f.name, 8, 0, width - 150, height, juce::Justification::centredLeft, true);
    g.setColour (theme.get ("textDim"));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (formatName (f.format), width - 140, 0, 52, height, juce::Justification::centredRight);
}

void WindowsPluginsDialog::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    for (auto* l : { &intro, &status, &health })
        l->setColour (juce::Label::textColourId, theme.get ("text"));
    realtime.setColour (juce::Label::textColourId, theme.get ("textDim"));
}

void WindowsPluginsDialog::resized()
{
    auto area = getLocalBounds().reduced (margin);
    intro.setBounds (area.removeFromTop (44));
    area.removeFromTop (6);

    const bool ready = page == Page::ready;
    auto statusArea = area.removeFromTop (ready ? 44 : 110);
    status.setBounds (statusArea);
    if (progressBar.isVisible())
    {
        auto row = area.removeFromTop (28);
        cancelButton.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        progressBar.setBounds (row);
        area.removeFromTop (8);
    }

    auto buttons = area.removeFromTop (32);
    if (! ready)
    {
        setUpButton.setBounds (buttons.withWidth (220));
        downloadButton.setBounds (buttons.withWidth (240));
        return;
    }

    addButton.setBounds (buttons.removeFromLeft (200));
    buttons.removeFromLeft (8);
    folderButton.setBounds (buttons.removeFromLeft (170));
    area.removeFromTop (8);

    // The health text takes the lines it needs (a known problem with the Wine in use adds a few).
    const auto healthWidth = juce::jmax (1, area.getWidth() - 10);
    int lines = 0;
    for (const auto& paragraph : juce::StringArray::fromLines (health.getText()))
        lines += juce::jmax (1, (int)std::ceil (juce::GlyphArrangement::getStringWidth (health.getFont(), paragraph) /
                                                (float)healthWidth));
    const auto healthHeight = juce::jlimit (44, 120, lines * (int)std::ceil (health.getFont().getHeight()) + 8);
    auto bottom = area.removeFromBottom (136 + healthHeight);
    list.setBounds (area);
    bottom.removeFromTop (10);
    health.setBounds (bottom.removeFromTop (healthHeight));
    auto row = bottom.removeFromTop (30);
    retestButton.setBounds (row.removeFromLeft (100));
    row.removeFromLeft (8);
    switchButton.setBounds (row.removeFromLeft (240));
    bottom.removeFromTop (6);
    drawingToggle.setBounds (bottom.removeFromTop (24));
    bottom.removeFromTop (4);
    realtime.setBounds (bottom);
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
    Window (WindowsPluginsDialog* dialog, juce::Component* owner)
        : DocumentWindow ("Windows plugins", juce::Colour (0xff1e2227), DocumentWindow::closeButton)
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

void WindowsPluginsDialog::show (WindowsPlugins& windows, juce::File scannerExecutable, juce::Component* centreAround,
                                 std::function<void()> onPluginsChanged, bool retestNow,
                                 std::function<juce::StringArray (const juce::Array<juce::File>&)> whoUses)
{
    if (auto* existing = openWindow().getComponent())
    {
        existing->toFront (true);
        return;
    }
    auto* dialog = new WindowsPluginsDialog (windows, std::move (scannerExecutable));
    dialog->onPluginsChanged = std::move (onPluginsChanged);
    dialog->whoUses = std::move (whoUses);
    openWindow() = new Window (dialog, centreAround);
    if (retestNow)
        dialog->retest();
}

void WindowsPluginsDialog::closeWindow()
{
    if (auto* window = openWindow().getComponent())
        delete window;
}

} // namespace spinus::ui
