#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/ParameterText.h"

/** Parameter helpers shared by Spinus's own effects: ranges and how values read. */
namespace spinus::dsp::param
{

inline juce::String decibels (float v, int) { return juce::String (v, 1) + " dB"; }
inline juce::String percent (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }
inline juce::String hertz (float v, int)
{
    return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz" : juce::String (v / 1000.0f, 2) + " kHz";
}
inline juce::String milliseconds (float v, int)
{
    if (v < 10.0f)
        return juce::String (v, 2) + " ms";
    if (v < 1000.0f)
        return juce::String (v, v < 100.0f ? 1 : 0) + " ms";
    return juce::String (v / 1000.0f, 2) + " s";
}
inline juce::String ratio (float v, int) { return juce::String (v, v < 10.0f ? 1 : 0) + ":1"; }
inline juce::String plain (float v, int) { return juce::String (v, 2); }

/** A range that moves evenly on a log scale (frequencies, times, Q). */
inline juce::NormalisableRange<float> logRange (float min, float max)
{
    return { min, max, [] (float lo, float hi, float x) { return lo * std::pow (hi / lo, x); },
             [] (float lo, float hi, float v) { return std::log (v / lo) / std::log (hi / lo); },
             [] (float lo, float hi, float v) { return juce::jlimit (lo, hi, v); } };
}

inline std::unique_ptr<juce::AudioParameterFloat> floatParameter (const juce::String& id, const juce::String& name,
                                                                  const juce::NormalisableRange<float>& range,
                                                                  float value,
                                                                  std::function<juce::String (float, int)> text)
{
    return std::make_unique<spinus::dsp::param::FloatSetting> (
        juce::ParameterID { id, 1 }, name, range, value,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction (std::move (text)));
}

inline std::unique_ptr<juce::AudioParameterChoice> choiceParameter (const juce::String& id, const juce::String& name,
                                                                    const juce::StringArray& items, int value)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, value);
}

} // namespace spinus::dsp::param
