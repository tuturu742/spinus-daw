#pragma once

#include "TimelineControls.h"
#include "ai/AiService.h"
#include "session/Session.h"

namespace spinus::ui
{

/** Spinus Tune, graphical mode: a vocal audio clip's notes as blobs over a
    keyboard, with the pitch as sung (grey) and as it will sound (yellow).

    Drag a note up or down to move it (by semitones; Alt: freely), double-click to split it, Join two
    neighbours, straighten its Drift or change its Vibrato, Snap to the key, Reset. Every edit is one
    command (clip.setTune) and one undo step; the Tuner renders the result.

    The vocal is analysed by the AI service (ai.v1.listen.vocal) the first time; the analysis is kept by
    the Tuner, so later the window opens without AI.
*/
class SpinusTuneEditor final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    SpinusTuneEditor (Session&, model::ClipId, ai::AiService*);
    ~SpinusTuneEditor() override;

    /** The notes shown: the clip's tuned notes, or the analysis's notes before the first edit. */
    const std::vector<model::TuneNote>& getNotes() const noexcept { return notes; }
    bool isAnalysed() const noexcept { return track.has_value(); }
    juce::String getStatusText() const;

    // Edits (also used by the mouse). Each is one undo step; false if nothing changed.
    bool moveNotes (const std::vector<int>& indices, float semitones, bool snapToSemitones = true);
    bool splitNote (int index, double seconds);
    bool joinNotes (int first, int second);
    bool setDrift (const std::vector<int>& indices, float drift);
    bool setVibrato (const std::vector<int>& indices, float vibrato);
    bool snapToKey (const std::vector<int>& indices);
    bool reset (const std::vector<int>& indices);

    std::vector<int> getSelection() const;
    void select (std::vector<int>);

    void paint (juce::Graphics&) override;
    void resized() override;
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

    // A drag moving notes vertically.
    struct Drag
    {
        bool active = false;
        float startY = 0.0f;
        float semitones = 0.0f;
    } drag;

    juce::TextButton joinButton { "Join" }, snapButton { "Snap to key" }, resetButton { "Reset" };
    TimelineControls zoomControls { nullptr, {} };
    juce::Slider driftSlider, vibratoSlider;
    juce::Label driftLabel, vibratoLabel, status;

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
