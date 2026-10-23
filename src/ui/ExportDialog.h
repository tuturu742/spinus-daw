#pragma once

#include "core/FileDialog.h"
#include "session/LiveExport.h"
#include "session/Session.h"

namespace spinus::ui
{

/** File → Export audio (Ctrl+R): the song or the current pattern as WAV or MP3, optionally
    with one stem per mixer insert. Shows progress with Cancel, then where the files went. Playback
    keeps working while it exports. "Play while exporting" exports the song as it plays instead
    (LiveExport). */
class ExportDialog final : public juce::Component, private juce::Timer
{
public:
    explicit ExportDialog (Session&);
    ~ExportDialog() override;

    /** Opens the dialog in its own window. */
    static void show (Session&, juce::Component* centreAround);

    //==============================================================================
    // For the buttons and for tests.
    void setFormat (engine::ExportSettings::Format);
    void setStems (bool);
    void setExportPattern (bool); // false: the song
    void setPlayWhileExporting (bool);
    bool isPlayingWhileExporting() const { return playAlong.getToggleState(); }
    engine::ExportSettings getSettings() const;

    void chooseFileAndExport();
    void startExport (const juce::File& destination);
    bool isExporting() const { return (job != nullptr && job->isRunning()) || (live != nullptr && live->isRunning()); }
    juce::String getStatusText() const { return status.getText(); }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
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
