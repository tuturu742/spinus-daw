#include "TuneEditor.h"

#include "TuneProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
juce::String noteName (int note)
{
    // FL's naming: MIDI 60 is C5.
    return TuneProcessor::getKeyNames()[note % 12] + juce::String (note / 12);
}
} // namespace

TuneEditor::TuneEditor (TuneProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    auto& state = p.getParameterState();

    auto combo = [&] (juce::ComboBox& box, juce::Label& label, const char* id, const char* text)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id));
        jassert (parameter != nullptr);
        box.addItemList (parameter->choices, 1);
        comboAttachments.push_back (
            std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, id, box));
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, look::dim);
        label.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (box);
        addAndMakeVisible (label);
    };
    combo (key, keyLabel, "key", "Key");
    combo (scale, scaleLabel, "scale", "Scale");
    combo (formants, formantsLabel, "formants", "Formants");
    combo (range, rangeLabel, "range", "Range");

    retune = std::make_unique<ParameterKnob> (state, "retune", "Retune speed");
    humanize = std::make_unique<ParameterKnob> (state, "humanize", "Humanize");
    mix = std::make_unique<ParameterKnob> (state, "mix", "Mix");
    for (auto* k : { retune.get(), humanize.get(), mix.get() })
        addAndMakeVisible (*k);

    setResizable (true, true);
    setResizeLimits (480, 380, 1000, 760);
    setSize (540, 400);
    timerCallback();
    startTimerHz (30);
}

TuneEditor::~TuneEditor() = default;

juce::String TuneEditor::getStatusText() const
{
    if (shownPitch <= 0.0f || shownTarget < 0)
        return "No pitch";
    const auto nearest = juce::roundToInt (shownPitch);
    const auto cents = juce::roundToInt ((shownPitch - (float)nearest) * 100.0f);
    return noteName (nearest) + " " + (cents >= 0 ? "+" : "") + juce::String (cents) + " cents -> " +
           noteName (shownTarget);
}

void TuneEditor::timerCallback()
{
    const auto& engine = processor.getEngine();
    const auto pitch = engine.getInputPitch(), correction = engine.getCorrection();
    const auto target = engine.getTargetNote();
    if (std::abs (pitch - shownPitch) > 0.01f || target != shownTarget ||
        std::abs (correction - shownCorrection) > 0.01f)
    {
        shownPitch = pitch;
        shownTarget = target;
        shownCorrection = correction;
        repaint (display.getUnion (keyboard));
    }
}

void TuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Spinus Tune", 10, 6, 200, 24, juce::Justification::centredLeft);

    // The note heard and where it goes; under it, how far off it was (the needle) and the correction.
    g.setColour (look::panel);
    g.fillRoundedRectangle (display.toFloat(), 5.0f);
    auto area = display.reduced (12, 8);
    const bool pitched = shownPitch > 0.0f && shownTarget >= 0;
    g.setColour (pitched ? look::text : look::dim);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText (pitched ? noteName (shownTarget) : juce::String ("-"), area.removeFromTop (34),
                juce::Justification::centred);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (look::dim);
    g.drawText (getStatusText(), area.removeFromTop (18), juce::Justification::centred);

    const auto bar = area.removeFromTop (22).reduced (0, 6).toFloat();
    g.setColour (look::background);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (look::dim);
    g.drawVerticalLine ((int)bar.getCentreX(), bar.getY() - 3.0f, bar.getBottom() + 3.0f);
    if (pitched)
    {
        // -100 .. +100 cents from the target, across the bar.
        const auto off = juce::jlimit (-1.0f, 1.0f, shownPitch - (float)shownTarget);
        const auto x = bar.getCentreX() + off * bar.getWidth() * 0.5f;
        g.setColour (look::accent.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (x, bar.getCentreX()), bar.getY(),
                                                                juce::jmax (x, bar.getCentreX()), bar.getBottom()));
        g.setColour (look::accent);
        g.fillRect (x - 1.5f, bar.getY() - 3.0f, 3.0f, bar.getHeight() + 6.0f);
    }

    // One octave: the scale's notes lit, the target brightest.
    const auto settings = processor.getSettings();
    const auto keyWidth = (float)keyboard.getWidth() / 12.0f;
    for (int note = 0; note < 12; ++note)
    {
        const auto r = juce::Rectangle<float> ((float)keyboard.getX() + keyWidth * (float)note, (float)keyboard.getY(),
                                               keyWidth, (float)keyboard.getHeight())
                           .reduced (1.5f);
        const bool inScale = TuneEngine::isInScale (note, settings.key, settings.scale);
        const bool isTarget = pitched && shownTarget % 12 == note;
        g.setColour (isTarget ? look::accent : inScale ? look::panel.brighter (0.35f) : look::panel);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (isTarget ? look::background : inScale ? look::text : look::dim.withAlpha (0.6f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (TuneProcessor::getKeyNames()[note], r, juce::Justification::centredBottom);
    }
}

void TuneEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);
    display = area.removeFromTop (92);
    area.removeFromTop (8);
    keyboard = area.removeFromTop (34);
    area.removeFromTop (10);

    auto combos = area.removeFromTop (52);
    const auto width = combos.getWidth() / 4;
    for (auto [box, label] : { std::pair { &key, &keyLabel }, std::pair { &scale, &scaleLabel },
                               std::pair { &formants, &formantsLabel }, std::pair { &range, &rangeLabel } })
    {
        auto column = combos.removeFromLeft (width).reduced (4, 0);
        label->setBounds (column.removeFromTop (20));
        box->setBounds (column.removeFromTop (26));
    }

    area.removeFromTop (8);
    const auto third = area.getWidth() / 3;
    retune->setBounds (area.removeFromLeft (third));
    humanize->setBounds (area.removeFromLeft (third));
    mix->setBounds (area);
}

} // namespace spinus::dsp
