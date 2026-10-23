#pragma once

#include "core/FileDialog.h"
#include "Workspace.h"
#include "session/Session.h"

namespace spinus::ui
{

/** Audio tracks: the channels that audio clips play through (vocals, stems, long
    recordings), apart from the Channel Rack's instruments, which are for patterns. One row per track:
    mute, name, volume and pan knobs, its mixer insert and a summary of its clips on the Playlist.
    Right-click a name for Rename, automation clips, Spinus Tune and Delete. "+ Add audio file" and
    files dropped on the panel add tracks with the file on the Playlist at the song's start. Every edit
    is one Command.
*/
class AudioTracks final : public Panel,
                          public juce::FileDragAndDropTarget,
                          public juce::DragAndDropTarget,
                          private juce::ChangeListener,
                          private juce::Timer,
                          private juce::AsyncUpdater
{
public:
    static constexpr int rowHeight = 34;

    explicit AudioTracks (Session&);
    ~AudioTracks() override;

    /** The audio tracks shown, in Channel Rack order. */
    std::vector<model::ChannelId> getTracks() const;
    int getNumRows() const { return (int)rows.size(); }

    /** Each one undo step; `joined`: part of the step before (a knob drag). */
    bool setVolume (model::ChannelId, double db, bool joined = false);
    bool setPan (model::ChannelId, double pan, bool joined = false);
    bool setMuted (model::ChannelId, bool);
    bool rename (model::ChannelId, const juce::String&);
    /** Recording: whether Record audio records the track, and its input (from 1, 0 for the one chosen in
        Options › Audio; stereo: that input and the next). */
    bool setArmed (model::ChannelId, bool);
    bool setRecordInput (model::ChannelId, int input, bool stereo);
    std::vector<model::ChannelId> getArmedTracks() const;
    /** The level of the track's input (0..1) while armed or recording. */
    float getTrackLevel (model::ChannelId) const;
    /** Adds an empty audio track ("Track 3") to record into. */
    bool addRecordingTrack();
    /** Removes the track and its audio clips. */
    bool removeTrack (model::ChannelId);

    /** The one audio file the track's clips play, if they all play the same one. */
    std::optional<juce::File> getTrackFile (model::ChannelId) const;
    /** Called for "Move to Channel Rack": MainView makes a Sampler channel of the file and removes the
        track. */
    std::function<void (model::ChannelId, const juce::File&)> onMoveToRack;
    /** Called for "Open in Beat Slicer": a Beat Slicer channel with the track's file (the track stays). */
    std::function<void (const juce::File&)> onSlice;

    /** "2 clips, 3:12": what the track plays on the Playlist. */
    juce::String describeClips (model::ChannelId) const;

    /** Called with audio files to add as new tracks (MainView puts them on the Playlist). */
    std::function<void (const juce::StringArray&)> onAddFiles;
    /** The line beside Record audio (MainView says where the take went). */
    void setRecordStatus (const juce::String&);
    /** Where Record audio puts the take: a new track (nothing), or into an audio track (as another take where
        it already has audio). */
    std::optional<model::ChannelId> getRecordTarget() const;
    void setRecordTarget (std::optional<model::ChannelId>);
    /** Called to open an audio clip in Spinus Tune. */
    std::function<void (model::ClipId)> onOpenAudioClip;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    // Samples dragged from the Browser.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { fileDragEnter ({}, 0, 0); }
    void itemDragExit (const SourceDetails&) override { fileDragExit ({}); }

    /** Applies pending updates now (tests). */
    void updateNow() { handleUpdateNowIfNeeded(); }

    juce::Rectangle<int> getDefaultBounds() const override { return { 60, 90, 620, 260 }; }
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    void rebuild();
    void chooseFiles();
    bool perform (const juce::String& command, const juce::var& parameters, bool joined = false);
    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { rebuild(); }
    void timerCallback() override;

public:
    Session& session;

private:
    int historyListener = 0;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Component rowHolder;
    juce::Viewport viewport;
    juce::TextButton addButton, addTrackButton, recordButton;
    juce::ComboBox recordInto;
    std::optional<model::ChannelId> recordTarget;
    void updateRecordInto();
    juce::Label recordStatus;
    void toggleRecording();
    juce::Label emptyMessage;
    std::unique_ptr<core::FileDialog> chooser;
    bool dropHighlight = false;
};

} // namespace spinus::ui
