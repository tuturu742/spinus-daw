#include "SongWriter.h"

#include "PartSounds.h"

namespace spinus::song
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}

const juce::Array<juce::var>& arrayOf (const juce::var& v)
{
    static const juce::Array<juce::var> none;
    const auto* a = v.getArray();
    return a != nullptr ? *a : none;
}

/** The song's sections by id. */
std::map<juce::String, juce::var> sectionsOf (const juce::var& song)
{
    std::map<juce::String, juce::var> found;
    for (const auto& s : arrayOf (song["sections"]))
        found[s["id"].toString()] = s;
    return found;
}

bool isSong (const juce::var& song, juce::String* error)
{
    if (song["kind"].toString() == "spinus.song" && (int)song["version"] == 1 && song["sections"].isArray() &&
        song["arrangement"].isArray() && song["parts"].isObject() && (int)song["barTicks"] > 0)
        return true;
    if (error != nullptr)
        *error = "This is not a song the Track creator wrote (or one of another version).";
    return false;
}

/** A part's notes in one section, as pattern.setNotes takes them (drums on the channel's notes). */
juce::Array<juce::var> notesOf (const juce::var& song, const juce::String& part, const juce::String& section,
                                const std::map<juce::String, int>& drums)
{
    juce::Array<juce::var> notes;
    for (const auto& n : arrayOf (song["parts"][juce::Identifier (part)][juce::Identifier (section)]))
    {
        int pitch = 0;
        if (n.hasProperty ("drum"))
        {
            const auto drum = drums.find (n["drum"].toString());
            if (drum == drums.end())
                continue;
            pitch = drum->second;
        }
        else
            pitch = (int)n["pitch"];
        notes.add (object ({ { "start", std::max<juce::int64> (0, (juce::int64)n["start"]) },
                             { "length", std::max<juce::int64> (1, (juce::int64)n["length"]) },
                             { "pitch", juce::jlimit (0, 127, pitch) },
                             { "velocity", juce::jlimit (0.0, 1.0, (double)n["velocity"]) } }));
    }
    return notes;
}

juce::String patternName (const juce::String& song, const juce::var& section)
{
    return song + ": " + section["name"].toString();
}
} // namespace

const juce::StringArray& partNames()
{
    static const juce::StringArray names { "drums", "bass", "chords", "lead" };
    return names;
}

juce::String summaryPart (const juce::String& part)
{
    return part == "chords" ? juce::String ("other") : part == "lead" ? juce::String ("vocals") : part;
}

juce::String displayName (const juce::String& part) { return part.substring (0, 1).toUpperCase() + part.substring (1); }

juce::String channelName (const juce::String& part)
{
    return parts::displayName (summaryPart (part)).upToFirstOccurrenceOf (" (", false, false) + " (Track creator)";
}

std::map<juce::String, int> drumNotesFor (const model::Channel& channel)
{
    if (channel.instrument && channel.instrument->stockType == "spinus.drum-machine")
        return { { "kick", 60 },   { "snare", 61 },  { "hat", 63 },     { "openHat", 64 },
                 { "tomLow", 65 }, { "tomMid", 66 }, { "tomHigh", 67 }, { "crash", 70 } };
    return { { "kick", 36 },   { "snare", 38 },  { "hat", 42 },     { "openHat", 46 },
             { "tomLow", 45 }, { "tomMid", 47 }, { "tomHigh", 50 }, { "crash", 49 } };
}

std::map<juce::String, model::ChannelId> findChannels (const model::Project& project)
{
    std::map<juce::String, model::ChannelId> found;
    for (const auto& channel : project.channels)
        for (const auto& part : partNames())
            if (! channel.audioTrack && channel.name == channelName (part))
                found[part] = channel.id;
    return found;
}

Sound defaultSound (const juce::String& part)
{
    auto stock = [] (const char* type) { return object ({ { "kind", "stock" }, { "stockType", type } }); };
    if (part == "drums")
        return { stock ("spinus.drum-machine"), {}, {} };
    return { stock ("spinus.3x-osc"), part == "bass" ? "Pluck Bass" : part == "chords" ? "Soft Keys" : "Saw Lead", {} };
}

juce::String nextSongName (const model::Project& project)
{
    for (int n = 1;; ++n)
    {
        const auto name = "Song " + juce::String (n);
        if (std::none_of (project.patterns.begin(), project.patterns.end(),
                          [&] (const model::Pattern& p) { return p.name.startsWith (name + ": "); }))
            return name;
    }
}

juce::var makeProposal (const juce::var& song, const model::Project& project, const Options& options,
                        juce::String* error)
{
    if (! isSong (song, error))
        return {};
    juce::Array<juce::var> steps;
    auto step = [&steps] (const char* command, const juce::var& parameters)
    {
        steps.add (object ({ { "command", command }, { "parameters", parameters } }));
        return "$" + juce::String (steps.size());
    };

    const auto tempo = juce::jlimit (20.0, 999.0, (double)song["tempo"]);
    step ("project.setTempo", object ({ { "bpm", tempo } }));
    const auto& key = song["key"];
    step ("project.setKey", object ({ { "tonic", juce::jlimit (0, 11, (int)key["tonic"]) },
                                      { "scale", key["scale"].toString() == "minor" ? "minor" : "major" } }));
    const auto beats = juce::jlimit (1, 32, (int)song["beatsPerBar"]);
    if (project.settings.timeSignature.numerator != beats || project.settings.timeSignature.denominator != 4)
        step ("project.setTimeSignature", object ({ { "numerator", beats }, { "denominator", 4 } }));

    // A channel for each part: the one there is, or a new one.
    std::map<juce::String, juce::var> channels;
    std::map<juce::String, int> drums = options.drums;
    for (const auto& part : partNames())
    {
        const auto given = options.channels.find (part);
        const auto* existing = given != options.channels.end() ? project.findChannel (given->second) : nullptr;
        if (existing != nullptr)
        {
            channels[part] = (int)existing->id.value;
            if (part == "drums" && drums.empty())
                drums = drumNotesFor (*existing);
            continue;
        }
        const auto found = options.sounds.find (part);
        const auto sound = found != options.sounds.end() ? found->second : defaultSound (part);
        const auto channel =
            step ("channel.add", object ({ { "name", channelName (part) }, { "instrument", sound.instrument } }));
        step ("channel.set", object ({ { "channel", channel + ".channel" }, { "group", "Track creator" } }));
        if (sound.preset.isNotEmpty())
            step ("device.loadPreset", object ({ { "device", channel + ".device" }, { "preset", sound.preset } }));
        channels[part] = channel + ".channel";
        if (part == "drums" && drums.empty())
        {
            model::Channel made;
            made.instrument = model::Device {};
            made.instrument->stockType = sound.instrument["stockType"].toString();
            drums = ! sound.drums.empty() ? sound.drums : drumNotesFor (made);
        }
    }

    // One pattern per section, every part's notes in it.
    const auto barTicks = (juce::int64)song["barTicks"];
    const auto swing = juce::jlimit (0.0, 1.0, (double)song["patternSwing"]);
    std::map<juce::String, juce::String> patterns;
    for (const auto& section : arrayOf (song["sections"]))
    {
        const auto id = section["id"].toString();
        const auto length = std::max<juce::int64> (1, (juce::int64)section["bars"]) * barTicks;
        const auto pattern =
            step ("pattern.add", object ({ { "name", patternName (options.name, section) }, { "length", length } }));
        patterns[id] = pattern;
        if (swing > 0.0)
            step ("pattern.setSwing", object ({ { "pattern", pattern + ".pattern" }, { "swing", swing } }));
        for (const auto& part : partNames())
        {
            const auto notes = notesOf (song, part, id, drums);
            if (! notes.isEmpty())
                step ("pattern.setNotes",
                      object (
                          { { "pattern", pattern + ".pattern" }, { "channel", channels[part] }, { "notes", notes } }));
        }
        // Its notes may end before its last bar (the ending): it keeps the section's length.
        step ("pattern.setLength", object ({ { "pattern", pattern + ".pattern" }, { "length", length } }));
    }

    // The sections in order on a Playlist track of their own, after what the song has, with markers.
    model::Ticks end = 0;
    for (const auto& clip : project.playlist.clips)
        end = std::max (end, clip.start + clip.length);
    const auto offset = (end + barTicks - 1) / barTicks * barTicks;
    const auto sections = sectionsOf (song);
    // A drum loop the sounds for each part put on a track of its own: the song's drums play instead.
    for (const auto& existing : project.playlist.lanes)
        if (existing.name == channelName ("drums") && ! existing.muted)
            step ("lane.set", object ({ { "lane", (int)existing.id.value }, { "muted", true } }));
    const auto lane = step ("lane.add", object ({ { "name", "Track creator: " + options.name } }));
    for (const auto& entry : arrayOf (song["arrangement"]))
    {
        const auto id = entry["section"].toString();
        const auto found = sections.find (id);
        if (found == sections.end() || ! patterns.contains (id))
        {
            if (error != nullptr)
                *error = "The song's order names a section it does not have.";
            return {};
        }
        const auto at = offset + (juce::int64)entry["bar"] * barTicks;
        step ("clip.addPattern", object ({ { "lane", lane + ".lane" },
                                           { "pattern", patterns[id] + ".pattern" },
                                           { "start", at },
                                           { "length", (juce::int64)found->second["bars"] * barTicks } }));
        step ("marker.add", object ({ { "at", at }, { "name", found->second["name"].toString() } }));
    }

    const auto keyName = key["name"].toString();
    return object ({ { "summary", "a new song, " + juce::String ((int)song["bars"]) + " bars at " +
                                      juce::String (tempo, 0) + " BPM in " + keyName },
                     { "steps", steps } });
}

bool Applied::isIn (const model::Project& project) const
{
    if (patterns.empty())
        return false;
    for (const auto& [section, id] : patterns)
        if (project.findPattern (id) == nullptr)
            return false;
    for (const auto& [part, id] : channels)
        if (project.findChannel (id) == nullptr)
            return false;
    return true;
}

Applied findApplied (const juce::var& song, const model::Project& project, const juce::String& name)
{
    Applied applied;
    applied.name = name;
    for (const auto& section : arrayOf (song["sections"]))
    {
        const auto wanted = patternName (name, section);
        for (const auto& p : project.patterns) // the newest of that name
            if (p.name == wanted)
                applied.patterns[section["id"].toString()] = p.id;
    }
    // Its clips: on its Playlist track, playing its patterns.
    for (const auto& lane : project.playlist.lanes)
        if (lane.name == "Track creator: " + name)
            for (const auto& clip : project.playlist.clips)
                if (const auto* pc = std::get_if<model::PatternClip> (&clip.content);
                    pc != nullptr && clip.lane == lane.id)
                    for (const auto& [section, pattern] : applied.patterns)
                        if (pc->pattern == pattern)
                            applied.clips[section].push_back (clip.id);
    applied.channels = findChannels (project);
    if (const auto drums = applied.channels.find ("drums"); drums != applied.channels.end())
        if (const auto* channel = project.findChannel (drums->second))
            applied.drums = drumNotesFor (*channel);
    return applied;
}

juce::var makeRedoProposal (const juce::var& song, const Applied& applied, const juce::String& part,
                            const juce::String& section, juce::String* error)
{
    if (! isSong (song, error))
        return {};
    auto fail = [error] (const juce::String& why)
    {
        if (error != nullptr)
            *error = why;
        return juce::var();
    };
    if (part.isEmpty() == section.isEmpty())
        return fail ("Say which part or which section to write again.");
    if (part.isNotEmpty() && ! partNames().contains (part))
        return fail ("There is no part \"" + part + "\".");
    juce::Array<juce::var> steps;
    const auto barTicks = (juce::int64)song["barTicks"];
    juce::String sectionName;
    for (const auto& s : arrayOf (song["sections"]))
    {
        const auto id = s["id"].toString();
        if (section.isNotEmpty() && id != section)
            continue;
        sectionName = s["name"].toString();
        const auto pattern = applied.patterns.find (id);
        if (pattern == applied.patterns.end())
            return fail ("The song's " + sectionName + " is not in the project: apply the song first.");
        for (const auto& p : partNames())
        {
            if (part.isNotEmpty() && p != part)
                continue;
            const auto channel = applied.channels.find (p);
            if (channel == applied.channels.end())
                return fail ("The " + displayName (p).toLowerCase() + " channel is not in the project.");
            steps.add (object ({ { "command", "pattern.setNotes" },
                                 { "parameters", object ({ { "pattern", (int)pattern->second.value },
                                                           { "channel", (int)channel->second.value },
                                                           { "notes", notesOf (song, p, id, applied.drums) } }) } }));
        }
        // New notes fit the pattern to them (and the clips that play all of it): the section keeps its length.
        const auto length = std::max<juce::int64> (1, (juce::int64)s["bars"]) * barTicks;
        steps.add (object (
            { { "command", "pattern.setLength" },
              { "parameters", object ({ { "pattern", (int)pattern->second.value }, { "length", length } }) } }));
        if (const auto clips = applied.clips.find (id); clips != applied.clips.end())
            for (const auto& clip : clips->second)
                steps.add (
                    object ({ { "command", "clip.set" },
                              { "parameters", object ({ { "clip", (int)clip.value }, { "length", length } }) } }));
    }
    if (steps.isEmpty())
        return fail ("The song has no section \"" + section + "\".");
    return object ({ { "summary", part.isNotEmpty() ? "new " + displayName (part).toLowerCase()
                                                    : "the " + sectionName.toLowerCase() + " written again" },
                     { "steps", steps } });
}

juce::String describe (const juce::var& song)
{
    juce::StringArray lines;
    for (const auto& line : arrayOf (song["plain"]))
        lines.add (line.toString());
    return lines.joinIntoString ("\n");
}

} // namespace spinus::song
