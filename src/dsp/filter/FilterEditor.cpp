#include "FilterEditor.h"

#include "FilterProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
constexpr int displayHeight = 130, sectionTitle = 22, rowHeight = 112;
constexpr float minHz = 20.0f, maxHz = 20000.0f, topDb = 18.0f, bottomDb = -36.0f;
} // namespace

FilterEditor::FilterEditor (FilterProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    auto& state = p.getParameterState();

    auto combo = [&] (juce::ComboBox& box, const char* id)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id));
        jassert (parameter != nullptr);
        box.addItemList (parameter->choices, 1);
        comboAttachments.push_back (std::make_unique<ComboAttachment> (state, id, box));
        addAndMakeVisible (box);
    };
    combo (type, "type");
    combo (slope, "slope");
    combo (lfoShape, "lfo_shape");
    combo (lfoSync, "lfo_sync");

    auto knob = [&] (std::unique_ptr<ParameterKnob>& k, const char* id, const char* label)
    {
        k = std::make_unique<ParameterKnob> (state, id, label);
        addAndMakeVisible (*k);
    };
    knob (cutoff, "cutoff", "Cutoff");
    knob (resonance, "resonance", "Resonance");
    knob (mix, "mix", "Mix");
    knob (gain, "gain", "Volume");
    knob (lfoAmount, "lfo_amount", "Amount");
    knob (lfoRate, "lfo_rate", "Rate");
    knob (lfoLength, "lfo_length", "Length");
    knob (envelopeAmount, "env_amount", "Amount");
    knob (envelopeAttack, "env_attack", "Attack");
    knob (envelopeRelease, "env_release", "Release");

    setResizable (true, true);
    setResizeLimits (560, 440, 1200, 900);
    setSize (620, 470);
    timerCallback();
    startTimerHz (30);
}

FilterEditor::~FilterEditor() = default;

juce::Rectangle<int> FilterEditor::getDisplayArea() const
{
    return getLocalBounds().reduced (12).withTrimmedTop (30).removeFromTop (displayHeight);
}

void FilterEditor::timerCallback()
{
    // Rate for a free LFO, length for a synced one.
    const bool synced = processor.getSettings().lfoSync;
    lfoRate->setVisible (! synced);
    lfoLength->setVisible (synced);

    // Before any audio has run, the cutoff knob's own value.
    auto now = processor.getModulatedCutoff();
    if (now < 0.0f)
        now = processor.getSettings().cutoffHz;
    if (std::abs (now - shownCutoff) > 0.5f)
    {
        shownCutoff = now;
        repaint (getDisplayArea());
    }
}

void FilterEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Filter", 10, 6, 140, 24, juce::Justification::centredLeft);

    // The response at the cutoff the LFO and envelope moved it to.
    const auto area = getDisplayArea().toFloat();
    g.setColour (look::panel);
    g.fillRoundedRectangle (area, 5.0f);

    auto xFor = [&] (float hz)
    { return area.getX() + area.getWidth() * std::log (hz / minHz) / std::log (maxHz / minHz); };
    auto yFor = [&] (float db)
    { return area.getY() + area.getHeight() * (topDb - juce::jlimit (bottomDb, topDb, db)) / (topDb - bottomDb); };

    g.setFont (juce::FontOptions (10.0f));
    for (float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        g.setColour (look::dim.withAlpha (0.3f));
        g.drawVerticalLine ((int)xFor (hz), area.getY(), area.getBottom());
        g.setColour (look::dim);
        g.drawText (hz < 1000.0f    ? "100 Hz"
                    : hz < 10000.0f ? "1 kHz"
                                    : "10 kHz",
                    (int)xFor (hz) + 3, (int)area.getBottom() - 14, 50, 12, juce::Justification::left);
    }
    g.setColour (look::dim.withAlpha (0.3f));
    g.drawHorizontalLine ((int)yFor (0.0f), area.getX(), area.getRight());

    const auto settings = processor.getSettings();
    const auto at = shownCutoff > 0.0f ? shownCutoff : settings.cutoffHz;
    juce::Path curve;
    for (int i = 0; i <= 200; ++i)
    {
        const auto hz = minHz * std::pow (maxHz / minHz, (float)i / 200.0f);
        const auto db = juce::Decibels::gainToDecibels (FilterEngine::magnitudeAt (settings, at, hz), -100.0f);
        const juce::Point<float> point (xFor (hz), yFor (db));
        if (i == 0)
            curve.startNewSubPath (point);
        else
            curve.lineTo (point);
    }
    g.setColour (look::accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f));
    g.fillEllipse (xFor (juce::jlimit (minHz, maxHz, at)) - 4.0f, yFor (0.0f) - 4.0f, 8.0f, 8.0f);

    g.setColour (look::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    for (auto [r, text] : { std::pair { filterTitle, "Filter" }, std::pair { outputTitle, "Output" },
                            std::pair { lfoTitle, "LFO" }, std::pair { envelopeTitle, "Envelope follower" } })
        g.drawText (text, r, juce::Justification::centredLeft);
}

void FilterEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30 + displayHeight + 8);

    auto layoutRow = [&] (juce::Rectangle<int> row, juce::ComboBox* comboA, juce::ComboBox* comboB,
                          std::initializer_list<ParameterKnob*> before, std::initializer_list<ParameterKnob*> after)
    {
        const auto knobs = (float)(before.size() + after.size()) + (comboA != nullptr ? 1.3f : 0.0f);
        const auto unit = juce::jmin (90, (int)((float)row.getWidth() / knobs));
        for (auto* k : before)
            k->setBounds (row.removeFromLeft (unit));
        if (comboA != nullptr)
        {
            auto stack = row.removeFromLeft (unit * 13 / 10).reduced (4, 0);
            const auto top = stack.getCentreY() - 32;
            comboA->setBounds (stack.getX(), top, stack.getWidth(), 26);
            comboB->setBounds (stack.getX(), top + 34, stack.getWidth(), 26);
        }
        for (auto* k : after)
            k->setBounds (row.removeFromLeft (unit));
        return row;
    };

    auto first = area.removeFromTop (sectionTitle + rowHeight);
    const auto half = first.getWidth() * 4 / 7;
    filterTitle = first.removeFromTop (sectionTitle).withWidth (half);
    outputTitle = filterTitle.withX (filterTitle.getRight());
    layoutRow (first.removeFromLeft (half), &type, &slope, {}, { cutoff.get(), resonance.get() });
    layoutRow (first, nullptr, nullptr, { mix.get(), gain.get() }, {});

    auto second = area.removeFromTop (sectionTitle + rowHeight + 8).withTrimmedTop (8);
    lfoTitle = second.removeFromTop (sectionTitle).withWidth (half);
    envelopeTitle = lfoTitle.withX (lfoTitle.getRight());
    auto lfoArea = second.removeFromLeft (half);
    layoutRow (lfoArea, &lfoShape, &lfoSync, { lfoAmount.get() }, { lfoRate.get() });
    lfoLength->setBounds (lfoRate->getBounds());
    layoutRow (second, nullptr, nullptr, { envelopeAmount.get(), envelopeAttack.get(), envelopeRelease.get() }, {});
}

} // namespace spinus::dsp
