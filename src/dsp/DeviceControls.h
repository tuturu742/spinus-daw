#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Colours shared by the windows of Spinus's own devices (they also run in other hosts, so they do not
    use Spinus's theme). */
namespace look
{
inline const juce::Colour background { 0xff23272e }, panel { 0xff2c313a }, text { 0xffd8dee9 }, dim { 0xff8a93a3 },
    accent { 0xffe8c547 }, loop { 0xff5fae6a }, error { 0xffd66a6a };
} // namespace look

/** A labelled knob attached to a parameter; attach() can move it to another parameter (a Drum Machine
    knob follows the selected pad). */
class ParameterKnob final : public juce::Component
{
public:
    explicit ParameterKnob (const juce::String& label);
    ParameterKnob (juce::AudioProcessorValueTreeState&, const juce::String& parameterId, const juce::String& label);

    void attach (juce::AudioProcessorValueTreeState&, const juce::String& parameterId);
    juce::Slider& getSlider() noexcept { return slider; }

    void resized() override;

private:
    juce::Slider slider;
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

} // namespace spinus::dsp
