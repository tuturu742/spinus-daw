#include "ExportDialog.h"

#include "Theme.h"
#include "WindowStacking.h"
#include "session/ProgramFolder.h"

namespace spinus::ui
{

using Format = engine::ExportSettings::Format;

namespace
{
// Format menu IDs; MP3 bit rates are their own entries.
constexpr int wav16Id = 1, wav24Id = 2, wav32Id = 3, mp3_320Id = 4, mp3_192Id = 5, flacId = 6, opusId = 7, aacId = 8;
constexpr int songId = 1, patternId = 2;
} // namespace

ExportDialog::ExportDialog (Session& s) : session (s)
{
    mp3Available = engine::ExportJob::isMp3Available();
    aacAvailable = engine::ExportJob::isAacAvailable();

    whatLabel.setText ("Export", juce::dontSendNotification);
    formatLabel.setText ("Format", juce::dontSendNotification);
    rateLabel.setText ("Sample rate", juce::dontSendNotification);

    what.addItem ("The song (playlist)", songId);
    what.addItem ("The current pattern", patternId);
    updatePatternChoice();
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    // Export what Play plays: the playlist in song mode, the pattern in pattern mode (if the playlist is empty).
    const bool songIsEmpty = engine::ExportJob::getSongEnd (session.getProject()) <= 0;
    what.setSelectedId (pattern != nullptr && (session.getMode() == Session::Mode::pattern && songIsEmpty) ? patternId
                                                                                                           : songId,
                        juce::dontSendNotification);

    format.addItem ("WAV, 16-bit (CD)", wav16Id);
    format.addItem ("WAV, 24-bit", wav24Id);
    format.addItem ("WAV, 32-bit float", wav32Id);
    format.addItem ("FLAC (lossless, smaller than WAV)", flacId);
    format.addItem ("MP3, 320 kbit/s", mp3_320Id);
    format.addItem ("MP3, 192 kbit/s", mp3_192Id);
    format.addItem ("Opus, 160 kbit/s", opusId);
    format.addItem ("AAC (M4A), 256 kbit/s", aacId);
    format.setItemEnabled (mp3_320Id, mp3Available);
    format.setItemEnabled (mp3_192Id, mp3Available);
    format.setItemEnabled (aacId, aacAvailable);
    format.setSelectedId (wav24Id, juce::dontSendNotification);

    rate.addItem ("44.1 kHz", 44100);
    rate.addItem ("48 kHz", 48000);
    rate.setSelectedId (juce::roundToInt (session.getProject().settings.sampleRate) == 44100 ? 44100 : 48000,
                        juce::dontSendNotification);

    status.setJustificationType (juce::Justification::topLeft);
    status.setText (getMissingEncoderText (mp3Available, aacAvailable, getSystem()), juce::dontSendNotification);

    exportButton.onClick = [this] { chooseFileAndExport(); };
    cancelButton.onClick = [this]
    {
        if (job != nullptr)
            job->cancel();
        if (live != nullptr)
            live->cancel();
    };
    showButton.onClick = [this] { lastFile.revealToUser(); };
    playAlong.setTooltip ("Hear the song while it is exported: it plays once from the start through your speakers, "
                          "and what you hear is saved (the song only, at the sound card's sample rate, no stems)");
    playAlong.onClick = [this] { updateEnablement(); };

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &whatLabel, &formatLabel, &rateLabel, &what, &format, &rate, &stems, &playAlong, &exportButton, &status })
        addAndMakeVisible (c);
    addChildComponent (cancelButton);
    addChildComponent (showButton);
    addChildComponent (progressBar);

    setSize (440, 290);
    session.addChangeListener (this); // the current pattern can change while the window is open
}

ExportDialog::~ExportDialog()
{
    session.removeChangeListener (this);
    // Cancels a running export.
    job = nullptr;
    live = nullptr;
}

void ExportDialog::updatePatternChoice()
{
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    what.changeItemText (patternId,
                         "The current pattern" + (pattern != nullptr ? " (" + pattern->name + ")" : juce::String()));
    what.setItemEnabled (patternId, pattern != nullptr);
    if (pattern == nullptr && what.getSelectedId() == patternId)
        what.setSelectedId (songId, juce::dontSendNotification);
}

namespace
{
juce::Component::SafePointer<juce::DocumentWindow>& openWindow()
{
    static juce::Component::SafePointer<juce::DocumentWindow> window;
    return window;
}

/** The dialog's window: not modal, so Spinus keeps working while it exports. Closing it cancels the export. */
class Window final : public juce::DocumentWindow
{
public:
    Window (ExportDialog* content, juce::Component* owner)
        : DocumentWindow ("Export audio", juce::Colour (0xff1e2227), DocumentWindow::closeButton)
    {
        if (owner != nullptr)
            if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&owner->getLookAndFeel()))
            {
                setBackgroundColour (lf->getTheme().get ("panel"));
                content->setLookAndFeel (lf);
            }
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
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

void ExportDialog::show (Session& session, juce::Component* centreAround)
{
    if (openWindow() == nullptr)
        openWindow() = new Window (new ExportDialog (session), centreAround);
    openWindow()->toFront (true);
}

ExportDialog* ExportDialog::getOpen()
{
    return openWindow() != nullptr ? dynamic_cast<ExportDialog*> (openWindow()->getContentComponent()) : nullptr;
}

void ExportDialog::closeWindow()
{
    if (auto* window = openWindow().getComponent())
        delete window;
}

ExportDialog::System ExportDialog::getSystem()
{
#if JUCE_WINDOWS
    return System::windows;
#else
    return isFlatpak() ? System::flatpak : System::linuxNative;
#endif
}

juce::String ExportDialog::getMissingEncoderText (bool mp3, bool aac, System system)
{
    juce::StringArray missing;
    switch (system)
    {
    case System::linuxNative:
        if (! mp3)
            missing.add ("MP3 needs the 'lame' program (on Arch Linux: sudo pacman -S lame).");
        if (! aac)
            missing.add ("AAC needs the 'ffmpeg' program (on Arch Linux: sudo pacman -S ffmpeg).");
        break;
    case System::flatpak:
        if (! mp3 || ! aac)
            missing.add (juce::String (! mp3 && ! aac ? "MP3 and AAC are"
                                       : ! mp3        ? "MP3 is"
                                                      : "AAC is") +
                         " not available in the Flatpak version of Spinus. FLAC and Opus work.");
        break;
    case System::windows:
        if (! mp3)
            missing.add ("MP3 needs the LAME encoder (lame.exe) installed and on the PATH.");
        if (! aac)
            missing.add ("AAC needs FFmpeg (ffmpeg.exe) installed and on the PATH.");
        break;
    }
    return missing.joinIntoString ("\n");
}

//==============================================================================
void ExportDialog::setFormat (Format f)
{
    const auto id = f == Format::wav16        ? wav16Id
                    : f == Format::wav32Float ? wav32Id
                    : f == Format::mp3        ? mp3_320Id
                    : f == Format::flac       ? flacId
                    : f == Format::opus       ? opusId
                    : f == Format::aac        ? aacId
                                              : wav24Id;
    format.setSelectedId (id, juce::dontSendNotification);
}

void ExportDialog::setStems (bool on) { stems.setToggleState (on, juce::dontSendNotification); }

void ExportDialog::setPlayWhileExporting (bool on)
{
    playAlong.setToggleState (on, juce::dontSendNotification);
    updateEnablement();
}

void ExportDialog::setExportPattern (bool on)
{
    what.setSelectedId (on ? patternId : songId, juce::dontSendNotification);
}

engine::ExportSettings ExportDialog::getSettings() const
{
    engine::ExportSettings s;
    switch (format.getSelectedId())
    {
    case wav16Id:
        s.format = Format::wav16;
        break;
    case wav32Id:
        s.format = Format::wav32Float;
        break;
    case mp3_320Id:
    case mp3_192Id:
        s.format = Format::mp3;
        s.mp3Kbps = format.getSelectedId() == mp3_192Id ? 192 : 320;
        break;
    case flacId:
        s.format = Format::flac;
        break;
    case opusId:
        s.format = Format::opus;
        break;
    case aacId:
        s.format = Format::aac;
        break;
    default:
        s.format = Format::wav24;
        break;
    }
    s.sampleRate = (double)rate.getSelectedId();
    s.stems = stems.getToggleState();
    if (what.getSelectedId() == patternId)
        s.pattern = session.getCurrentPattern();
    if (playAlong.getToggleState()) // the song as it plays, as one file
    {
        s.stems = false;
        s.pattern.reset();
    }
    return s;
}

juce::File ExportDialog::getSuggestedFile() const
{
    const auto title = session.getSongInfo().title; // the project's name when the song has no title
    const auto name = juce::File::createLegalFileName (title.isNotEmpty() ? title : "Untitled");
    const auto projectFolder = session.getProjectFolder();
    const auto folder =
        projectFolder != juce::File()
            ? projectFolder
            : (lastFile != juce::File() ? lastFile.getParentDirectory()
                                        : juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    return folder.getChildFile (name + engine::ExportSettings::extensionFor (getSettings().format));
}

juce::String ExportDialog::findProblem() const
{
    const auto settings = getSettings();
    if (auto problem = engine::ExportJob::findProblem (session.getProject(), settings); problem.isNotEmpty())
        return problem;
    if (playAlong.getToggleState() && session.getEdit().engine.getDeviceManager().getSampleRate() <= 0.0)
        return "There is no sound card to play through: export without playing instead.";
    return {};
}

void ExportDialog::chooseFileAndExport()
{
    updatePatternChoice();
    // Say what is wrong before asking for a file name.
    if (const auto problem = findProblem(); problem.isNotEmpty())
    {
        status.setText (problem, juce::dontSendNotification);
        showButton.setVisible (false);
        return;
    }

    const auto suggested = getSuggestedFile();
    chooser = std::make_unique<core::FileDialog> ("Export audio as", suggested, "*" + suggested.getFileExtension());
    using Browser = juce::FileBrowserComponent;
    constexpr auto chooserFlags = (int)((unsigned)Browser::saveMode | (unsigned)Browser::canSelectFiles |
                                        (unsigned)Browser::warnAboutOverwriting);
    chooser->launchAsync (chooserFlags,
                          [safe = juce::Component::SafePointer<ExportDialog> (this)] (const core::FileDialog& fc)
                          {
                              if (safe != nullptr && fc.getResult() != juce::File())
                                  safe->startExport (fc.getResult());
                          });
}

void ExportDialog::startExport (const juce::File& destination)
{
    showButton.setVisible (false);
    progress = 0.0;
    if (playAlong.getToggleState())
    {
        live = std::make_unique<LiveExport> (session, destination, getSettings(),
                                             [this] (const LiveExport::Result& r) { finished (r); });
        lastFile = live->getFile();
        status.setText ("Playing and exporting " + lastFile.getFileName() + "... (Cancel or Stop to stop)",
                        juce::dontSendNotification);
        startTimerHz (10);
        live->start();
        updateEnablement();
        return;
    }

    job = session.createExportJob (destination, getSettings(),
                                   [this] (const engine::ExportJob::Result& r) { finished (r); });
    lastFile = job->getPlannedFiles().front();
    status.setText ("Exporting " + lastFile.getFileName() + "...", juce::dontSendNotification);
    job->start();
    updateEnablement();
    startTimerHz (10);
}

void ExportDialog::finished (const engine::ExportJob::Result& result)
{
    stopTimer();
    progress = 1.0;

    if (result.ok)
    {
        const auto stemCount = (int)result.files.size() - 1;
        status.setText ("Saved " + lastFile.getFileName() +
                            (stemCount > 0 ? " and " + juce::String (stemCount) + " stems in \"" +
                                                 lastFile.getFileNameWithoutExtension() + " stems\""
                                           : juce::String()) +
                            ".",
                        juce::dontSendNotification);
        showButton.setVisible (true);
    }
    else
    {
        status.setText (result.error, juce::dontSendNotification);
    }

    // Delete the finished job after this callback (it is the one calling).
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<ExportDialog> (this)]
        {
            if (safe != nullptr)
            {
                safe->job = nullptr;
                safe->live = nullptr;
                safe->updateEnablement();
            }
        });
    updateEnablement();
}

void ExportDialog::timerCallback()
{
    if (job != nullptr)
        progress = job->getProgress();
    else if (live != nullptr)
        progress = live->getProgress();
}

void ExportDialog::updateEnablement()
{
    const bool busy = isExporting();
    for (juce::Component* c :
         std::initializer_list<juce::Component*> { &what, &format, &rate, &stems, &playAlong, &exportButton })
        c->setEnabled (! busy);
    // Playing along: the song, at the sound card's rate, without stems.
    if (playAlong.getToggleState())
    {
        what.setSelectedId (songId, juce::dontSendNotification);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &what, &rate, &stems })
            c->setEnabled (false);
    }
    cancelButton.setVisible (busy);
    progressBar.setVisible (busy);
}

//==============================================================================
void ExportDialog::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto row = [&] (juce::Label& label, juce::Component& control)
    {
        auto r = area.removeFromTop (26);
        label.setBounds (r.removeFromLeft (100));
        control.setBounds (r);
        area.removeFromTop (8);
    };
    row (whatLabel, what);
    row (formatLabel, format);
    row (rateLabel, rate);
    stems.setBounds (area.removeFromTop (24).withTrimmedLeft (96));
    playAlong.setBounds (area.removeFromTop (24).withTrimmedLeft (96));
    area.removeFromTop (12);

    auto buttons = area.removeFromBottom (28);
    exportButton.setBounds (buttons.removeFromRight (110));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
    showButton.setBounds (buttons.removeFromLeft (130));

    area.removeFromBottom (8);
    progressBar.setBounds (area.removeFromBottom (18));
    status.setBounds (area);
}

void ExportDialog::paint (juce::Graphics& g)
{
    if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&getLookAndFeel()))
        g.fillAll (lf->getTheme().get ("panel"));
}

} // namespace spinus::ui
