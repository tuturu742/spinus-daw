#include "DuckerEditor.h"

#include "DuckerProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

DuckerEditor::DuckerEditor (DuckerProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    auto& state = p.getParameterState();
    trigger.addItemList ({ "Sidechain", "Every beat", "Every half beat", "Every 2 beats" }, 1);
    triggerAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "trigger", trigger);
    addAndMakeVisible (trigger);

    amount = std::make_unique<ParameterKnob> (state, "amount", "Amount");
    release = std::make_unique<ParameterKnob> (state, "release", "Release");
    curve = std::make_unique<ParameterKnob> (state, "curve", "Curve");
    for (auto* k : { amount.get(), release.get(), curve.get() })
        addAndMakeVisible (*k);

    setResizable (true, true);
    setResizeLimits (460, 330, 1000, 700);
    setSize (520, 350);
    timerCallback();
    startTimerHz (30);
}

DuckerEditor::~DuckerEditor() = default;

juce::String DuckerEditor::getStatusText() const { return status; }

void DuckerEditor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (processor.getEngine().getSidechainPeak() > 0.003f)
        lastSidechainSound = now;

    juce::String text;
    if (processor.getSettings().trigger != DuckerSettings::Trigger::sidechain)
        text = "Dips in time with the song while it plays. No sidechain needed.";
    else if (lastSidechainSound != 0 && now - lastSidechainSound < 2000)
        text = "Sound is arriving on the sidechain.";
    else
        text = "No sound on the sidechain yet. In the Mixer, right-click this insert's name and choose "
               "Sidechain from, then the insert to follow (e.g. the kick's).";

    const auto gain = processor.getEngine().getGain();
    if (text != status)
    {
        status = std::move (text);
        repaint (statusArea);
    }
    if (std::abs (gain - shownGain) > 0.002f)
    {
        shownGain = gain;
        repaint (meter);
    }
}

void DuckerEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Ducker", 10, 6, 140, 24, juce::Justification::centredLeft);

    // The dip after one hit: volume (up) over time (right).
    const auto settings = processor.getSettings();
    const auto area = display.toFloat();
    g.setColour (look::panel);
    g.fillRoundedRectangle (area, 5.0f);
    const auto length = (DuckerEngine::attackSeconds + settings.release) * 1.25f;
    juce::Path shape;
    shape.startNewSubPath (area.getX(), area.getBottom());
    for (int i = 0; i <= 160; ++i)
    {
        const auto t = length * (float)i / 160.0f;
        shape.lineTo (area.getX() + area.getWidth() * (float)i / 160.0f,
                      area.getBottom() - area.getHeight() * 0.9f * DuckerEngine::gainAt (settings, t));
    }
    shape.lineTo (area.getRight(), area.getBottom());
    shape.closeSubPath();
    g.setColour (look::accent.withAlpha (0.25f));
    g.fillPath (shape);
    g.setColour (look::accent);
    g.strokePath (shape, juce::PathStrokeType (2.0f));
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (juce::String (juce::roundToInt (length * 1000.0f)) + " ms", display.reduced (6, 2),
                juce::Justification::bottomRight);

    // The volume right now.
    g.setColour (look::panel);
    g.fillRoundedRectangle (meter.toFloat(), 3.0f);
    g.setColour (look::loop);
    const auto level = meter.toFloat().reduced (3.0f);
    g.fillRect (level.withTop (level.getBottom() - level.getHeight() * juce::jlimit (0.0f, 1.0f, shownGain)));

    g.setColour (status.startsWith ("No sound") ? look::dim : look::text);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (status, statusArea, juce::Justification::centredLeft, 2);
}

void DuckerEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);
    auto top = area.removeFromTop (juce::jmax (90, area.getHeight() - 200));
    meter = top.removeFromRight (18);
    top.removeFromRight (8);
    display = top;
    statusArea = area.removeFromTop (40);

    auto controls = area.reduced (0, 4);
    amount->setBounds (controls.removeFromLeft (controls.getWidth() * 2 / 5));
    auto right = controls;
    trigger.setBounds (right.removeFromTop (28).reduced (8, 0));
    right.removeFromTop (6);
    release->setBounds (right.removeFromLeft (right.getWidth() / 2));
    curve->setBounds (right);
}

} // namespace spinus::dsp
