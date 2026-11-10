#include "DeviceControls.h"

namespace spinus::dsp
{

ParameterKnob::ParameterKnob (const juce::String& text)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, look::accent);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxTextColourId, look::text);
    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::textColourId, look::dim);
    label.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

ParameterKnob::ParameterKnob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                              const juce::String& text)
    : ParameterKnob (text)
{
    attach (state, parameterId);
}

void ParameterKnob::attach (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId)
{
    attachment = nullptr;
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterId, slider);
    parameter = state.getParameter (parameterId);
    startTimerHz (30);
}

void ParameterKnob::timerCallback()
{
    if (parameter == nullptr || slider.isMouseButtonDown())
        return;
    const auto now = (double)parameter->convertFrom0to1 (parameter->getValue());
    if (! juce::approximatelyEqual (slider.getValue(), now))
        slider.setValue (now, juce::dontSendNotification);
}

void ParameterKnob::resized()
{
    auto area = getLocalBounds();
    label.setBounds (area.removeFromTop (16));
    slider.setBounds (area);
}

ComboBoxAttachment::ComboBoxAttachment (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                                        juce::ComboBox& b)
    : juce::AudioProcessorValueTreeState::ComboBoxAttachment (state, parameterId, b), box (b),
      parameter (state.getParameter (parameterId))
{
    if (parameter != nullptr)
        startTimerHz (30);
}

void ComboBoxAttachment::timerCallback()
{
    // As JUCE's attachment shows a value: the items spread evenly over the setting's 0 to 1.
    const auto items = box.getNumItems();
    if (items < 1 || box.isPopupActive())
        return;
    const auto index = juce::roundToInt (parameter->getValue() * (float)(items - 1));
    if (index != box.getSelectedItemIndex())
        box.setSelectedItemIndex (index, juce::dontSendNotification);
}

ButtonAttachment::ButtonAttachment (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId,
                                    juce::Button& b)
    : juce::AudioProcessorValueTreeState::ButtonAttachment (state, parameterId, b), button (b),
      parameter (state.getParameter (parameterId))
{
    if (parameter != nullptr)
        startTimerHz (30);
}

void ButtonAttachment::timerCallback()
{
    if (button.isDown())
        return;
    const auto on = parameter->getValue() >= 0.5f;
    if (on != button.getToggleState())
        button.setToggleState (on, juce::dontSendNotification);
}

} // namespace spinus::dsp
