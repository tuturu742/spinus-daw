#include "PartModes.h"

#include <set>

namespace spinus::partmodes
{

namespace
{
using Mode = InstrumentProfiles::Mode;
using Job = InstrumentProfiles::Job;

using Named = std::vector<std::pair<juce::String, int>>; // what -> key, in key order

/** The switch keys of a mode that change articulation (named, and not a key that enters a mode). */
Named switchesOf (const Profile& profile, const Mode& mode)
{
    std::set<int> entering;
    for (const auto& m : profile.modes)
        if (m.enterKey >= 0)
            entering.insert (m.enterKey);
    Named found;
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

bool saysAny (const juce::String& what, const juce::StringArray& words)
{
    juce::StringArray said;
    said.addTokens (what.toLowerCase().replaceCharacters ("-_/,.;:()0123456789", "                   "), " ", "");
    for (const auto& w : said)
        if (words.contains (w))
            return true;
    return false;
}

/** A chord key (generic words): it picks the chord the mode plays. */
bool isChordKey (const InstrumentProfiles::Key& k)
{
    return k.job == Job::special && saysAny (k.what, { "chord", "chords" }) &&
           ! saysAny (k.what, { "mode", "switch", "toggle" });
}

/** A pattern key (generic words): it plays the chord held, with a rhythm or strum of the instrument's. */
bool isPatternKey (const InstrumentProfiles::Key& k)
{
    return k.job == Job::special && ! isChordKey (k) &&
           saysAny (k.what, { "pattern", "patterns", "seq", "sequence", "rhythm", "strum", "groove", "riff" }) &&
           ! saysAny (k.what, { "time", "speed", "control", "mode", "switch", "toggle", "velocity" });
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

juce::var namedList (const Named& named)
{
    juce::Array<juce::var> list;
    for (const auto& [what, key] : named)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("what", what);
        s->setProperty ("key", key);
        list.add (juce::var (s));
    }
    return list;
}

/** What a part may name per section in a mode: its articulation switches, and a chord mode's patterns. */
Named namesOf (const Profile& profile, const Mode& mode)
{
    auto names = switchesOf (profile, mode);
    for (const auto& p : chordKeysOf (mode).patterns)
        names.push_back (p);
    return names;
}

/** The chord's notes on the chord keys: its root at or above the zone's lowest key, the other tones stacked above
    it, the whole moved down an octave while it reaches past the zone and that keeps it inside. */
std::vector<int> voicing (const juce::Array<juce::var>& tones, const ChordKeys& zone)
{
    std::vector<int> notes;
    for (const auto& t : tones)
    {
        const auto pc = ((int)t % 12 + 12) % 12;
        int n = notes.empty() ? zone.low + (pc - zone.low % 12 + 12) % 12 : notes.back() + 1;
        while (n % 12 != pc)
            ++n;
        notes.push_back (n);
    }
    while (! notes.empty() && notes.back() > zone.high && notes.front() - 12 >= zone.low)
        for (auto& n : notes)
            n -= 12;
    std::erase_if (notes, [&] (int n) { return n < zone.low || n > zone.high; });
    return notes;
}

juce::var note (juce::int64 start, juce::int64 length, int pitch, double velocity, bool switches)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("start", start);
    o->setProperty ("length", std::max<juce::int64> (1, length));
    o->setProperty ("pitch", pitch);
    o->setProperty ("velocity", velocity);
    if (switches)
        o->setProperty ("switch", true);
    else
        o->setProperty ("chordKey", true); // written from the song's chords (a chord mode)
    return juce::var (o);
}
} // namespace

ChordKeys chordKeysOf (const Mode& mode)
{
    ChordKeys found;
    for (int key = 0; key < 128; ++key)
    {
        const auto& k = mode.keys[(size_t)key];
        if (isChordKey (k))
        {
            found.low = found.low < 0 ? key : found.low;
            found.high = key;
        }
        else if (isPatternKey (k) && std::none_of (found.patterns.begin(), found.patterns.end(), [&] (const auto& p)
                                                   { return p.first.equalsIgnoreCase (k.what.trim()); }))
            found.patterns.emplace_back (k.what.trim(), key);
    }
    return found;
}

bool isChordMode (const Mode& mode)
{
    const auto zone = chordKeysOf (mode);
    return ! mode.playable().has_value() && zone.low >= 0 && zone.high - zone.low >= 11;
}

std::vector<int> usable (const Profile& profile)
{
    std::vector<int> found;
    for (size_t i = 0; i < profile.modes.size(); ++i)
    {
        const auto& m = profile.modes[i];
        if ((i == 0 || m.enterKey >= 0) && (m.playable().has_value() || isChordMode (m)))
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
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", m.name);
        if (m.goodFor.isNotEmpty())
            o->setProperty ("goodFor", m.goodFor);
        if (isChordMode (m))
        {
            // The instrument plays the chords itself: Spinus holds each chord on its chord keys.
            const auto zone = chordKeysOf (m);
            o->setProperty ("voices", "chords");
            o->setProperty ("chordKeys", true);
            o->setProperty ("low", zone.low);
            o->setProperty ("high", zone.high);
            if (! zone.patterns.empty())
                o->setProperty ("patterns", namedList (zone.patterns));
        }
        else
        {
            const auto keys = m.playable().value_or (PlayableKeys {});
            if (const auto v = voicesName (m.voices); v.isNotEmpty())
                o->setProperty ("voices", v);
            o->setProperty ("low", keys.lowest());
            o->setProperty ("high", keys.highest());
            int gaps = 0;
            for (int n = keys.lowest(); n <= keys.highest(); ++n)
                gaps += keys.plays (n) ? 0 : 1;
            if (gaps > 0)
                o->setProperty ("notPlaying", gaps); // keys between low and high that do something else
        }
        if (const auto switches = switchesOf (profile, m); ! switches.empty())
            o->setProperty ("switches", namedList (switches));
        if (i == 0)
            o->setProperty ("first", true); // the mode the instrument opens in
        list.add (juce::var (o));
    }
    return list;
}

int pick (const Profile& profile, const juce::String& words, bool chords)
{
    std::vector<int> modes;
    for (const auto i : usable (profile))
        if (chords || ! isChordMode (profile.modes[(size_t)i]))
            modes.push_back (i);
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

Use useFor (const juce::var& song, const juce::String& part, const Profile& profile, const juce::String& words,
            bool chords)
{
    Use use;
    const auto entry = song["modes"][juce::Identifier (part)];
    const auto named = entry["mode"].toString();
    for (const auto i : usable (profile))
        if (named.isNotEmpty() && profile.modes[(size_t)i].name.equalsIgnoreCase (named))
            use.mode = i;
    if (use.mode < 0)
    {
        use.mode = pick (profile, words, chords);
        use.picked = true;
        return use;
    }
    const auto names = namesOf (profile, profile.modes[(size_t)use.mode]);
    if (const auto* given = entry["articulations"].getDynamicObject())
        for (const auto& [section, what] : given->getProperties())
            for (const auto& s : names)
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
    std::map<juce::String, juce::var> sections;
    for (const auto& s : arrayOf (copy["sections"]))
    {
        order.add (s["id"].toString());
        sections[s["id"].toString()] = s;
    }
    const auto barTicks = std::max<juce::int64> (1, (juce::int64)copy["barTicks"]);
    auto* modes = copy["modes"].getDynamicObject();
    if (modes == nullptr)
    {
        modes = new juce::DynamicObject();
        copy.getDynamicObject()->setProperty ("modes", juce::var (modes));
    }

    for (const auto& [id, instrument] : instruments)
    {
        const auto use = useFor (copy, id, instrument.profile, instrument.words, instrument.chords);
        if (use.mode < 0)
            continue;
        const auto& mode = instrument.profile.modes[(size_t)use.mode];
        const auto chordMode = isChordMode (mode);
        const auto zone = chordKeysOf (mode);
        const auto keys = mode.playable().value_or (PlayableKeys {});
        std::map<juce::String, int> keyOf;
        for (const auto& [what, key] : namesOf (instrument.profile, mode))
            keyOf[what] = key;
        std::set<int> patternKeys;
        for (const auto& p : zone.patterns)
            patternKeys.insert (p.second);
        // What is named for each section: as named, else as the section before it, else the first named. A chord
        // mode's pattern goes on the same way; one never named is picked by the section's energy.
        auto carried = [&] (bool patterns)
        {
            std::map<juce::String, juce::String> chosen;
            juce::String first, last;
            const auto counts = [&] (const juce::String& what)
            { return patternKeys.contains (keyOf[what]) == patterns; };
            for (const auto& s : order)
                if (const auto a = use.articulations.find (s);
                    a != use.articulations.end() && first.isEmpty() && counts (a->second))
                    first = a->second;
            for (const auto& s : order)
            {
                if (const auto a = use.articulations.find (s); a != use.articulations.end() && counts (a->second))
                    last = a->second;
                if (const auto c = last.isNotEmpty() ? last : first; c.isNotEmpty())
                    chosen[s] = c;
                else if (patterns && ! zone.patterns.empty())
                {
                    const auto energy = juce::jlimit (0.0, 1.0, (double)sections[s]["energy"]);
                    chosen[s] =
                        zone.patterns[(size_t)juce::roundToInt (energy * (double)(zone.patterns.size() - 1))].first;
                }
            }
            return chosen;
        };
        const auto articulation = carried (false);
        const auto pattern = chordMode ? carried (true) : std::map<juce::String, juce::String> {};

        auto write = [&] (const juce::var& bySectionVar)
        {
            auto* bySection = bySectionVar.getDynamicObject();
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
                const auto sid = section.toString();
                std::vector<int> switches;
                if (use.mode != 0 && mode.enterKey >= 0)
                    switches.push_back (mode.enterKey);
                if (const auto a = articulation.find (sid); a != articulation.end())
                    switches.push_back (keyOf[a->second]);
                const auto lead = switches.empty() ? juce::int64 (0) : switchLead;
                if (chordMode)
                {
                    // The song's chords held on the chord keys, the section's pattern key pressed with each.
                    list->clear();
                    const auto& chords = arrayOf (sections[sid]["chords"]);
                    const auto bars = std::max<juce::int64> (1, (juce::int64)sections[sid]["bars"]);
                    const auto patternKey = pattern.contains (sid) ? keyOf[pattern.at (sid)] : -1;
                    for (int c = 0; c < chords.size(); ++c)
                    {
                        if (c > 0 && chords[c]["name"] == chords[c - 1]["name"])
                            continue; // the same chord goes on
                        int next = c + 1;
                        while (next < chords.size() && chords[next]["name"] == chords[c]["name"])
                            ++next;
                        const auto from = (juce::int64)chords[c]["bar"] * barTicks + (c == 0 ? lead : 0);
                        const auto to =
                            next < chords.size() ? (juce::int64)chords[next]["bar"] * barTicks : bars * barTicks;
                        if (to - from <= 2 * switchLead)
                            continue;
                        for (const auto n : voicing (arrayOf (chords[c]["tones"]), zone))
                            list->add (note (from, to - from, n, 0.8, false));
                        if (patternKey >= 0)
                            list->add (note (from + switchLead, to - from - switchLead - 1, patternKey, 0.8, false));
                    }
                }
                else
                    for (auto& n : *list)
                        if (auto* o = n.getDynamicObject(); o != nullptr && ! o->hasProperty ("drum"))
                        {
                            o->setProperty ("pitch", keys.fit ((int)o->getProperty ("pitch")));
                            if (const auto start = (juce::int64)o->getProperty ("start"); start < lead)
                            {
                                o->setProperty ("start", lead);
                                o->setProperty (
                                    "length",
                                    std::max<juce::int64> (1, (juce::int64)o->getProperty ("length") - (lead - start)));
                            }
                        }
                for (size_t i = 0; i < switches.size(); ++i)
                    list->insert ((int)i, note (0, switchTicks, switches[i], 0.8, true));
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
        if (use.picked || (bool)modes->getProperty (id)["picked"]) // picked once, by the words: still so
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
        const auto use = useFor (song, id, instrument.profile, instrument.words, instrument.chords);
        if (use.mode < 0 || (instrument.profile.modes.size() < 2 && use.articulations.empty()))
            continue; // nothing to say of an instrument with one mode and no switches used
        const auto name = names.contains (id) ? names.at (id) : id;
        juce::StringArray used;
        for (const auto& s : arrayOf (song["sections"]))
            if (const auto a = use.articulations.find (s["id"].toString()); a != use.articulations.end())
                used.addIfNotAlreadyThere (a->second);
        const auto& mode = instrument.profile.modes[(size_t)use.mode];
        auto line = name + ": " + mode.name + " mode";
        if (isChordMode (mode))
            line << " (it plays the song's chords)";
        if (! used.isEmpty())
            line << "; " << used.joinIntoString (", ");
        lines.add (line);
    }
    return lines;
}

} // namespace spinus::partmodes
