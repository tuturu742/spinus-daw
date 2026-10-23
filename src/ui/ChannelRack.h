#pragma once

#include "Workspace.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The Channel Rack (F6): one row per channel with mute, name, volume and pan, and the
    step sequencer for the current pattern. A step is a 1/16 note (or a 1/16 triplet, per pattern) at C5;
    clicking it adds or removes that note. Every edit is one Command (one undo step).
*/
class ChannelRack final : public Panel,
                          public juce::DragAndDropTarget,
                          public juce::FileDragAndDropTarget,
                          private juce::ChangeListener,
                          private juce::Timer,
                          private juce::AsyncUpdater
{
public:
    static constexpr int stepPitch = 60; // C5 in FL's naming
    static constexpr float stepVelocity = 0.78f;

    explicit ChannelRack (Session&);
    ~ChannelRack() override;

    /** Adds a channel with a bundled instrument (`stockType`), or an empty one. Returns false on failure. */
    bool addChannel (const juce::String& stockType, const juce::String& name);

    /** Adds a channel playing this instrument (a bundled device or an installed plugin). */
    bool addChannelWith (const model::Device& instrument, const juce::String& name);

    /** Adds a Sampler channel playing this audio file, named after it (FL: drop a sample on the rack).
        Fails, adding nothing, if the file is not audio Spinus can read. */
    bool addSamplerChannel (const juce::File&);

    /** Adds a Beat Slicer channel with this file as its loop, and opens its window, to map the file's
        hits to keys (FL: open in Slicex). */
    bool addBeatSlicerChannel (const juce::File&);
    /** The same with a Sampler channel's sample (the channel stays). */
    bool sliceSamplerChannel (model::ChannelId);
    /** True if the channel is a Sampler holding a sample. */
    bool hasSamplerSample (model::ChannelId) const;

    /** Files longer than this (or whose length is unknown until decoded) are long audio, such as vocals:
        dropped on the rack, they go to onLongAudio instead of becoming Sampler channels. */
    static constexpr double longAudioSeconds = 20.0;
    static bool isLongAudio (const juce::File&);
    /** Called with long audio files dropped on the rack (MainView adds them as audio tracks). Holding
        Shift while dropping makes them Sampler channels all the same. */
    std::function<void (const juce::StringArray&)> onLongAudio;

    // Audio files dropped from the file manager become Sampler channels.
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

    // Instruments dragged from the Browser add a channel.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;

    /** True when the channel's notes in the current pattern are more than steps can show (a note off the
        sixteenth grid, longer than a step, at another pitch than C5, after the first bar, or two on one
        step). Its row then
        shows a small picture of the notes instead of steps, and clicking it opens the Piano Roll (FL). */
    bool hasPianoRollNotes (model::ChannelId) const;
    /** True while the channel's instrument runs in a separate process and has stopped there. */
    bool isInstrumentStopped (model::ChannelId) const;
    /** The channel's instrument plugin could not be loaded (not installed here, or it failed to start): its
        name shows "Missing:" in red, as effects do in the Mixer. */
    bool isInstrumentMissing (model::ChannelId) const;

    /** Called to show the Piano Roll (clicking a channel's note picture). */
    std::function<void()> onOpenPianoRoll;
    /** Clicking a channel's note picture: selects the channel and shows the Piano Roll. */
    bool openNotes (model::ChannelId);

    /** Adds or removes the step note; creates a pattern first if there is none. */
    bool toggleStep (model::ChannelId, int step);
    bool isStepOn (model::ChannelId, int step) const;
    /** One bar of the current pattern's time signature, in steps. */
    int getNumSteps() const;
    /** The current pattern's step: a sixteenth (240 ticks), or a sixteenth-note triplet (160). */
    model::Ticks getStepTicks() const;
    /** Switches the current pattern's steps between sixteenths and triplets. */
    bool setTripletSteps (bool);
    bool hasTripletSteps() const;
    /** The current pattern's swing, 0 to 1 (pattern.setSwing; `joined`: part of a knob drag). */
    double getSwing() const;
    bool setSwing (double swing, bool joined = false);

    /** `joined`: part of the same undo step as the change before (a knob drag). */
    bool setChannelVolume (model::ChannelId, double db, bool joined = false);
    bool setChannelPan (model::ChannelId, double pan, bool joined = false);
    bool setChannelMuted (model::ChannelId, bool);
    bool renameChannel (model::ChannelId, const juce::String&);
    bool setChannelColour (model::ChannelId, juce::uint32 argb);

    /** Groups (FL's filter groups): the rack shows all channels, one group, or those in none. */
    bool setChannelGroup (model::ChannelId, const juce::String& group);
    juce::StringArray getGroups() const; // in the order they first appear
    static constexpr const char *allGroups = "", *noGroup = "\x01";
    void showGroup (const juce::String& group); // allGroups, noGroup or a group's name
    /** The group's channels play into a new mixer insert named after it (a bus): one undo step. */
    bool routeGroupToBus (const juce::String& group);
    const juce::String& getShownGroup() const noexcept { return shownGroup; }
    int getNumShownChannels() const noexcept { return (int)rows.size(); }
    bool removeChannel (model::ChannelId);

    /** Freezes or unfreezes the channel (model/Freeze.h). */
    bool setChannelFrozen (model::ChannelId, bool frozen);

    /** Opens the channel's instrument window (FL: clicking the channel button). */
    bool showInstrumentWindow (model::ChannelId);

    /** Writes the channel's Beat Slicer slices as notes (see spinus::slicesToPattern). */
    bool slicesToPattern (model::ChannelId);

    int getNumRows() const { return (int)rows.size(); }

    /** Applies pending updates now (tests). */
    void updateNow() { handleUpdateNowIfNeeded(); }
    int getPlayingStep() const noexcept { return playingStep; }

    juce::Rectangle<int> getDefaultBounds() const override { return { 24, 24, 640, 320 }; }
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    class Row;

    void rebuild();
    bool perform (const juce::String& command, const juce::var& parameters, bool joined = false);
    const std::vector<model::Note>* notesOf (model::ChannelId) const;
    void showAddMenu();
    bool dropHighlight = false;
    // Rebuilds run after the current event: a click or knob drag on a row triggers a command whose
    // notification would otherwise delete that row while it is still handling the event.
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
    juce::TextButton addButton, tripletButton;
    juce::Slider swingKnob;
    juce::Label swingLabel;
    juce::ComboBox groupFilter;
    juce::String shownGroup;
    void updateGroupFilter();
    int playingStep = -1;
    bool wasRendering = false;
    std::vector<model::DeviceId> stoppedDevices, missingDevices;
};

} // namespace spinus::ui
