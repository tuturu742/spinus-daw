#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

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
    double asTyped = 0.0; // the number as written ("2.5" for "2.5 s")
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
    const auto asTyped = r.value;

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
    r.asTyped = asTyped;
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

/** What a setting shows at a value (0 to 1), with its unit: a plugin that gives the unit apart (a VST3's
    label, "dB" beside "-6.0") has it added. Message thread. */
inline juce::String shownText (const juce::AudioProcessorParameter& p, float value)
{
    auto text = p.getText (value, 200).trim();
    const auto label = p.getLabel().trim();
    if (label.isNotEmpty() && text.isNotEmpty() && ! text.endsWithIgnoreCase (label))
        text << " " << label;
    return text;
}

/** Another plugin's setting read by its own text: the 0..1 value at which it shows what was typed. The plugin's
    own reading (getValueForText) when what it then shows is as close as can be; else the value is found from
    what it shows at points over its whole range (numbers, in the units the rule above reads, or a word it
    shows), narrowed between the two points the number lies between. Out of its range: the closest end.
    Nothing when it shows no number (or not that word). Message thread. */
inline std::optional<float> byPluginText (const juce::AudioProcessorParameter& p, const juce::String& text)
{
    constexpr int points = 200;
    const auto typed = read (text);
    if (! typed)
    {
        // A word ("Hall", "Off"): the middle of the first run of values that show it.
        std::optional<float> first, last;
        for (int k = 0; k <= points; ++k)
        {
            const auto v = (float)k / (float)points;
            if (p.getText (v, 200).trim().equalsIgnoreCase (text.trim()))
            {
                if (! first)
                    first = v;
                last = v;
            }
            else if (first)
                break;
        }
        if (first && last)
            return (*first + *last) / 2.0f;
        const auto own = juce::jlimit (0.0f, 1.0f, p.getValueForText (text));
        if (p.getText (own, 200).trim().equalsIgnoreCase (text))
            return own;
        return std::nullopt;
    }

    auto at = [&p] (float v) -> std::optional<double>
    {
        if (const auto shown = read (shownText (p, v)))
            return shown->value;
        return std::nullopt;
    };
    std::vector<std::pair<float, double>> shown; // value, the number it shows
    bool bare = true;                            // it shows numbers without a unit
    for (int k = 0; k <= points; ++k)
        if (const auto v = (float)k / (float)points; const auto reading = read (shownText (p, v)))
        {
            shown.emplace_back (v, reading->value);
            bare = bare && (reading->kind == UnitKind::none || reading->kind == UnitKind::unknown);
        }
    // A plugin that shows bare numbers ("2.575" for a decay in seconds): "2.5 s" may mean 2.5 as it shows it
    // rather than 2500 ms. The number as written when only it is in the setting's range.
    auto target = typed->value;
    if (bare && ! shown.empty() && ! juce::exactlyEqual (typed->asTyped, typed->value))
    {
        const auto [low, high] = std::minmax_element (shown.begin(), shown.end(), [] (const auto& x, const auto& y)
                                                      { return x.second < y.second; });
        auto inRange = [&, l = low->second, h = high->second] (double x) { return x >= l && x <= h; };
        if (! inRange (target) && inRange (typed->asTyped))
            target = typed->asTyped;
    }
    auto distance = [&] (float v)
    {
        const auto number = at (v);
        return number ? std::abs (*number - target) : std::numeric_limits<double>::infinity();
    };

    std::optional<float> found;
    for (size_t i = 0; i + 1 < shown.size() && ! found; ++i)
    {
        const auto [a, va] = shown[i];
        const auto [b, vb] = shown[i + 1];
        if (target < std::min (va, vb) || target > std::max (va, vb))
            continue;
        // Between these two: narrowed by what it shows.
        const bool rising = vb >= va;
        auto low = a, high = b;
        for (int step = 0; step < 30; ++step)
        {
            const auto middle = (low + high) / 2.0f;
            const auto number = at (middle);
            if (! number)
                break;
            if ((*number < target) == rising)
                low = middle;
            else
                high = middle;
        }
        const auto closest = distance (low) <= distance (high) ? low : high;
        // The middle of the values that show the same (a shown "70%" covers 69.5% to 70.5%).
        const auto same = shownText (p, closest);
        auto edge = [&] (float from, float to)
        {
            if (shownText (p, to) == same)
                return to;
            for (int step = 0; step < 30; ++step)
            {
                const auto middle = (from + to) / 2.0f;
                if (shownText (p, middle) == same)
                    from = middle;
                else
                    to = middle;
            }
            return from;
        };
        found = (edge (closest, a) + edge (closest, b)) / 2.0f;
    }
    if (! found && ! shown.empty()) // out of its range: the closest point
        found = std::min_element (shown.begin(), shown.end(), [target] (const auto& x, const auto& y)
                                  { return std::abs (x.second - target) < std::abs (y.second - target); })
                    ->first;

    const auto own = juce::jlimit (0.0f, 1.0f, p.getValueForText (text));
    if (! found || distance (own) <= distance (*found))
        return std::isfinite (distance (own)) ? std::optional<float> (own) : found;
    return found;
}

/** What a setting shows at a few points, for a message: "-60.0 dB" at 0, "-30.0 dB" at 0.5, "0.0 dB" at 1. */
inline juce::String examplesOf (const juce::AudioProcessorParameter& p)
{
    juce::StringArray said;
    for (auto v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        said.add ("\"" + shownText (p, v) + "\" at " + juce::String (v));
    return said.joinIntoString (", ");
}

/** The 0..1 value for text typed for any plugin's setting, by the rule above: a choice or switch by its name;
    a Spinus device's number setting as FloatSetting reads it; another plugin's by its own text (byPluginText),
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

    if (! unitsAgree (trimmed, shownText (p, p.getValue())))
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
    if (dynamic_cast<const juce::RangedAudioParameter*> (&p) != nullptr) // a Spinus device's other settings
    {
        if (! trimmed.containsAnyOf ("0123456789"))
            return std::nullopt;
        return juce::jlimit (0.0f, 1.0f, p.getValueForText (trimmed));
    }
    return byPluginText (p, trimmed);
}

/** A choice's or switch's values by name, in order ("Bell", "Low cut"...); empty for a number setting or one
    with too many steps to list. Message thread. */
inline juce::StringArray choicesOf (const juce::AudioProcessorParameter& p)
{
    juce::StringArray names;
    const auto steps = p.getNumSteps();
    if ((p.isDiscrete() || p.isBoolean()) && steps >= 2 && steps <= 64)
        for (int k = 0; k < steps; ++k)
            names.add (p.getText ((float)k / (float)(steps - 1), 200).trim());
    return names;
}

inline float FloatSetting::getValueForText (const juce::String& text) const
{
    const juce::AudioProcessorParameter& self = *this; // AudioParameterFloat's own are private
    return normalisedForText (self, text).value_or (self.getValue());
}

} // namespace spinus::dsp::param
