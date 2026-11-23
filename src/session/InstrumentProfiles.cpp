#include "InstrumentProfiles.h"
#include "PartModes.h"
#include "SpinusProfiles.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace spinus
{

namespace
{
const juce::StringArray jobNames { "unknown", "plays", "switch", "special", "unused" };
const juce::StringArray sourceNames { "user", "probing", "plugin", "manual", "bundled", "imported" };
const juce::StringArray voicesNames { "unknown", "chords", "one" };

template <typename E> E fromName (const juce::StringArray& names, const juce::var& v, E otherwise)
{
    const auto i = names.indexOf (v.toString());
    return i < 0 ? otherwise : (E)i;
}
} // namespace

juce::String keyName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 5); // middle C is C5, as in the Piano roll
}

// --- PlayableKeys ------------------------------------------------------------------------------------------------

PlayableKeys PlayableKeys::between (int low, int high)
{
    PlayableKeys p;
    for (int n = juce::jmax (0, juce::jmin (low, high)); n <= juce::jmin (127, juce::jmax (low, high)); ++n)
        p.keys.set ((size_t)n);
    return p;
}

int PlayableKeys::lowest() const noexcept
{
    for (int n = 0; n < 128; ++n)
        if (plays (n))
            return n;
    return -1;
}

int PlayableKeys::highest() const noexcept
{
    for (int n = 127; n >= 0; --n)
        if (plays (n))
            return n;
    return -1;
}

int PlayableKeys::fit (int note) const noexcept
{
    if (! any() || plays (note))
        return note;
    // The fewest octaves that land on a playable key, up first when as far.
    for (int octaves = 1; octaves <= 11; ++octaves)
    {
        if (plays (note + 12 * octaves))
            return note + 12 * octaves;
        if (plays (note - 12 * octaves))
            return note - 12 * octaves;
    }
    // No octave of it plays: the nearest key that does, the lower when as near.
    for (int d = 1; d < 128; ++d)
    {
        if (plays (note - d))
            return note - d;
        if (plays (note + d))
            return note + d;
    }
    return note;
}

juce::String PlayableKeys::describe() const
{
    if (! any())
        return "no key";
    const auto low = lowest(), high = highest();
    int gaps = 0;
    for (int n = low; n <= high; ++n)
        gaps += plays (n) ? 0 : 1;
    return keyName (low) + " to " + keyName (high) +
           (gaps == 0   ? juce::String()
            : gaps == 1 ? juce::String (", 1 key between does not play")
                        : ", " + juce::String (gaps) + " keys between do not play");
}

// --- Modes -------------------------------------------------------------------------------------------------------

std::optional<PlayableKeys> InstrumentProfiles::Mode::playable() const
{
    PlayableKeys p;
    for (size_t n = 0; n < keys.size(); ++n)
        if (keys[n].job == Job::plays)
            p.keys.set (n);
    if (! p.any())
        return std::nullopt;
    return p;
}

int InstrumentProfiles::Mode::keyToSound (int pitch) const
{
    const auto key = keyFor (pitch);
    if (const auto playing = playable())
        return playing->fit (key);
    return juce::jlimit (0, 127, key);
}

void InstrumentProfiles::Mode::setKeys (int from, int to, const Key& key)
{
    for (int n = juce::jmax (0, juce::jmin (from, to)); n <= juce::jmin (127, juce::jmax (from, to)); ++n)
        keys[(size_t)n] = key;
}

// --- The file ----------------------------------------------------------------------------------------------------

InstrumentProfiles::InstrumentProfiles (const juce::File& indexFolder)
    : file (fileIn (indexFolder)), oldNotes (oldNotesFileIn (indexFolder))
{
}

juce::String InstrumentProfiles::jobName (Job job) { return jobNames[(int)job]; }
juce::String InstrumentProfiles::sourceName (Source source) { return sourceNames[(int)source]; }

juce::var InstrumentProfiles::describeForAgents (const Profile& profile)
{
    const auto hasJob = [] (const Mode& mode, Job job)
    { return std::any_of (mode.keys.begin(), mode.keys.end(), [job] (const Key& k) { return k.job == job; }); };
    if (profile.modes.empty())
        return {};
    if (const auto& only = profile.modes.front(); profile.modes.size() == 1)
        if (const auto keys = only.playable(); keys && keys->keys.all() && only.octaves == 0 &&
                                               ! hasJob (only, Job::switches) && ! hasJob (only, Job::special))
            return {};

    juce::Array<juce::var> modes;
    for (const auto& mode : profile.modes)
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("name", mode.name);
        if (mode.enter.isNotEmpty())
            m->setProperty ("enter", mode.enter);
        if (mode.enterKey >= 0)
            m->setProperty ("enterKey", mode.enterKey);
        if (mode.goodFor.isNotEmpty())
            m->setProperty ("goodFor", mode.goodFor);
        if (mode.voices != Voices::unknown)
            m->setProperty ("voices", voicesNames[(int)mode.voices]);
        if (const auto keys = mode.playable())
        {
            juce::Array<juce::var> runs;
            for (int n = 0; n < 128; ++n)
                if (keys->plays (n))
                {
                    int end = n;
                    while (keys->plays (end + 1))
                        ++end;
                    runs.add (juce::Array<juce::var> { n, end });
                    n = end;
                }
            m->setProperty ("plays", runs);
            m->setProperty ("playsInWords", keys->describe());
        }
        if (mode.octaves != 0)
        {
            m->setProperty ("soundsOctaves", mode.octaves);
            const auto apart = std::abs (mode.octaves) == 1 ? juce::String ("an octave")
                                                            : juce::String (std::abs (mode.octaves)) + " octaves";
            m->setProperty ("soundsInWords", "each key sounds " + apart + (mode.octaves < 0 ? " lower" : " higher") +
                                                 " than its number: for a note to sound as MIDI n, write key n " +
                                                 (mode.octaves < 0 ? "+ " : "- ") +
                                                 juce::String (12 * std::abs (mode.octaves)));
        }
        else if (partmodes::isChordMode (mode))
        {
            // It plays chords held on its chord keys, strummed by its pattern keys (as partmodes::apply writes them).
            const auto zone = partmodes::chordKeysOf (mode);
            m->setProperty ("chordKeys", juce::Array<juce::var> { zone.low, zone.high });
            juce::Array<juce::var> patterns;
            for (const auto& [what, key] : zone.patterns)
            {
                auto* p = new juce::DynamicObject();
                p->setProperty ("key", key);
                p->setProperty ("note", keyName (key));
                p->setProperty ("what", what);
                patterns.add (juce::var (p));
            }
            m->setProperty ("patterns", patterns);
            m->setProperty ("howToPlay",
                            "chords, not single notes: hold each chord on the chord keys " + keyName (zone.low) +
                                " to " + keyName (zone.high) + " (its root at or above " + keyName (zone.low) +
                                ", the other notes stacked above it, an octave lower when they would go past " +
                                keyName (zone.high) + ")" +
                                (zone.patterns.empty()
                                     ? juce::String()
                                     : ", and press a pattern key with each chord, from just after it starts to "
                                       "just before the next, to play it") +
                                "; enter the mode first");
        }
        for (const auto job : { Job::switches, Job::special })
        {
            juce::Array<juce::var> list;
            for (int n = 0; n < 128; ++n)
                if (const auto& key = mode.keys[(size_t)n]; key.job == job)
                {
                    auto* k = new juce::DynamicObject();
                    k->setProperty ("key", n);
                    k->setProperty ("note", keyName (n));
                    if (key.what.isNotEmpty())
                        k->setProperty ("what", key.what);
                    list.add (juce::var (k));
                }
            if (! list.isEmpty())
                m->setProperty (job == Job::switches ? "switches" : "special", list);
        }
        modes.add (juce::var (m));
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("modes", modes);
    return juce::var (o);
}

juce::var InstrumentProfiles::toVar (const Profile& profile)
{
    juce::Array<juce::var> modes;
    for (const auto& mode : profile.modes)
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("name", mode.name);
        if (mode.enter.isNotEmpty())
            m->setProperty ("enter", mode.enter);
        if (mode.enterKey >= 0)
            m->setProperty ("enterKey", mode.enterKey);
        if (mode.goodFor.isNotEmpty())
            m->setProperty ("goodFor", mode.goodFor);
        m->setProperty ("voices", voicesNames[(int)mode.voices]);
        if (mode.octaves != 0)
            m->setProperty ("octaves", mode.octaves);
        // Runs of keys alike, the unknown ones left out.
        juce::Array<juce::var> runs;
        for (int n = 0; n < 128;)
        {
            int end = n;
            while (end + 1 < 128 && mode.keys[(size_t)end + 1] == mode.keys[(size_t)n])
                ++end;
            if (const auto& key = mode.keys[(size_t)n]; key.job != Job::unknown)
            {
                auto* r = new juce::DynamicObject();
                r->setProperty ("from", n);
                r->setProperty ("to", end);
                r->setProperty ("job", jobName (key.job));
                if (key.what.isNotEmpty())
                    r->setProperty ("what", key.what);
                r->setProperty ("source", sourceName (key.source));
                runs.add (juce::var (r));
            }
            n = end + 1;
        }
        m->setProperty ("keys", runs);
        modes.add (juce::var (m));
    }
    auto* o = new juce::DynamicObject();
    o->setProperty ("modes", modes);
    if (! profile.manuals.empty())
    {
        juce::Array<juce::var> manuals;
        for (const auto& manual : profile.manuals)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("source", manual.source);
            if (manual.version.isNotEmpty())
                m->setProperty ("version", manual.version);
            manuals.add (juce::var (m));
        }
        o->setProperty ("manuals", manuals);
    }
    return juce::var (o);
}

std::optional<InstrumentProfiles::Profile> InstrumentProfiles::fromVar (const juce::var& v)
{
    Profile profile;
    if (const auto* modes = v["modes"].getArray())
        for (const auto& m : *modes)
        {
            if (! m.isObject())
                continue;
            Mode mode;
            if (const auto name = m["name"].toString().trim(); name.isNotEmpty())
                mode.name = name;
            mode.enter = m["enter"].toString();
            mode.enterKey = m.hasProperty ("enterKey") ? juce::jlimit (-1, 127, (int)m["enterKey"]) : -1;
            mode.goodFor = m["goodFor"].toString();
            mode.voices = fromName (voicesNames, m["voices"], Voices::unknown);
            mode.octaves = juce::jlimit (-3, 3, (int)m["octaves"]);
            if (const auto* runs = m["keys"].getArray())
                for (const auto& r : *runs)
                    if (r.hasProperty ("from") && r.hasProperty ("to"))
                        mode.setKeys ((int)r["from"], (int)r["to"],
                                      { fromName (jobNames, r["job"], Job::unknown), r["what"].toString(),
                                        fromName (sourceNames, r["source"], Source::user) });
            profile.modes.push_back (std::move (mode));
        }
    if (const auto* manuals = v["manuals"].getArray())
        for (const auto& m : *manuals)
            if (const auto source = m["source"].toString().trim(); source.isNotEmpty())
                profile.manuals.push_back ({ source, m["version"].toString() });
    if (profile.modes.empty())
        return std::nullopt;
    return profile;
}

void InstrumentProfiles::load()
{
    profiles.clear();
    const auto saved = juce::JSON::parse (file);
    if (auto* instruments = saved["profiles"].getDynamicObject())
        for (const auto& [instrument, profile] : instruments->getProperties())
            if (auto read = fromVar (profile))
                profiles[instrument.toString()] = std::move (*read);
    // Notes it plays, as set before profiles: the first mode's playing keys.
    const auto before = juce::JSON::parse (oldNotes);
    if (auto* ranges = before["ranges"].getDynamicObject())
        for (const auto& [instrument, range] : ranges->getProperties())
        {
            const auto low = juce::jlimit (0, 127, (int)range["low"]);
            const auto high = juce::jlimit (0, 127, (int)range["high"]);
            if (! range.hasProperty ("low") || ! range.hasProperty ("high") || low > high ||
                profiles.count (instrument.toString()) > 0)
                continue;
            Mode mode;
            mode.setKeys (low, high, { Job::plays, {}, Source::user });
            profiles[instrument.toString()] = Profile { { mode } };
        }
}

bool InstrumentProfiles::save() const
{
    auto* all = new juce::DynamicObject();
    for (const auto& [instrument, profile] : profiles)
        all->setProperty (instrument, toVar (profile));
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("profiles", juce::var (all));
    file.getParentDirectory().createDirectory();
    if (! file.replaceWithText (juce::JSON::toString (juce::var (root)) + "\n"))
        return false;
    oldNotes.deleteFile(); // read into the profiles, which are saved now
    return true;
}

std::optional<InstrumentProfiles::Profile> InstrumentProfiles::get (const juce::String& instrument) const
{
    if (const auto found = profiles.find (instrument); found != profiles.end())
        return found->second;
    return bundled (instrument);
}

std::optional<InstrumentProfiles::Profile> InstrumentProfiles::bundled (const juce::String& instrument)
{
    // Read once from the files built into Spinus (packaging/share/instrument-profiles).
    static const auto shipped = []
    {
        std::map<juce::String, Profile> all;
        for (int i = 0; i < SpinusProfiles::namedResourceListSize; ++i)
        {
            int size = 0;
            const auto* data = SpinusProfiles::getNamedResource (SpinusProfiles::namedResourceList[i], size);
            const auto file = juce::JSON::parse (juce::String::fromUTF8 (data, size));
            juce::String name, problem;
            auto profile = fromShared (file, name, problem);
            const auto key = file["for"].toString();
            jassert (profile.has_value() && key.isNotEmpty()); // a shipped file that doesn't read
            if (! profile.has_value() || key.isEmpty())
                continue;
            for (auto& mode : profile->modes)
                for (auto& k : mode.keys)
                    if (k.job != Job::unknown)
                        k.source = Source::bundled;
            all.emplace (key, std::move (*profile));
        }
        return all;
    }();
    if (const auto found = shipped.find (instrument); found != shipped.end())
        return found->second;
    return std::nullopt;
}

juce::var InstrumentProfiles::toShared (const Profile& profile, const juce::String& instrumentName)
{
    auto shared = profile;
    std::erase_if (shared.manuals, [] (const Manual& m) { return ! m.isWebPage(); });
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", "spinus.instrument-profile");
    o->setProperty ("version", 1);
    o->setProperty ("instrument", instrumentName);
    o->setProperty ("profile", toVar (shared));
    return juce::var (o);
}

std::optional<InstrumentProfiles::Profile>
InstrumentProfiles::fromShared (const juce::var& v, juce::String& instrumentName, juce::String& problem)
{
    if (v["kind"].toString() != "spinus.instrument-profile")
    {
        problem = "it is not an instrument profile Spinus exported.";
        return std::nullopt;
    }
    if ((int)v["version"] > 1)
    {
        problem = "it was exported by a newer Spinus.";
        return std::nullopt;
    }
    auto profile = fromVar (v["profile"]);
    if (! profile.has_value())
    {
        problem = "it holds no mode.";
        return std::nullopt;
    }
    instrumentName = v["instrument"].toString();
    for (auto& mode : profile->modes)
        for (auto& key : mode.keys)
            if (key.job != Job::unknown)
                key.source = Source::imported;
    return profile;
}

void InstrumentProfiles::set (const juce::String& instrument, std::optional<Profile> profile)
{
    if (profile.has_value() && ! profile->modes.empty())
        profiles[instrument] = std::move (*profile);
    else
        profiles.erase (instrument);
}

std::optional<PlayableKeys> InstrumentProfiles::playable (const juce::String& instrument) const
{
    if (const auto profile = get (instrument); profile.has_value() && ! profile->modes.empty())
        return profile->modes.front().playable();
    return std::nullopt;
}

} // namespace spinus
