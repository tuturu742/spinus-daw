#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstring>
#include <optional>

/** Settings typed as text, the one rule everything follows (the knobs' text boxes, the automation editor's
    Value box, device.setParameters' "text", the demo song generators):

    A number is in the units the setting shows. "35%", "220 ms", "-6 dB", "1.2 kHz", "4:1", "-12 st",
    "30% L". The unit may be left out; a plain number is then in the setting's smallest unit: milliseconds
    for times (a time showing "1.50 s" reads "1500" or "1.5 s"), hertz for frequencies ("1.2 kHz" or "1200"),
    percent for settings shown in percent ("35" is 35%), and the shown unit otherwise. A unit of another kind
    than the setting shows ("50%" for a setting in dB) is not read. A choice reads by its name ("High pass",
    any capitals). Values from 0 to 1 over a setting's whole range only come as numbers in an explicit field
    (device.setParameters' "values", the project file), never as text. */
namespace spinus::dsp::param
{

enum class UnitKind
{
    none, // a plain number
    percent,
    decibels,
    frequency,
    time,
    semitones,
    cents,
    degrees,
    ratio,
    unknown // something else: not checked against the setting's unit
};

/** A number read from text, in its kind's smallest unit (Hz, ms); left ("30% L") is negative. */
struct Reading
{
    double value = 0.0;
    UnitKind kind = UnitKind::none;
};

inline std::optional<Reading> read (const juce::String& text)
{
    auto t = text.trim().toLowerCase().replace (juce::CharPointer_UTF8 ("\xe2\x88\x92"), "-"); // a real minus
    int i = 0;
    const int n = t.length();
    if (i < n && (t[i] == '+' || t[i] == '-'))
        ++i;
    bool digits = false;
    while (i < n && (juce::CharacterFunctions::isDigit (t[i]) || t[i] == '.'))
    {
        digits = digits || juce::CharacterFunctions::isDigit (t[i]);
        ++i;
    }
    if (! digits)
        return std::nullopt;

    Reading r;
    r.value = t.substring (0, i).getDoubleValue();
    auto unit = t.substring (i).removeCharacters (" \t");

    // A side, as pans show it: "30% L", "30 R", "30 left".
    for (auto [word, sign] :
         { std::pair { "left", -1.0 }, std::pair { "right", 1.0 }, std::pair { "l", -1.0 }, std::pair { "r", 1.0 } })
        if (const auto rest = unit.dropLastCharacters ((int)std::strlen (word));
            unit.endsWith (word) && (rest.isEmpty() || rest == "%"))
        {
            r.value = sign * std::abs (r.value);
            unit = rest;
            break;
        }

    auto is = [&unit] (std::initializer_list<const char*> names)
    {
        for (auto* name : names)
            if (unit == juce::String (juce::CharPointer_UTF8 (name)))
                return true;
        return false;
    };
    if (unit.isEmpty())
        r.kind = UnitKind::none;
    else if (is ({ "%" }))
        r.kind = UnitKind::percent;
    else if (is ({ "db" }))
        r.kind = UnitKind::decibels;
    else if (is ({ "hz" }))
        r.kind = UnitKind::frequency;
    else if (is ({ "khz" }))
        r = { r.value * 1000.0, UnitKind::frequency };
    else if (is ({ "ms", "msec", "millisecond", "milliseconds" }))
        r.kind = UnitKind::time;
    else if (is ({ "s", "sec", "secs", "second", "seconds" }))
        r = { r.value * 1000.0, UnitKind::time };
    else if (is ({ "st", "semi", "semitone", "semitones" }))
        r.kind = UnitKind::semitones;
    else if (is ({ "ct", "cent", "cents" }))
        r.kind = UnitKind::cents;
    else if (is ({ "\xc2\xb0", "deg", "degree", "degrees" }))
        r.kind = UnitKind::degrees;
    else if (is ({ ":1" }))
        r.kind = UnitKind::ratio;
    else
        r.kind = UnitKind::unknown;
    return r;
}

/** False when the typed text has a unit of another kind than the shown text ("50%" for "-6.0 dB"). A plain
    number, or a unit or a shown text Spinus doesn't know, agrees. */
inline bool unitsAgree (const juce::String& typed, const juce::String& shown)
{
    const auto a = read (typed), b = read (shown);
    if (! a || ! b)
        return true;
    auto known = [] (UnitKind k) { return k != UnitKind::none && k != UnitKind::unknown; };
    return ! known (a->kind) || ! known (b->kind) || a->kind == b->kind;
}

/** The value (in the setting's own range) for text, as `show` writes the setting's values: the number shown
    per unit stored comes from the range's end farthest from zero ("100%" for 1, "1000 ms" for 1 second).
    Words some settings show at an end ("Robot", "Centre") read too. */
inline std::optional<float> valueForText (const juce::String& text, const std::function<juce::String (float)>& show,
                                          const juce::NormalisableRange<float>& range)
{
    const auto typed = read (text);
    if (! typed)
    {
        const auto word = text.trim();
        for (auto v : { range.start, range.end, 0.0f })
            if (v >= range.start && v <= range.end && show (v).trim().equalsIgnoreCase (word))
                return v;
        if ((word.equalsIgnoreCase ("centre") || word.equalsIgnoreCase ("center") || word.equalsIgnoreCase ("c")) &&
            range.start <= 0.0f && range.end >= 0.0f)
            return 0.0f;
        return std::nullopt;
    }
    double scale = 1.0;
    for (auto end : std::abs (range.end) >= std::abs (range.start) ? std::array { range.end, range.start }
                                                                   : std::array { range.start, range.end })
        if (! juce::exactlyEqual (end, 0.0f))
            if (const auto shown = read (show (end)); shown && ! juce::exactlyEqual (shown->value, 0.0))
            {
                scale = shown->value / (double)end;
                break;
            }
    return (float)(typed->value / scale);
}

/** A Spinus device's number setting: typed text reads by the rule above (in the units it shows). Every
    Spinus device makes its number settings with this. */
class FloatSetting : public juce::AudioParameterFloat
{
public:
    using juce::AudioParameterFloat::AudioParameterFloat;

    /** Text with no number or a unit of another kind leaves the setting as it is. */
    float getValueForText (const juce::String& text) const override;
};

/** The 0..1 value for text typed for any plugin's setting, by the rule above: a choice or switch by its name;
    a Spinus device's number setting as FloatSetting reads it; another plugin's by the plugin's own reading,
    once the unit agrees with what it shows. Nothing when the text cannot be read so. Message thread. */
inline std::optional<float> normalisedForText (const juce::AudioProcessorParameter& p, const juce::String& text)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return std::nullopt;
    const auto steps = p.getNumSteps();
    if ((p.isDiscrete() || p.isBoolean()) && steps >= 2 && steps <= 1000)
    {
        for (int k = 0; k < steps; ++k)
        {
            const auto v = (float)k / (float)(steps - 1);
            if (p.getText (v, 200).trim().equalsIgnoreCase (trimmed))
                return v;
        }
        if (p.isBoolean())
        {
            for (auto* on : { "on", "yes", "true", "1" })
                if (trimmed.equalsIgnoreCase (on))
                    return 1.0f;
            for (auto* off : { "off", "no", "false", "0" })
                if (trimmed.equalsIgnoreCase (off))
                    return 0.0f;
        }
        // Spinus's own choices and switches read by name only; another plugin's stepped setting may take a number.
        if (dynamic_cast<const juce::AudioParameterChoice*> (&p) != nullptr ||
            dynamic_cast<const juce::AudioParameterBool*> (&p) != nullptr || ! trimmed.containsAnyOf ("0123456789"))
            return std::nullopt;
    }

    if (! unitsAgree (trimmed, p.getText (p.getValue(), 200)))
        return std::nullopt;
    if (auto* setting = dynamic_cast<const FloatSetting*> (&p))
    {
        const auto& range = setting->getNormalisableRange();
        const auto value =
            valueForText (trimmed, [&p, &range] (float v) { return p.getText (range.convertTo0to1 (v), 200); }, range);
        if (! value)
            return std::nullopt;
        return juce::jlimit (0.0f, 1.0f, range.convertTo0to1 (range.snapToLegalValue (*value)));
    }
    if (! trimmed.containsAnyOf ("0123456789"))
        return std::nullopt;
    return juce::jlimit (0.0f, 1.0f, p.getValueForText (trimmed));
}

inline float FloatSetting::getValueForText (const juce::String& text) const
{
    const juce::AudioProcessorParameter& self = *this; // AudioParameterFloat's own are private
    return normalisedForText (self, text).value_or (self.getValue());
}

} // namespace spinus::dsp::param
