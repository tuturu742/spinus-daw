#pragma once

#include "TimelineControls.h"
#include "ai/AiService.h"
#include "session/Session.h"

namespace spinus::ui
{

/** Spinus Tune, graphical mode: a vocal audio clip's notes as blobs over a
    keyboard, with the pitch as sung (grey) and as it will sound (yellow).

    Drag a note up or down to move it (by semitones; Alt: freely), sideways to move it in time, or by an edge
    to make it longer or shorter (its neighbours stay where they are); double-click to split it, Join two
    neighbours, straighten its Drift, change its Vibrato, Volume or Formant, Snap to the key, Reset. Every edit is one
    command (clip.setTune) and one undo step; the Tuner renders the result.

    The vocal is analysed by the AI service (ai.v1.listen.vocal) the first time; the analysis is kept by
    the Tuner, so later the window opens without AI.
*/
class SpinusTuneEditor final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    /** The toolbar's height, as in the Piano Roll, the Playlist and the Automation editor. */
    static constexpr int toolbarHeight = 30;
    /** Narrow enough for most screens, wide enough for every control in the toolbar. */
    static constexpr int minimumWidth = 1100;

    SpinusTuneEditor (Session&, model::ClipId, ai::AiService*);
    ~SpinusTuneEditor() override;

    /** The notes shown: the clip's tuned notes, or the analysis's notes before the first edit. */
    const std::vector<model::TuneNote>& getNotes() const noexcept { return notes; }
    bool isAnalysed() const noexcept { return track.has_value(); }
    /** Where a note is drawn, in this component. */
    juce::Rectangle<float> getNoteBounds (int index) const
    {
        return juce::isPositiveAndBelow (index, (int)notes.size()) ? noteBounds (notes[(size_t)index])
                                                                   : juce::Rectangle<float>();
    }
    juce::String getStatusText() const;

    // Edits (also used by the mouse). Each is one undo step; false if nothing changed.
    bool moveNotes (const std::vector<int>& indices, float semitones, bool snapToSemitones = true);
    /** Later (earlier when negative), as far as the neighbours and the clip's ends allow. */
    bool moveInTime (const std::vector<int>& indices, double seconds);
    /** Where a note starts and ends as it sounds (seconds into the clip), within its neighbours. */
    bool resizeNote (int index, double outputStart, double outputEnd);
    bool splitNote (int index, double seconds);
    bool joinNotes (int first, int second);
    bool setDrift (const std::vector<int>& indices, float drift);
    bool setVibrato (const std::vector<int>& indices, float vibrato);
    bool setVolume (const std::vector<int>& indices, float gainDb);
    bool setFormant (const std::vector<int>& indices, float semitones);
    bool snapToKey (const std::vector<int>& indices);
    bool reset (const std::vector<int>& indices);

    std::vector<int> getSelection() const;
    void select (std::vector<int>);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Zooms the time around `x` (none: the grid's middle); fit shows the whole clip. */
    void zoom (double factor, std::optional<float> x = {});
    void fit();
    double getPixelsPerSecond() const noexcept { return pixelsPerSecond; }

private:
    void load();
    void analyse();
    bool commit (const std::vector<model::TuneNote>&, const juce::String& description);
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    juce::Rectangle<float> noteBounds (const model::TuneNote&) const;
    int noteAt (juce::Point<float>) const;
    /** -1 on a note's start edge, 1 on its end edge, 0 elsewhere. */
    int edgeAt (int index, juce::Point<float>) const;
    // The notes with an edit applied (also shown while dragging).
    std::vector<model::TuneNote> withPitchMoved (std::vector<model::TuneNote>, const std::vector<int>&, float semitones,
                                                 bool snap) const;
    std::vector<model::TuneNote> withTimeMoved (std::vector<model::TuneNote>, const std::vector<int>&,
                                                double seconds) const;
    std::vector<model::TuneNote> withResized (std::vector<model::TuneNote>, int index, double outputStart,
                                              double outputEnd) const;
    std::vector<model::TuneNote> shownNotes() const;
    float timeToX (double seconds) const;
    double xToTime (float x) const;
    float pitchToY (float pitch) const;
    juce::File sourceFile() const;
    std::vector<int> selectedOrAll() const;

    Session& session;
    model::ClipId clip;
    ai::AiService* service;
    int historyListener = 0, aiListener = 0;
    juce::String job, problem;

    std::vector<model::TuneNote> notes;
    std::optional<std::vector<float>> track; // pitch as sung (MIDI per 5 ms frame, 0 where none)
    double frameSeconds = 0.005, duration = 1.0;
    std::set<int> selected;

    // View.
    juce::Rectangle<int> grid, keyboard;
    double viewStart = 0.0, pixelsPerSecond = 120.0;
    float lowestPitch = 48.0f, rowHeight = 14.0f;

    // A drag moving notes (up or down, and in time), or one note's edge (-1 its start, 1 its end).
    struct Drag
    {
        bool active = false;
        juce::Point<float> start;
        float semitones = 0.0f;
        double seconds = 0.0;
        int note = -1, edge = 0;
    } drag;

    juce::TextButton joinButton { "Join" }, snapButton { "Snap to key" }, resetButton { "Reset" };
    TimelineControls zoomControls { nullptr, {} };
    juce::Slider driftSlider, vibratoSlider, volumeSlider, formantSlider;
    juce::Label driftLabel, vibratoLabel, volumeLabel, formantLabel, status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpinusTuneEditor)
};

/** The editor in its own window. */
class SpinusTuneWindow final : public juce::DocumentWindow
{
public:
    SpinusTuneWindow (Session&, model::ClipId, ai::AiService*);
    void closeButtonPressed() override;
    SpinusTuneEditor& getEditor() noexcept { return *editor; }
    std::function<void()> onClose;

private:
    SpinusTuneEditor* editor = nullptr;
};

} // namespace spinus::ui
