#pragma once

#include "core/FileDialog.h"
#include "session/LiveExport.h"
#include "session/Session.h"

namespace spinus::ui
{

/** File → Export audio (Ctrl+R): the song or the current pattern as WAV or MP3, optionally
    with one stem per mixer insert. Shows progress with Cancel, then where the files went. Its window is not
    modal: the user keeps working (and playing) while it exports; closing the window cancels the export.
    "Play while exporting" exports the song as it plays instead (LiveExport). */
class ExportDialog final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit ExportDialog (Session&);
    ~ExportDialog() override;

    /** Opens the dialog in its own window, or brings the open one to the front. */
    static void show (Session&, juce::Component* centreAround);
    /** The open dialog, if any. */
    static ExportDialog* getOpen();
    /** Closes the window (cancelling an export): at quit, before the session goes. */
    static void closeWindow();

    //==============================================================================
    // For the buttons and for tests.
    void setFormat (engine::ExportSettings::Format);
    void setStems (bool);
    void setExportPattern (bool); // false: the song
    void setPlayWhileExporting (bool);
    bool isPlayingWhileExporting() const { return playAlong.getToggleState(); }
    engine::ExportSettings getSettings() const;

    /** The file the save dialog suggests: named after the song's title or the project, in the project's folder
        (an untitled project: "Untitled", in the folder of the last export or the Music folder). */
    juce::File getSuggestedFile() const;

    /** Why the export cannot start (an empty playlist, no sound card to play along), or nothing. */
    juce::String findProblem() const;

    /** Asks for the file and exports; says what is wrong instead when the export cannot start. */
    void chooseFileAndExport();
    void startExport (const juce::File& destination);
    bool isExporting() const { return (job != nullptr && job->isRunning()) || (live != nullptr && live->isRunning()); }
    juce::String getStatusText() const { return status.getText(); }

    /** Where Spinus runs, for the hint about formats whose program is missing. */
    enum class System
    {
        linuxNative,
        flatpak,
        windows
    };
    static System getSystem();
    /** What the dialog says about the formats that need a program that is missing: an Arch command on Linux, a
        plain sentence in the Flatpak (which has neither program) and on Windows. Empty when nothing is missing. */
    static juce::String getMissingEncoderText (bool mp3Available, bool aacAvailable, System);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updatePatternChoice(); }
    /** "The current pattern (name)", greyed out when there is none. */
    void updatePatternChoice();
    void finished (const engine::ExportJob::Result&);
    void updateEnablement();

    Session& session;
    juce::Label whatLabel, formatLabel, rateLabel, status;
    juce::ComboBox what, format, rate;
    juce::ToggleButton stems { "Also export stems (one file per mixer insert)" };
    juce::ToggleButton playAlong { "Play while exporting (in real time)" };
    juce::TextButton exportButton { "Export..." }, cancelButton { "Cancel" }, showButton { "Show in folder" };
    double progress = 0.0;
    juce::ProgressBar progressBar { progress };

    std::unique_ptr<core::FileDialog> chooser;
    std::unique_ptr<engine::ExportJob> job;
    std::unique_ptr<LiveExport> live;
    juce::File lastFile;
    bool mp3Available = false, aacAvailable = false;
};

} // namespace spinus::ui
