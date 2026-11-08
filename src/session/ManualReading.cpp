#include "ManualReading.h"

#include "KeyProbe.h"

namespace spinus::manualreading
{

namespace
{
using Job = InstrumentProfiles::Job;
using Mode = InstrumentProfiles::Mode;

Job jobOf (const juce::String& name)
{
    if (name == "plays")
        return Job::plays;
    if (name == "switches")
        return Job::switches;
    if (name == "special")
        return Job::special;
    if (name == "unused")
        return Job::unused;
    return Job::unknown;
}

InstrumentProfiles::Voices voicesOf (const juce::String& name)
{
    return name == "chords" ? InstrumentProfiles::Voices::chords
           : name == "one"  ? InstrumentProfiles::Voices::one
                            : InstrumentProfiles::Voices::unknown;
}

Mode moved (const Mode& mode, int semitones)
{
    auto result = mode;
    result.keys = {};
    for (int n = 0; n < 128; ++n)
        if (const auto to = n + semitones; to >= 0 && to < 128)
            result.keys[(size_t)to] = mode.keys[(size_t)n];
    if (mode.enterKey >= 0)
        result.enterKey =
            mode.enterKey + semitones >= 0 && mode.enterKey + semitones < 128 ? mode.enterKey + semitones : -1;
    return result;
}
} // namespace

std::optional<Reading> fromVar (const juce::var& v)
{
    Reading reading;
    reading.middleCSaid = v.hasProperty ("middleCSaid") ? (bool)v["middleCSaid"] : true;
    reading.notes = v["notes"].toString();
    reading.by = v["by"].toString();
    if (const auto* read = v["read"].getArray())
        for (const auto& r : *read)
            if (r.hasProperty ("problem"))
                reading.problems.add (r["source"].toString().fromLastOccurrenceOf ("/", false, false) + ": " +
                                      r["problem"].toString());
    if (const auto* modes = v["modes"].getArray())
        for (const auto& m : *modes)
        {
            Mode mode;
            if (const auto name = m["name"].toString().trim(); name.isNotEmpty())
                mode.name = name;
            mode.enter = m["enter"].toString();
            mode.enterKey = m.hasProperty ("enterKey") ? juce::jlimit (-1, 127, (int)m["enterKey"]) : -1;
            mode.goodFor = m["goodFor"].toString();
            mode.voices = voicesOf (m["voices"].toString());
            if (const auto* zones = m["zones"].getArray())
                for (const auto& z : *zones)
                    if (const auto job = jobOf (z["job"].toString()); job != Job::unknown)
                        mode.setKeys (juce::jlimit (0, 127, (int)z["from"]), juce::jlimit (0, 127, (int)z["to"]),
                                      { job, z["what"].toString(), InstrumentProfiles::Source::manual });
            reading.modes.push_back (std::move (mode));
        }
    if (reading.modes.empty())
        return std::nullopt;
    return reading;
}

int matchingMode (const InstrumentProfiles::Profile& profile, const Mode& proposed)
{
    for (size_t i = 0; i < profile.modes.size(); ++i)
        if (profile.modes[i].name.equalsIgnoreCase (proposed.name))
            return (int)i;
    // The mode the instrument opens in (entered by nothing) is the profile's first, whatever it is called.
    if (! profile.modes.empty() && proposed.enterKey < 0 && proposed.enter.isEmpty())
    {
        const auto& first = profile.modes.front();
        if (first.enterKey < 0 && first.enter.isEmpty())
            return 0;
    }
    return -1;
}

int fitOctaves (Reading& reading, const InstrumentProfiles::Profile& profile)
{
    reading.octavesMoved = 0;
    if (reading.middleCSaid)
        return 0;
    // How well the reading fits what is known: a key it says plays that is known to play counts for it, one known
    // to switch or do nothing counts against it (and the other way round for its switch keys).
    const auto fit = [&] (int semitones)
    {
        int score = 0;
        for (const auto& proposed : reading.modes)
        {
            const auto i = matchingMode (profile, proposed);
            if (i < 0)
                continue;
            const auto& known = profile.modes[(size_t)i].keys;
            for (int n = 0; n < 128; ++n)
            {
                const auto job = proposed.keys[(size_t)n].job;
                const auto to = n + semitones;
                if (job == Job::unknown || to < 0 || to >= 128 || known[(size_t)to].job == Job::unknown ||
                    known[(size_t)to].source == InstrumentProfiles::Source::manual)
                    continue;
                const auto knownJob = known[(size_t)to].job;
                if (job == Job::plays)
                    score += knownJob == Job::plays ? 1 : -1;
                else if (job == Job::switches || job == Job::special)
                    score += knownJob == Job::plays ? -1 : 1;
            }
        }
        return score;
    };
    int best = 0, bestScore = fit (0);
    for (const auto octaves : { -1, 1, -2, 2 })
        if (const auto score = fit (12 * octaves); score > bestScore)
        {
            best = octaves;
            bestScore = score;
        }
    if (best != 0)
        for (auto& mode : reading.modes)
            mode = moved (mode, 12 * best);
    reading.octavesMoved = best;
    return best;
}

int apply (InstrumentProfiles::Profile& profile, const Reading& reading)
{
    int changed = 0;
    for (const auto& proposed : reading.modes)
    {
        const auto i = matchingMode (profile, proposed);
        if (i < 0)
        {
            profile.modes.push_back (proposed);
            ++changed;
            continue;
        }
        auto& mode = profile.modes[(size_t)i];
        if (i == 0 && mode.name == Mode {}.name && mode.name != proposed.name)
        {
            mode.name = proposed.name;
            ++changed;
        }
        const auto fill = [&changed] (auto& field, const auto& value, const auto& empty)
        {
            if (field == empty && value != empty)
            {
                field = value;
                ++changed;
            }
        };
        fill (mode.enter, proposed.enter, juce::String());
        fill (mode.enterKey, proposed.enterKey, -1);
        fill (mode.goodFor, proposed.goodFor, juce::String());
        fill (mode.voices, proposed.voices, InstrumentProfiles::Voices::unknown);
        changed += keyprobe::merge (mode, proposed.keys);
    }
    return changed;
}

juce::String describe (const Reading& reading)
{
    juce::StringArray modes;
    for (const auto& mode : reading.modes)
        modes.add (mode.name + (mode.enterKey >= 0 ? " (entered by " + keyName (mode.enterKey) + ", MIDI " +
                                                         juce::String (mode.enterKey) + ")"
                                                   : juce::String()));
    return juce::String ((int)reading.modes.size()) + (reading.modes.size() == 1 ? " mode: " : " modes: ") +
           modes.joinIntoString (", ");
}

} // namespace spinus::manualreading
