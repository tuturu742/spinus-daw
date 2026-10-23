#pragma once

#include "commands/CommandHistory.h"
#include "engine/EngineAdapter.h"
#include "engine/Exporter.h"
#include "engine/SamplePreview.h"
#include "session/Freezer.h"
#include "session/AudioRecorder.h"
#include "session/IdeaRecorder.h"
#include "session/LauncherRecorder.h"
#include "session/MidiRecorder.h"
#include "session/Suggestions.h"
#include "session/Tuner.h"
#include "plugins/DeviceList.h"
#include "plugins/SandboxList.h"
#include "project/ProjectFiles.h"

namespace spinus
{

/** One open project in a running Spinus: its command history, the Tracktion Edit that plays it (kept in
    step by the Engine Adapter), and session state that is not part of the project: transport, play mode
    (pattern or song), the current pattern, the metronome.

    Message thread only. Sends a change message when session state changes (play, mode, pattern,
    metronome); project changes come through the command history's listeners.
*/
class Session : public juce::ChangeBroadcaster
{
public:
    enum class Mode
    {
        pattern,
        song
    };

    explicit Session (tracktion::Engine&, std::unique_ptr<engine::DeviceFactory> devices = nullptr);
    ~Session() override;

    commands::CommandHistory& getHistory() noexcept { return history; }
    const model::Project& getProject() const noexcept { return history.getProject(); }
    tracktion::Edit& getEdit() noexcept { return *edit; }
    engine::EngineAdapter& getAdapter() noexcept { return *adapter; }
    engine::DeviceFactory& getDeviceFactory() noexcept { return *devices; }

    /** Notes suggested for a pattern, waiting for Accept or Discard (shown in the Piano Roll). */
    Suggestions& getSuggestions() noexcept { return suggestions; }

    /** Frozen channels and inserts (model/Freeze.h): their render status. */
    Freezer& getFreezer() noexcept { return *freezer; }

    /** True if the device's plugin is unloaded because its channel or insert is frozen (its window
        cannot open until it is unfrozen). */
    bool isDeviceFrozen (model::DeviceId) const;

    /** Spinus Tune's renderings of tuned audio clips. */
    Tuner& getTuner() noexcept { return *tuner; }
    /** Plays a sample on its own, outside the song (the Browser's preview). */
    engine::SamplePreview& getSamplePreview() noexcept { return *preview; }

    /** Plays newly rendered audio (called by the Freezer and the Tuner). */
    void refreshRenderedAudio();

    /** What can be added to channels and effect slots (bundled devices, plus the app's scanned plugins). */
    plugins::DeviceList& getDeviceList() noexcept { return deviceList; }

    /** The project and every device's current settings, ready to save. */
    project::ProjectData captureProjectData() const;

    /** Replaces the open project with a loaded one: stops playback, clears the undo history and
        creates the devices with their saved settings. */
    void loadProject (project::ProjectData);

    /** The project's folder (invalid while untitled): audio clips with relative paths are found in its
        audio/ folder. */
    void setProjectFolder (const juce::File&);
    juce::File getProjectFolder() const noexcept { return projectFolder; }
    juce::File resolveAudioFile (const juce::String& path) const { return adapter->resolveAudioFile (path); }

    //==============================================================================
    bool isPlaying() const;
    void play();
    void stop(); // returns to where playback started (FL behaviour)
    void togglePlay();

    /** The clip launcher, through the Engine Adapter: launching while stopped starts playback,
        and stopping playback stops every launched clip. A channel that played a launched clip stays off
        the Playlist until backToPlaylist() (Ableton's rule). */
    bool launchClip (model::ClipId);
    void launchScene (model::SceneId);
    void stopLauncherTrack (model::LauncherTrackId);
    void stopLauncher();
    void backToPlaylist();
    /** "Record to Playlist": while armed, launched clips are recorded in song mode (arming switches to it)
        and written as Playlist clips when playback stops or recording is turned off (one undo step). */
    void setLauncherRecording (bool);
    bool isLauncherRecording() const noexcept { return launcherRecorder->isArmed(); }
    LauncherRecorder& getLauncherRecorder() noexcept { return *launcherRecorder; }
    /** Playlist clips written by the last launcher recording. */
    int getLastLauncherRecorded() const noexcept { return lastLauncherRecorded; }

    Mode getMode() const noexcept { return mode; }
    void setMode (Mode);
    void toggleMode();

    /** A loop region (FL: right-drag in the timeline): playback loops over it. One for song mode (in
        playlist ticks) and one for pattern mode (in the current pattern's ticks; cleared when another
        pattern is chosen). Without one, pattern mode loops the whole pattern and song mode plays on. A
        view setting: not saved in the project. */
    struct LoopRegion
    {
        model::Ticks start = 0, end = 0;
        bool operator== (const LoopRegion&) const = default;
    };
    void setLoopRegion (Mode, std::optional<LoopRegion>);
    std::optional<LoopRegion> getLoopRegion (Mode m) const { return m == Mode::pattern ? patternLoop : songLoop; }

    /** The pattern pattern mode plays (and the Channel Rack shows). Invalid when there are none. */
    model::PatternId getCurrentPattern() const noexcept { return currentPattern; }
    void setCurrentPattern (model::PatternId);

    /** The channel the Piano Roll edits (FL: the channel you opened it for). Follows channel edits:
        invalid only when there are no channels. */
    model::ChannelId getSelectedChannel() const noexcept { return selectedChannel; }
    void setSelectedChannel (model::ChannelId);

    /** Recording: when armed, playing records the MIDI keyboard into the current pattern
        for the selected channel (arming switches to pattern mode). Stopping writes the notes as one
        undo step. */
    bool isRecordArmed() const noexcept { return recordArmed; }
    void setRecordArmed (bool);
    void toggleRecord() { setRecordArmed (! recordArmed); }
    bool isRecording() const noexcept { return recorder != nullptr && recorder->isRecording(); }
    MidiRecorder& getRecorder() noexcept { return *recorder; }

    /** "Record idea": the audio input over the current pattern, for hum, sing or beatbox to notes. */
    IdeaRecorder& getIdeaRecorder() noexcept { return *ideaRecorder; }
    AudioRecorder& getAudioRecorder() noexcept { return *audioRecorder; }
    /** Notes written by the last recording (shown in the hint bar). */
    int getLastRecordedCount() const noexcept { return lastRecorded; }

    bool isMetronomeOn() const;
    void setMetronome (bool);

    /** Plugins that run in a separate process: the app's list (without one, nothing does). */
    void setSandboxList (plugins::SandboxList* list) { sandboxList = list; }
    plugins::SandboxList* getSandboxList() const noexcept { return sandboxList; }
    bool runsSeparately (model::DeviceId id) const { return adapter->runsSeparately (id); }
    /** Runs this device's plugin in a separate process, or back inside Spinus: remembered for the plugin,
        and every device of it in the project is made again (settings kept). */
    void setRunSeparately (model::DeviceId, bool);
    /** Makes a device's plugin again with its settings (after it stopped working in its process). */
    void reloadDevice (model::DeviceId);

    /** Tries again to create devices whose plugin was missing (e.g. just downloaded or installed). */
    void retryMissingDevices();

    /** Opens the device's own window (its plugin editor, or a generic one with sliders), or brings it
        to the front. Fails when the device's plugin is missing or has no window (e.g. no display). */
    bool showDeviceWindow (model::DeviceId);
    bool isDeviceWindowShowing (model::DeviceId) const;
    void closeDeviceWindow (model::DeviceId);

    /** Adds an automation clip for the target on a new Playlist lane (FL: "Create automation clip"): four
        bars long from the song's start, flat at the parameter's current value. Returns the clip, or an
        invalid ID if the target cannot be automated. */
    model::ClipId createAutomationClip (const model::ParameterTarget&);

    /** The plugin control the user moved last, in a plugin's own window or Spinus's (FL: "last tweaked"),
        for "Automate the last moved control". Not set by automation playing back. */
    std::optional<model::ParameterTarget> getLastMovedControl() const noexcept { return lastMoved; }
    /** "Cutoff (Surge XT - Bass)". */
    juce::String describeLastMovedControl() const;

    /** What an automation target is called, e.g. "Bass volume" or "Surge XT - Filter 1 Cutoff". */
    juce::String describeTarget (const model::ParameterTarget&) const;
    /** An automation value (0..1) as the control shows it, e.g. "-6.0 dB" or "1.20 kHz" (a percentage
        when the target cannot be found). */
    juce::String describeTargetValue (const model::ParameterTarget&, float normalized) const;
    /** A typed value as 0..1, by the rule in dsp/ParameterText.h: in the units the control shows ("-6",
        "-6 dB", "35%", "220 ms", "1.2 kHz"; a choice by its name). Nothing if it cannot be read so, e.g. a unit of
        another kind ("50%" for a control in dB). A target that cannot be found reads as describeTargetValue
        shows it then, a percentage of the whole range. */
    std::optional<float> parseTargetValue (const model::ParameterTarget&, const juce::String&) const;

    /** A device's automatable parameters (ID, name), for menus. */
    std::vector<std::pair<juce::String, juce::String>> getDeviceParameters (model::DeviceId) const;
    /** A device parameter's current value as the plugin shows it ("1.20 kHz"), or empty. */
    juce::String getDeviceParameterValueText (model::DeviceId, const juce::String& parameterId) const;

    /** A device's window title: its name and where it is, e.g. "Hall Reverb - 4 Reverb". */
    juce::String getDeviceTitle (model::DeviceId) const;

    /** An export of the current project (with the devices' current settings), ready to start(). */
    std::unique_ptr<engine::ExportJob>
    createExportJob (const juce::File& destination, engine::ExportSettings,
                     std::function<void (const engine::ExportJob::Result&)> onFinished);

    /** Playback position as FL shows it: bar:beat:step (1-based; 4 steps per beat). */
    juce::String getPositionText() const;

    /** Audio processing load, 0..1. */
    float getCpuUsage() const;

private:
    void projectChanged (const commands::ChangeSet&);
    void applyMode();
    /** Sets the transport's loop for the mode (the pattern, or a loop region). */
    void applyLoop();
    tracktion::TimeRange toTimeRange (LoopRegion) const;
    void syncAdapter();

    std::unique_ptr<engine::DeviceFactory> devices;
    plugins::DeviceList deviceList;
    commands::CommandHistory history;
    Suggestions suggestions { history };
    std::unique_ptr<tracktion::Edit> edit;
    std::unique_ptr<engine::EngineAdapter> adapter;
    std::unique_ptr<Freezer> freezer;
    std::unique_ptr<Tuner> tuner;
    std::unique_ptr<engine::SamplePreview> preview;
    std::unique_ptr<MidiRecorder> recorder;
    std::unique_ptr<IdeaRecorder> ideaRecorder;
    std::unique_ptr<AudioRecorder> audioRecorder;
    std::unique_ptr<LauncherRecorder> launcherRecorder;
    int historyListener = 0;
    bool recordArmed = false;
    enum class Loading
    {
        no,
        clearing,
        loading
    } loading = Loading::no;
    juce::File projectFolder;
    int lastRecorded = 0, lastLauncherRecorded = 0;
    plugins::SandboxList* sandboxList = nullptr;

    Mode mode = Mode::pattern;
    std::optional<LoopRegion> patternLoop, songLoop;

    /** Notes which plugin control changed (any thread); the message thread turns it into a target. */
    struct ControlWatcher final : tracktion::ParameterChangeHandler::Listener, juce::AsyncUpdater
    {
        explicit ControlWatcher (Session& s) : session (s) {}
        void pluginParameterChanged (tracktion::AutomatableParameter&, bool isFollowingAutomation) override;
        void handleAsyncUpdate() override;
        Session& session;
        std::atomic<tracktion::AutomatableParameter*> changed { nullptr };
    };
    std::unique_ptr<ControlWatcher> controlWatcher;
    std::optional<model::ParameterTarget> lastMoved;
    model::PatternId currentPattern;
    model::ChannelId selectedChannel;
    tracktion::TimePosition playStart;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};

} // namespace spinus
