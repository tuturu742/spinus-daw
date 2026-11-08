#include "PartModes.h"

#include <set>

namespace spinus::partmodes
{

namespace
{
using Mode = InstrumentProfiles::Mode;
using Job = InstrumentProfiles::Job;

/** The switch keys of a mode that change articulation (named, and not a key that enters a mode): what -> key. */
std::vector<std::pair<juce::String, int>> switchesOf (const Profile& profile, const Mode& mode)
{
    std::set<int> entering;
    for (const auto& m : profile.modes)
        if (m.enterKey >= 0)
            entering.insert (m.enterKey);
    std::vector<std::pair<juce::String, int>> found;
    for (int key = 0; key < 128; ++key)
    {
        const auto& k = mode.keys[(size_t)key];
        if (k.job != Job::switches || k.what.trim().isEmpty() || entering.contains (key))
            continue;
        const auto what = k.what.trim();
        if (std::none_of (found.begin(), found.end(), [&] (const auto& f) { return f.first.equalsIgnoreCase (what); }))
            found.emplace_back (what, key);
    }
    return found;
}

juce::StringArray wordsOf (const juce::String& text)
{
    static const juce::StringArray small { "and", "for", "the", "with", "parts", "part", "mode", "music" };
    juce::StringArray words;
    words.addTokens (text.toLowerCase().replaceCharacters ("-_/,.;:()", "         "), " ", "");
    words.removeEmptyStrings();
    juce::StringArray kept;
    for (const auto& w : words)
        if (w.length() >= 3 && ! small.contains (w))
            kept.addIfNotAlreadyThere (w);
    return kept;
}

const juce::Array<juce::var>& arrayOf (const juce::var& v)
{
    static const juce::Array<juce::var> none;
    const auto* a = v.getArray();
    return a != nullptr ? *a : none;
}

juce::String voicesName (InstrumentProfiles::Voices v)
{
    return v == InstrumentProfiles::Voices::chords ? "chords"
           : v == InstrumentProfiles::Voices::one  ? "one note at a time"
                                                   : "";
}
} // namespace

std::vector<int> usable (const Profile& profile)
{
    std::vector<int> found;
    for (size_t i = 0; i < profile.modes.size(); ++i)
    {
        const auto& m = profile.modes[i];
        if ((i == 0 || m.enterKey >= 0) && m.playable().has_value())
            found.push_back ((int)i);
    }
    return found;
}

juce::var brief (const Profile& profile)
{
    juce::Array<juce::var> list;
    for (const auto i : usable (profile))
    {
        const auto& m = profile.modes[(size_t)i];
        const auto keys = m.playable().value_or (PlayableKeys {});
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", m.name);
        if (m.goodFor.isNotEmpty())
            o->setProperty ("goodFor", m.goodFor);
        if (const auto v = voicesName (m.voices); v.isNotEmpty())
            o->setProperty ("voices", v);
        o->setProperty ("low", keys.lowest());
        o->setProperty ("high", keys.highest());
        int gaps = 0;
        for (int n = keys.lowest(); n <= keys.highest(); ++n)
            gaps += keys.plays (n) ? 0 : 1;
        if (gaps > 0)
            o->setProperty ("notPlaying", gaps); // keys between low and high that do something else
        juce::Array<juce::var> switches;
        for (const auto& [what, key] : switchesOf (profile, m))
        {
            auto* s = new juce::DynamicObject();
            s->setProperty ("what", what);
            s->setProperty ("key", key);
            switches.add (juce::var (s));
        }
        if (! switches.isEmpty())
            o->setProperty ("switches", switches);
        if (i == 0)
            o->setProperty ("first", true); // the mode the instrument opens in
        list.add (juce::var (o));
    }
    return list;
}

int pick (const Profile& profile, const juce::String& words)
{
    const auto modes = usable (profile);
    if (modes.empty())
        return -1;
    const auto said = wordsOf (words);
    int best = modes.front(), bestScore = 0;
    for (const auto i : modes)
    {
        int score = 0;
        for (const auto& w : wordsOf (profile.modes[(size_t)i].goodFor))
            score += said.contains (w) ? 1 : 0;
        if (score > bestScore)
        {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

Use useFor (const juce::var& song, const juce::String& part, const Profile& profile, const juce::String& words)
{
    Use use;
    const auto entry = song["modes"][juce::Identifier (part)];
    const auto named = entry["mode"].toString();
    for (const auto i : usable (profile))
        if (named.isNotEmpty() && profile.modes[(size_t)i].name.equalsIgnoreCase (named))
            use.mode = i;
    if (use.mode < 0)
    {
        use.mode = pick (profile, words);
        use.picked = true;
        return use;
    }
    const auto switches = switchesOf (profile, profile.modes[(size_t)use.mode]);
    if (const auto* given = entry["articulations"].getDynamicObject())
        for (const auto& [section, what] : given->getProperties())
            for (const auto& s : switches)
                if (s.first.equalsIgnoreCase (what.toString().trim()))
                    use.articulations[section.toString()] = s.first;
    return use;
}

juce::var apply (const juce::var& song, const std::map<juce::String, Instrument>& instruments)
{
    if (instruments.empty() || ! song.isObject())
        return song;
    auto copy = song.clone(); // deep: the notes are changed in place below
    juce::StringArray order;
    for (const auto& s : arrayOf (copy["sections"]))
        order.add (s["id"].toString());
    auto* modes = copy["modes"].getDynamicObject();
    if (modes == nullptr)
    {
        modes = new juce::DynamicObject();
        copy.getDynamicObject()->setProperty ("modes", juce::var (modes));
    }

    for (const auto& [id, instrument] : instruments)
    {
        const auto use = useFor (copy, id, instrument.profile, instrument.words);
        if (use.mode < 0)
            continue;
        const auto& mode = instrument.profile.modes[(size_t)use.mode];
        const auto keys = mode.playable().value_or (PlayableKeys {});
        std::map<juce::String, int> switchKey;
        for (const auto& [what, key] : switchesOf (instrument.profile, mode))
            switchKey[what] = key;
        // The articulation of each section: named, else the one before it, else the first named.
        std::map<juce::String, juce::String> articulation;
        juce::String first, last;
        for (const auto& s : order)
            if (const auto a = use.articulations.find (s); a != use.articulations.end() && first.isEmpty())
                first = a->second;
        for (const auto& s : order)
        {
            if (const auto a = use.articulations.find (s); a != use.articulations.end())
                last = a->second;
            if (const auto chosen = last.isNotEmpty() ? last : first; chosen.isNotEmpty())
                articulation[s] = chosen;
        }

        auto write = [&] (const juce::var& sections)
        {
            auto* bySection = sections.getDynamicObject();
            if (bySection == nullptr)
                return;
            for (const auto& [section, notes] : bySection->getProperties())
            {
                auto* list = notes.getArray();
                if (list == nullptr)
                    continue;
                list->removeIf ([] (const juce::var& n) { return (bool)n["switch"]; });
                if (list->isEmpty())
                    continue; // the part rests here
                std::vector<int> switches;
                if (use.mode != 0 && mode.enterKey >= 0)
                    switches.push_back (mode.enterKey);
                if (const auto a = articulation.find (section.toString()); a != articulation.end())
                    switches.push_back (switchKey[a->second]);
                for (auto& n : *list)
                    if (auto* o = n.getDynamicObject(); o != nullptr && ! o->hasProperty ("drum"))
                    {
                        o->setProperty ("pitch", keys.fit ((int)o->getProperty ("pitch")));
                        if (const auto start = (juce::int64)o->getProperty ("start");
                            ! switches.empty() && start < switchLead)
                        {
                            o->setProperty ("start", switchLead);
                            o->setProperty ("length", std::max<juce::int64> (1, (juce::int64)o->getProperty ("length") -
                                                                                    (switchLead - start)));
                        }
                    }
                for (size_t i = 0; i < switches.size(); ++i)
                {
                    auto* s = new juce::DynamicObject();
                    s->setProperty ("start", 0);
                    s->setProperty ("length", switchTicks);
                    s->setProperty ("pitch", switches[i]);
                    s->setProperty ("velocity", 0.8);
                    s->setProperty ("switch", true);
                    list->insert ((int)i, juce::var (s));
                }
            }
        };
        write (copy["layerParts"][juce::Identifier (id)]);
        write (copy["parts"][juce::Identifier (id)]);

        auto* entry = new juce::DynamicObject();
        entry->setProperty ("mode", mode.name);
        auto* named = new juce::DynamicObject();
        for (const auto& [section, what] : use.articulations)
            named->setProperty (section, what);
        entry->setProperty ("articulations", juce::var (named));
        if (use.picked)
            entry->setProperty ("picked", true);
        modes->setProperty (id, juce::var (entry));
    }
    return copy;
}

juce::StringArray describe (const juce::var& song, const std::map<juce::String, Instrument>& instruments,
                            const std::map<juce::String, juce::String>& names)
{
    juce::StringArray lines;
    for (const auto& [id, instrument] : instruments)
    {
        const auto use = useFor (song, id, instrument.profile, instrument.words);
        if (use.mode < 0 || (instrument.profile.modes.size() < 2 && use.articulations.empty()))
            continue; // nothing to say of an instrument with one mode and no switches used
        const auto name = names.contains (id) ? names.at (id) : id;
        juce::StringArray used;
        for (const auto& s : arrayOf (song["sections"]))
            if (const auto a = use.articulations.find (s["id"].toString()); a != use.articulations.end())
                used.addIfNotAlreadyThere (a->second);
        auto line = name + ": " + instrument.profile.modes[(size_t)use.mode].name + " mode";
        if (! used.isEmpty())
            line << "; " << used.joinIntoString (", ");
        lines.add (line);
    }
    return lines;
}

} // namespace spinus::partmodes
