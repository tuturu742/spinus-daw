#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp::param
{

/** A setting as the audio thread reads it: the parameter's own value. A host's automation sets that with
    setValue(), which does not tell the tree state, so the tree's raw value (getRawParameterValue) would stay
    where the knob was last turned and the sound would not follow the automation. Reads like that raw value
    (`setting->load()`): a choice's index, a switch's 0 or 1. Lock-free. */
class Live
{
public:
    Live() = default;
    Live (std::nullptr_t) noexcept {}
    explicit Live (juce::RangedAudioParameter* p) noexcept
        : any (p), number (dynamic_cast<juce::AudioParameterFloat*> (p)),
          choice (dynamic_cast<juce::AudioParameterChoice*> (p)), toggle (dynamic_cast<juce::AudioParameterBool*> (p))
    {
    }

    float load() const noexcept
    {
        if (number != nullptr)
            return number->get();
        if (choice != nullptr)
            return (float)choice->getIndex();
        if (toggle != nullptr)
            return toggle->get() ? 1.0f : 0.0f;
        return any != nullptr ? any->convertFrom0to1 (any->getValue()) : 0.0f;
    }

    const Live* operator->() const noexcept { return this; }
    explicit operator bool() const noexcept { return any != nullptr; }

private:
    juce::RangedAudioParameter* any = nullptr;
    juce::AudioParameterFloat* number = nullptr;
    juce::AudioParameterChoice* choice = nullptr;
    juce::AudioParameterBool* toggle = nullptr;
};

/** The setting `id` of a device's state, read live (see Live). */
inline Live live (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    return Live (state.getParameter (id));
}

} // namespace spinus::dsp::param
