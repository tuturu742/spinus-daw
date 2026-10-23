#include "StereoUtilityEditor.h"

#include "StereoUtilityProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

StereoUtilityEditor::StereoUtilityEditor (StereoUtilityProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    auto& state = p.getParameterState();
    gain = std::make_unique<ParameterKnob> (state, "gain", "Gain");
    pan = std::make_unique<ParameterKnob> (state, "pan", "Pan");
    width = std::make_unique<ParameterKnob> (state, "width", "Width");
    for (auto* k : { gain.get(), pan.get(), width.get() })
        addAndMakeVisible (*k);

    for (auto [button, id] : { std::pair { &mono, "mono" }, std::pair { &invertLeft, "invert_left" },
                               std::pair { &invertRight, "invert_right" } })
    {
        button->setColour (juce::ToggleButton::textColourId, look::text);
        button->setColour (juce::ToggleButton::tickColourId, look::accent);
        buttonAttachments.push_back (
            std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, id, *button));
        addAndMakeVisible (*button);
    }

    setResizable (true, true);
    setResizeLimits (420, 280, 900, 600);
    setSize (480, 300);
    startTimerHz (30);
}

StereoUtilityEditor::~StereoUtilityEditor() = default;

void StereoUtilityEditor::timerCallback()
{
    const auto& engine = processor.getEngine();
    bool changed = false;
    for (int c = 0; c < 2; ++c)
    {
        // Peaks fall back slowly so they can be read.
        const auto peak = juce::jmax (engine.getPeak (c), shownPeaks[(size_t)c] * 0.85f);
        changed = changed || std::abs (peak - shownPeaks[(size_t)c]) > 0.001f;
        shownPeaks[(size_t)c] = peak;
    }
    const auto correlation = engine.getCorrelation();
    changed = changed || std::abs (correlation - shownCorrelation) > 0.01f;
    shownCorrelation = correlation;
    if (changed)
        repaint (meters);
}

void StereoUtilityEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Stereo Utility", 10, 6, 200, 24, juce::Justification::centredLeft);

    g.setColour (look::panel);
    g.fillRoundedRectangle (meters.toFloat(), 5.0f);
    auto area = meters.reduced (10, 8);
    g.setFont (juce::FontOptions (11.0f));

    // Left and right levels, -60 dB to 0 dB.
    for (int c = 0; c < 2; ++c)
    {
        auto row = area.removeFromTop (18);
        g.setColour (look::dim);
        g.drawText (c == 0 ? "L" : "R", row.removeFromLeft (16), juce::Justification::centredLeft);
        const auto db = juce::Decibels::gainToDecibels (shownPeaks[(size_t)c], -60.0f);
        const auto fill = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (look::background);
        g.fillRect (row.reduced (0, 4));
        g.setColour (db > -0.1f ? look::error : look::loop);
        g.fillRect (row.reduced (0, 4).withWidth (juce::roundToInt ((float)row.getWidth() * fill)));
        area.removeFromTop (4);
    }

    // Correlation: -1 (opposite: cancels in mono) .. +1 (the same on both sides: mono).
    auto row = area.removeFromTop (18);
    g.setColour (look::dim);
    g.drawText ("Alike", row.removeFromLeft (40), juce::Justification::centredLeft);
    g.setColour (look::background);
    g.fillRect (row.reduced (0, 4));
    const auto x = (float)row.getX() + (float)row.getWidth() * (shownCorrelation + 1.0f) / 2.0f;
    g.setColour (shownCorrelation < 0.0f ? look::error : look::accent);
    g.fillRect (juce::Rectangle<float> (x - 2.0f, (float)row.getY() + 2.0f, 4.0f, (float)row.getHeight() - 4.0f));
    g.setColour (look::dim);
    g.drawText ("-1", row.removeFromLeft (20).translated (0, 14), juce::Justification::centredLeft);
    g.drawText ("+1", row.removeFromRight (20).translated (0, 14), juce::Justification::centredRight);
}

void StereoUtilityEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);
    meters = area.removeFromTop (90);
    area.removeFromTop (10);

    auto buttons = area.removeFromRight (130);
    for (auto* b : { &mono, &invertLeft, &invertRight })
        b->setBounds (buttons.removeFromTop (30));

    const auto w = area.getWidth() / 3;
    for (auto* k : { gain.get(), pan.get(), width.get() })
        k->setBounds (area.removeFromLeft (w));
}

} // namespace spinus::dsp
