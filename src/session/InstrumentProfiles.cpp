#include "InstrumentProfiles.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace spinus
{

namespace
{
const juce::StringArray jobNames { "unknown", "plays", "switch", "special", "unused" };
const juce::StringArray sourceNames { "user", "probing", "plugin", "manual", "bundled" };
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
            if (const auto* runs = m["keys"].getArray())
                for (const auto& r : *runs)
                    if (r.hasProperty ("from") && r.hasProperty ("to"))
                        mode.setKeys ((int)r["from"], (int)r["to"],
                                      { fromName (jobNames, r["job"], Job::unknown), r["what"].toString(),
                                        fromName (sourceNames, r["source"], Source::user) });
            profile.modes.push_back (std::move (mode));
        }
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
    return std::nullopt;
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
