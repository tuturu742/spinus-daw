#include "TempoControl.h"

#include "Theme.h"

namespace spinus::ui
{

TempoControl::TempoControl()
{
    setHelpText ("Tempo: drag up or down, or scroll, to change it; double-click to type a value");
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);

    editor.setEditable (false, true, true);
    editor.setJustificationType (juce::Justification::centred);
    // The box closes however the edit ends: Enter, Escape, a click elsewhere, or Enter with nothing changed
    // (which changes no text). A value outside 20-999 BPM is kept within it; text that is no number is ignored.
    editor.onEditorHide = [this] { editor.setVisible (false); };
    editor.onTextChange = [this]
    {
        editor.setVisible (false);
        if (const auto value = parseTyped (editor.getText()))
            commit (*value, false);
    };
    addChildComponent (editor);
}

std::optional<double> TempoControl::parseTyped (const juce::String& text)
{
    // "128", "128.5", "128,5" or "128 BPM".
    const auto number = text.trim().upToFirstOccurrenceOf (" ", false, false).replaceCharacter (',', '.');
    if (! number.containsAnyOf ("0123456789") || ! number.containsOnly ("0123456789."))
        return std::nullopt;
    return juce::jlimit (minBpm, maxBpm, number.getDoubleValue());
}

void TempoControl::wheelStep (double deltaBpm, juce::uint32 nowMs)
{
    const bool continuing = lastWheelMs.has_value() && nowMs - lastWheelMs.value() < wheelGapMs;
    lastWheelMs = nowMs;
    commit (shown + deltaBpm, continuing);
}

void TempoControl::setTempo (double bpm)
{
    if (! dragging && ! juce::exactlyEqual (shown, bpm))
    {
        shown = bpm;
        repaint();
    }
}

void TempoControl::commit (double bpm, bool continuing)
{
    bpm = juce::jlimit (minBpm, maxBpm, std::round (bpm * 100.0) / 100.0);
    shown = bpm;
    repaint();
    if (onCommit)
        onCommit (bpm, continuing);
}

void TempoControl::paint (juce::Graphics& g)
{
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&getLookAndFeel());
    g.setColour (lf != nullptr ? lf->getTheme().get ("background") : juce::Colours::black);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
    g.setColour (lf != nullptr ? lf->getTheme().get (dragging ? "accent" : "text") : juce::Colours::white);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText (juce::String (shown, 2), getLocalBounds().withTrimmedRight (34), juce::Justification::centredRight);
    g.setColour (lf != nullptr ? lf->getTheme().get ("textDim") : juce::Colours::grey);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("BPM", getLocalBounds().removeFromRight (30), juce::Justification::centredLeft);
}

void TempoControl::resized() { editor.setBounds (getLocalBounds()); }

void TempoControl::mouseDown (const juce::MouseEvent&)
{
    dragStartValue = shown;
    dragging = false;
}

void TempoControl::mouseDrag (const juce::MouseEvent& e)
{
    // 1 BPM per 4 pixels; hold Shift for 0.1 BPM steps.
    const auto step = e.mods.isShiftDown() ? 0.1 : 1.0;
    const auto value =
        juce::jlimit (minBpm, maxBpm, dragStartValue - std::trunc (e.getDistanceFromDragStartY() / 4.0) * step);
    dragging = true;
    shown = std::round (value * 10.0) / 10.0;
    repaint();
}

void TempoControl::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        if (! juce::exactlyEqual (shown, dragStartValue))
            commit (shown, false);
    }
}

void TempoControl::mouseDoubleClick (const juce::MouseEvent&)
{
    editor.setText (juce::String (shown, 2), juce::dontSendNotification);
    editor.setVisible (true);
    editor.showEditor();
}

void TempoControl::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (! juce::exactlyEqual (wheel.deltaY, 0.0f))
        wheelStep (wheel.deltaY > 0 ? 1.0 : -1.0, juce::Time::getMillisecondCounter());
}

} // namespace spinus::ui
