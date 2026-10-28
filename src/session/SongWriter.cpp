#include "SongWriter.h"

#include "PartSounds.h"
#include "dsp/drums/StarterPack.h"

#include <optional>
#include <set>

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

juce::String layerPatternName (const juce::String& song, const juce::var& section, const Layer& layer)
{
    return patternName (song, section) + " - " + layer.name;
}

juce::String fxPatternName (const juce::String& song, const Layer& layer) { return song + ": " + layer.name; }

/** A layer's notes in one section (or an effect's notes: section empty), as pattern.setNotes takes them. */
juce::Array<juce::var> layerNotesOf (const juce::var& song, const Layer& layer, const juce::String& section,
                                     const std::map<juce::String, int>& drums)
{
    const auto& found = section.isEmpty() ? song["fx"][juce::Identifier (layer.id)]["notes"]
                                          : song["layerParts"][juce::Identifier (layer.id)][juce::Identifier (section)];
    juce::Array<juce::var> notes;
    for (const auto& n : arrayOf (found))
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

bool isEffect (const Layer& layer) { return layer.part == "fx"; }

/** The drums a Drum Machine (or another) instrument for a new channel plays. */
std::map<juce::String, int> drumsOfSound (const Sound& sound)
{
    if (! sound.drums.empty())
        return sound.drums;
    model::Channel made;
    made.instrument = model::Device {};
    made.instrument->stockType = sound.instrument["stockType"].toString();
    return drumNotesFor (made);
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

bool isFull (const juce::var& song) { return song["layers"].isArray() && ! arrayOf (song["layers"]).isEmpty(); }

std::vector<Layer> layersOf (const juce::var& song)
{
    std::vector<Layer> layers;
    if (! isFull (song))
        return layers;
    for (const auto& l : arrayOf (song["layers"]))
    {
        Layer layer;
        layer.id = l["id"].toString();
        layer.name = l["name"].toString();
        layer.family = l["family"].toString();
        // "#rrggbb" as the AI writes it, as a colour command takes it (#AARRGGBB).
        if (const auto c = l["colour"].toString();
            c.length() == 7 && c.startsWith ("#") && c.substring (1).containsOnly ("0123456789abcdefABCDEF"))
            layer.colour = "#ff" + c.substring (1).toLowerCase();
        layer.part = l["part"].toString();
        layer.role = l["role"].toString();
        layer.soundRole = l["soundRole"].toString();
        layer.drums = (bool)l["drums"];
        layer.instance = (int)l["instance"];
        if (const auto* r = l["register"].getArray(); r != nullptr && r->size() == 3)
        {
            layer.low = (int)(*r)[0];
            layer.middle = (int)(*r)[1];
            layer.high = (int)(*r)[2];
        }
        if (layer.id.isNotEmpty())
            layers.push_back (layer);
    }
    return layers;
}

juce::String familyColour (const juce::String& family)
{
    static const std::map<juce::String, juce::String> colours {
        { "Drums", "#ffff3fa4" },  { "Bass", "#ff2fd5e8" },      { "Guitars", "#ffe8663a" }, { "Keys", "#ff57c27a" },
        { "Synths", "#ff6e59d9" }, { "Orchestra", "#ffd1a94f" }, { "Leads", "#ffff9a3c" },   { "FX", "#ffa0a8b8" }
    };
    const auto found = colours.find (family);
    return found != colours.end() ? found->second : juce::String ("#ff9b5cff");
}

juce::String sendEffectKind (const juce::String& name)
{
    const auto words = " " + name.toLowerCase().replaceCharacters ("-_.()", "     ") + " ";
    for (const auto* word : { "reverb", "hall", "room", "plate", "chamber" })
        if (words.contains (juce::String (" ") + word + " ") || words.contains (juce::String (word) + "verb"))
            return "reverb";
    for (const auto* word : { "delay", "echo" })
        if (words.contains (juce::String (" ") + word))
            return "delay";
    return {};
}

juce::String layerColour (const Layer& layer)
{
    return layer.colour.isNotEmpty() ? layer.colour : familyColour (layer.family);
}

Sound defaultSound (const Layer& layer)
{
    if (layer.drums)
        return defaultSound ("drums");
    // 3x Osc's closest presets for each role (it has no guitar: a saw or square lead stands in, a pluck for picked
    // and strummed guitars).
    static const std::map<juce::String, juce::String> presets { { "bass", "Pluck Bass" },
                                                                { "sub", "Sub Bass" },
                                                                { "chords", "Soft Keys" },
                                                                { "pad", "Warm Pad" },
                                                                { "arp", "Pluck" },
                                                                { "lead", "Saw Lead" },
                                                                { "counter", "Bell" },
                                                                { "riser", "Noise Riser" },
                                                                { "rhythm-guitar", "Square Lead" },
                                                                { "lead-guitar", "Bright Lead" },
                                                                { "acoustic-guitar", "Pluck" },
                                                                { "clean-guitar", "Pluck" },
                                                                { "piano", "Soft Keys" },
                                                                { "organ", "Organ" },
                                                                { "strings", "String Pad" },
                                                                { "choir", "Choir Pad" },
                                                                { "brass", "Synth Brass" },
                                                                { "woodwinds", "Flute Lead" } };
    // A second part of a role sounds unlike the first.
    static const std::map<juce::String, juce::String> second {
        { "bass", "Square Bass" }, { "chords", "Pluck" },        { "pad", "String Pad" },         { "arp", "Bell" },
        { "lead", "Flute Lead" },  { "counter", "Synth Brass" }, { "rhythm-guitar", "Saw Lead" }, { "piano", "Bell" }
    };
    const auto& table = layer.instance > 0 && second.contains (layer.role) ? second : presets;
    auto sound = defaultSound ("lead");
    const auto found = table.find (layer.role);
    sound.preset = found != table.end() ? found->second : juce::String ("Soft Keys");
    return sound;
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

std::map<juce::String, model::ChannelId> findLayerChannels (const model::Project& project,
                                                            const std::vector<Layer>& layers)
{
    std::map<juce::String, model::ChannelId> found;
    const auto legacy = findChannels (project);
    for (const auto& layer : layers)
    {
        if (layer.part == "fx")
            continue;
        for (const auto& channel : project.channels) // the newest of the name
            if (! channel.audioTrack && channel.name == layer.name + " (Track creator)")
                found[layer.id] = channel.id;
        if (! found.contains (layer.id) && layer.instance == 0 && layer.role == layer.part)
            if (const auto l = legacy.find (layer.part); l != legacy.end())
                found[layer.id] = l->second;
    }
    return found;
}

juce::String lastSongName (const model::Project& project)
{
    int newest = 0;
    for (const auto& p : project.patterns)
        if (p.name.startsWith ("Song ") && p.name.contains (": "))
        {
            const auto number = p.name.substring (5).upToFirstOccurrenceOf (":", false, false);
            if (number.isNotEmpty() && number.containsOnly ("0123456789"))
                newest = juce::jmax (newest, number.getIntValue());
        }
    return newest > 0 ? "Song " + juce::String (newest) : juce::String();
}

juce::Array<juce::var> removeSongSteps (const model::Project& project, const juce::String& name, model::Ticks& start)
{
    juce::Array<juce::var> steps;
    auto add = [&steps] (const char* command, const juce::var& parameters)
    { steps.add (object ({ { "command", command }, { "parameters", parameters } })); };
    start = -1;
    std::set<model::PatternId> patterns;
    for (const auto& p : project.patterns)
        if (p.name.startsWith (name + ": "))
            patterns.insert (p.id);
    if (patterns.empty())
        return steps;
    model::Ticks end = 0;
    for (const auto& clip : project.playlist.clips)
        if (const auto* pc = std::get_if<model::PatternClip> (&clip.content);
            pc != nullptr && patterns.contains (pc->pattern))
        {
            start = start < 0 ? clip.start : std::min (start, clip.start);
            end = std::max (end, clip.start + clip.length);
        }

    // The channels only its patterns play, and their inserts; the Sounds page's channels stay.
    std::set<model::ChannelId> channels;
    for (const auto& c : project.channels)
    {
        if (c.audioTrack || c.name.endsWith ("(Track creator)"))
            continue;
        bool inSong = false, elsewhere = false;
        for (const auto& p : project.patterns)
            if (p.notes.contains (c.id))
                (patterns.contains (p.id) ? inSong : elsewhere) = true;
        if (inSong && ! elsewhere)
            channels.insert (c.id);
    }
    std::set<model::InsertId> inserts;
    for (const auto& id : channels)
        if (const auto* c = project.findChannel (id); c != nullptr && c->targetInsert != model::masterInsertId)
            if (std::none_of (project.channels.begin(), project.channels.end(), [&] (const model::Channel& o)
                              { return o.targetInsert == c->targetInsert && ! channels.contains (o.id); }))
                inserts.insert (c->targetInsert);
    // Its buses: inserts only its inserts play into.
    std::set<model::InsertId> buses;
    for (const auto& bus : project.mixer.inserts)
    {
        bool fed = false, other = false;
        for (const auto& i : project.mixer.inserts)
            if (i.output == bus.id && i.id != bus.id)
                (inserts.contains (i.id) ? fed : other) = true;
        const bool played = std::any_of (project.channels.begin(), project.channels.end(),
                                         [&] (const model::Channel& c) { return c.targetInsert == bus.id; });
        if (fed && ! other && ! played && ! inserts.contains (bus.id))
            buses.insert (bus.id);
    }
    for (const auto& id : channels)
        add ("channel.remove", object ({ { "channel", (int)id.value } }));
    for (const auto& id : inserts)
        add ("insert.remove", object ({ { "insert", (int)id.value } }));
    for (const auto& id : buses)
        add ("insert.remove", object ({ { "insert", (int)id.value } }));
    for (const auto& id : patterns)
        add ("pattern.remove", object ({ { "pattern", (int)id.value } }));

    // Its Playlist tracks (the tracks grouped under a family's folder before the folder), and its markers.
    std::vector<model::LaneId> folders, lanes;
    for (const auto& lane : project.playlist.lanes)
        if (lane.name == "Track creator: " + name || lane.name.startsWith (name + ": "))
            folders.push_back (lane.id);
    for (const auto& lane : project.playlist.lanes)
        if (std::find (folders.begin(), folders.end(), lane.group) != folders.end())
            lanes.push_back (lane.id);
    for (const auto& id : lanes)
        add ("lane.remove", object ({ { "lane", (int)id.value } }));
    for (const auto& id : folders)
        add ("lane.remove", object ({ { "lane", (int)id.value } }));
    if (start >= 0)
        for (const auto& marker : project.playlist.markers)
            if (marker.at >= start && marker.at < end)
                add ("marker.remove", object ({ { "marker", (int)marker.id.value } }));
    return steps;
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

namespace
{
/** The device type of one of Spinus's own effects as the AI names it ("eq"), or empty. */
juce::String stockEffectType (const juce::String& name)
{
    static const std::map<juce::String, juce::String> types { { "eq", "spinus.eq" },
                                                              { "compressor", "spinus.compressor" },
                                                              { "gate", "spinus.gate" },
                                                              { "limiter", "spinus.limiter" },
                                                              { "filter", "spinus.filter" },
                                                              { "ducker", "spinus.ducker" },
                                                              { "stereo", "spinus.stereo-utility" } };
    const auto found = types.find (name);
    return found != types.end() ? found->second : juce::String();
}

/** The full arrangement's channels, patterns and Playlist tracks (see SongWriter.h), after the tempo and key
    steps already in `steps`. */
juce::var makeFullProposal (const juce::var& song, const model::Project& project, const Options& options,
                            juce::Array<juce::var> steps, juce::String* error)
{
    // The song it replaces goes first (its channels, patterns, tracks and markers), in the same undo step.
    model::Ticks replacedAt = -1;
    if (options.replace.isNotEmpty())
        steps.addArray (removeSongSteps (project, options.replace, replacedAt));
    auto step = [&steps] (const char* command, const juce::var& parameters)
    {
        steps.add (object ({ { "command", command }, { "parameters", parameters } }));
        return "$" + juce::String (steps.size());
    };
    const auto layers = layersOf (song);
    const auto barTicks = (juce::int64)song["barTicks"];
    const auto swing = juce::jlimit (0.0, 1.0, (double)song["patternSwing"]);
    const auto sections = sectionsOf (song);

    // A channel per layer, coloured and grouped by family, each on an insert of its own. A part's channel the
    // Sounds page added is written into (its drums channel by the three drum layers).
    std::map<juce::String, juce::var> channels, inserts; // layer id -> its channel and its insert
    std::map<juce::String, std::map<juce::String, int>> drums;
    std::map<juce::String, juce::Array<juce::var>> familyInserts;
    juce::StringArray families;
    // The parts the AI put on buses of its own (its Effects and mix tools) leave their family's.
    const auto& production = song["production"];
    std::set<juce::String> onOwnBus;
    if (auto* buses = production["buses"].getDynamicObject())
        for (const auto& bus : buses->getProperties())
            for (const auto& part : arrayOf (bus.value))
                onOwnBus.insert (part.toString());
    for (const auto& layer : layers)
    {
        families.addIfNotAlreadyThere (layer.family);
        const auto partChannel = isEffect (layer) ? options.channels.end() : options.channels.find (layer.id);
        const auto* existing =
            partChannel != options.channels.end() ? project.findChannel (partChannel->second) : nullptr;
        if (existing != nullptr)
        {
            channels[layer.id] = (int)existing->id.value;
            inserts[layer.id] = (int)existing->targetInsert.value;
            if (layer.drums)
                drums[layer.id] = ! options.drums.empty() ? options.drums : drumNotesFor (*existing);
            continue;
        }
        const auto found = options.layerSounds.find (layer.id);
        const auto sound = found != options.layerSounds.end() ? found->second : defaultSound (layer);
        const auto colour = layerColour (layer);
        const auto channel =
            step ("channel.add", object ({ { "name", layer.name }, { "instrument", sound.instrument } }));
        step ("channel.set",
              object ({ { "channel", channel + ".channel" }, { "group", layer.family }, { "colour", colour } }));
        auto insert = object ({ { "insert", channel + ".insert" }, { "name", layer.name }, { "colour", colour } });
        if (sound.volumeDb)
            insert.getDynamicObject()->setProperty ("volumeDb", juce::jlimit (-100.0, 12.0, *sound.volumeDb));
        step ("insert.set", insert);
        if (sound.preset.isNotEmpty())
            step ("device.loadPreset", object ({ { "device", channel + ".device" }, { "preset", sound.preset } }));
        channels[layer.id] = channel + ".channel";
        inserts[layer.id] = channel + ".insert";
        if (! onOwnBus.contains (layer.id))
            familyInserts[layer.family].add (channel + ".insert");
        if (layer.drums)
            drums[layer.id] = drumsOfSound (sound);
    }
    // A bus per family of two or more new channels, so a family's level is one fader.
    for (const auto& family : families)
        if (familyInserts[family].size() >= 2)
        {
            const auto bus =
                step ("insert.addBus", object ({ { "name", family }, { "inserts", familyInserts[family] } }));
            step ("insert.set", object ({ { "insert", bus + ".insert" }, { "colour", familyColour (family) } }));
        }

    // A pattern per layer and section it plays in; one per effect.
    std::map<juce::String, std::map<juce::String, juce::String>> patterns; // layer -> section -> reference
    auto addPattern =
        [&] (const juce::String& name, juce::int64 length, const Layer& layer, const juce::Array<juce::var>& notes)
    {
        const auto pattern = step ("pattern.add", object ({ { "name", name }, { "length", length } }));
        step ("pattern.set", object ({ { "pattern", pattern + ".pattern" }, { "colour", layerColour (layer) } }));
        if (swing > 0.0 && ! isEffect (layer))
            step ("pattern.setSwing", object ({ { "pattern", pattern + ".pattern" }, { "swing", swing } }));
        step ("pattern.setNotes",
              object ({ { "pattern", pattern + ".pattern" }, { "channel", channels[layer.id] }, { "notes", notes } }));
        step ("pattern.setLength", object ({ { "pattern", pattern + ".pattern" }, { "length", length } }));
        return pattern;
    };
    for (const auto& layer : layers)
    {
        if (isEffect (layer))
        {
            const auto notes = layerNotesOf (song, layer, {}, drums[layer.id]);
            const auto bars = std::max (1, (int)song["fx"][juce::Identifier (layer.id)]["bars"]);
            if (! notes.isEmpty())
                patterns[layer.id][{}] =
                    addPattern (fxPatternName (options.name, layer), bars * barTicks, layer, notes);
            continue;
        }
        for (const auto& section : arrayOf (song["sections"]))
        {
            const auto id = section["id"].toString();
            const auto notes = layerNotesOf (song, layer, id, drums[layer.id]);
            if (! notes.isEmpty())
                patterns[layer.id][id] =
                    addPattern (layerPatternName (options.name, section, layer),
                                std::max<juce::int64> (1, (juce::int64)section["bars"]) * barTicks, layer, notes);
        }
    }

    // The Playlist: after what the song has, a folder track per family with a track per layer under it.
    model::Ticks end = 0;
    for (const auto& clip : project.playlist.clips)
        end = std::max (end, clip.start + clip.length);
    // After what the song has, or where the song it replaces was.
    const auto offset = replacedAt >= 0 ? replacedAt / barTicks * barTicks : (end + barTicks - 1) / barTicks * barTicks;
    for (const auto& existing : project.playlist.lanes)
        if (existing.name == channelName ("drums") && ! existing.muted)
            step ("lane.set", object ({ { "lane", (int)existing.id.value }, { "muted", true } }));
    std::map<juce::String, juce::String> lanes;
    for (const auto& family : families)
    {
        const auto folder = step ("lane.add", object ({ { "name", options.name + ": " + family } }));
        step ("lane.set", object ({ { "lane", folder + ".lane" }, { "colour", familyColour (family) } }));
        for (const auto& layer : layers)
            if (layer.family == family && patterns.contains (layer.id))
            {
                const auto lane = step ("lane.add", object ({ { "name", layer.name } }));
                step ("lane.set", object ({ { "lane", lane + ".lane" },
                                            { "colour", familyColour (family) },
                                            { "group", folder + ".lane" } }));
                lanes[layer.id] = lane;
            }
    }
    for (const auto& entry : arrayOf (song["arrangement"]))
    {
        const auto id = entry["section"].toString();
        const auto found = sections.find (id);
        if (found == sections.end())
        {
            if (error != nullptr)
                *error = "The song's order names a section it does not have.";
            return {};
        }
        const auto at = offset + (juce::int64)entry["bar"] * barTicks;
        const auto length = (juce::int64)found->second["bars"] * barTicks;
        for (const auto& layer : layers)
            if (const auto p = patterns[layer.id].find (id); p != patterns[layer.id].end())
                step ("clip.addPattern", object ({ { "lane", lanes[layer.id] + ".lane" },
                                                   { "pattern", p->second + ".pattern" },
                                                   { "start", at },
                                                   { "length", length } }));
        step ("marker.add", object ({ { "at", at }, { "name", found->second["name"].toString() } }));
    }
    for (const auto& layer : layers)
        if (const auto p = patterns[layer.id].find ({}); isEffect (layer) && p != patterns[layer.id].end())
        {
            const auto bars = std::max (1, (int)song["fx"][juce::Identifier (layer.id)]["bars"]);
            for (const auto& bar : arrayOf (song["fx"][juce::Identifier (layer.id)]["at"]))
                step ("clip.addPattern", object ({ { "lane", lanes[layer.id] + ".lane" },
                                                   { "pattern", p->second + ".pattern" },
                                                   { "start", offset + (juce::int64)bar * barTicks },
                                                   { "length", bars * barTicks } }));
        }

    // The AI's mix (its Effects and mix tools): its buses, effects, the reverb and delay buses with their sends,
    // and ducking.
    if (production.isObject())
    {
        if (auto* buses = production["buses"].getDynamicObject())
            for (const auto& bus : buses->getProperties())
            {
                juce::Array<juce::var> members;
                for (const auto& part : arrayOf (bus.value))
                    if (inserts.contains (part.toString()))
                        members.add (inserts[part.toString()]);
                if (! members.isEmpty())
                    step ("insert.addBus", object ({ { "name", bus.name.toString() }, { "inserts", members } }));
            }
        auto settings = [&] (const juce::var& device, const juce::var& text)
        {
            if (auto* o = text.getDynamicObject(); o != nullptr && ! o->getProperties().isEmpty())
                if (! options.withoutEffectSettings)
                    step ("device.setParameters", object ({ { "device", device }, { "text", text } }));
        };
        auto userDevice = [&] (const juce::String& name) -> juce::var
        {
            const auto found = options.effectDevices.find (name);
            return found != options.effectDevices.end() ? found->second : juce::var();
        };
        for (const auto& e : arrayOf (production["effects"]))
        {
            const auto part = e["part"].toString();
            if (! inserts.contains (part))
                continue;
            juce::var device;
            if (const auto type = e["effect"].toString(); type == "reverb" || type == "delay")
                device = userDevice (e["plugin"].toString());
            else if (const auto stock = stockEffectType (type); stock.isNotEmpty())
                device = object ({ { "kind", "stock" }, { "stockType", stock } });
            if (! device.isObject())
                continue;
            const auto added =
                step ("insert.addEffect", object ({ { "insert", inserts[part] }, { "device", device } }));
            settings (added + ".device", e["settings"]);
        }
        std::map<juce::String, juce::var> returns; // "reverb", "delay" -> the bus's insert
        if (auto* chosen = production["returns"].getDynamicObject())
            for (const auto& r : chosen->getProperties())
                if (const auto device = userDevice (r.value.toString()); device.isObject())
                {
                    const auto bus =
                        step ("insert.add",
                              object ({ { "name", options.name + " " +
                                                      (r.name.toString() == "reverb" ? "Reverb" : "Delay") } }));
                    step ("insert.addEffect", object ({ { "insert", bus + ".insert" }, { "device", device } }));
                    returns[r.name.toString()] = bus + ".insert";
                }
        if (auto* sends = production["sends"].getDynamicObject())
            for (const auto& part : sends->getProperties())
                if (auto* each = part.value.getDynamicObject();
                    each != nullptr && inserts.contains (part.name.toString()))
                    for (const auto& send : each->getProperties())
                        if (returns.contains (send.name.toString()))
                            step ("insert.setSend",
                                  object ({ { "insert", inserts[part.name.toString()] },
                                            { "target", returns[send.name.toString()] },
                                            { "levelDb", juce::jlimit (-100.0, 12.0, (double)send.value) } }));
        if (auto* ducks = production["ducks"].getDynamicObject())
            for (const auto& duck : ducks->getProperties())
            {
                const auto part = duck.name.toString(), source = duck.value["source"].toString();
                if (! inserts.contains (part) || ! inserts.contains (source))
                    continue;
                const auto ducker = step (
                    "insert.addEffect",
                    object ({ { "insert", inserts[part] },
                              { "device", object ({ { "kind", "stock" }, { "stockType", "spinus.ducker" } }) } }));
                settings (ducker + ".device",
                          object ({ { "trigger", "Sidechain" },
                                    { "amount",
                                      juce::String (juce::jlimit (0.0, 100.0, (double)duck.value["amount"]), 0) + "%" },
                                    { "release", duck.value["release"].toString() } }));
                step ("insert.setSidechain", object ({ { "insert", inserts[part] }, { "source", inserts[source] } }));
            }
    }

    // The AI's automation clips and samples (its Transitions and automation tools), on tracks of their own.
    if (production.isObject())
    {
        std::map<juce::String, juce::var> filters; // layer id -> the filter added on its insert
        juce::var folder;
        for (const auto& entry : arrayOf (production["automation"]))
        {
            const auto id = entry["part"].toString();
            const auto points = arrayOf (entry["points"]);
            if (! inserts.contains (id) || points.size() < 2)
                continue;
            const auto what = entry["what"].toString();
            juce::var target;
            if (what == "volume" || what == "pan")
                target =
                    object ({ { "kind", what == "volume" ? "insertVolume" : "insertPan" }, { "insert", inserts[id] } });
            else if (what == "filter")
            {
                if (! filters.contains (id))
                {
                    const auto added = step (
                        "insert.addEffect",
                        object ({ { "insert", inserts[id] },
                                  { "device", object ({ { "kind", "stock" }, { "stockType", "spinus.filter" } }) } }));
                    filters[id] = added + ".device";
                    step ("device.setParameters",
                          object ({ { "device", filters[id] },
                                    { "text", object ({ { "type", entry["filter"].toString() == "high-pass"
                                                                      ? "High pass"
                                                                      : "Low pass" } }) } }));
                }
                target =
                    object ({ { "kind", "deviceParameter" }, { "device", filters[id] }, { "parameterId", "cutoff" } });
            }
            else
                continue;
            if (folder.isVoid())
            {
                folder = step ("lane.add", object ({ { "name", options.name + ": Automation" } })) + ".lane";
                step ("lane.set", object ({ { "lane", folder }, { "colour", familyColour ("FX") } }));
            }
            juce::String name = id;
            for (const auto& layer : layers)
                if (layer.id == id)
                    name = layer.name;
            const auto lane = step ("lane.add", object ({ { "name", name + " " + what } }));
            step ("lane.set", object ({ { "lane", lane + ".lane" }, { "group", folder } }));
            const auto first = (double)points.getFirst()["bar"], last = (double)points.getLast()["bar"];
            juce::Array<juce::var> curve;
            for (const auto& p : points)
                curve.add (object ({ { "time", juce::roundToInt (((double)p["bar"] - first) * (double)barTicks) },
                                     { "value", juce::jlimit (0.0, 1.0, (double)p["value"]) },
                                     { "curve", juce::jlimit (-1.0, 1.0, (double)p["curve"]) } }));
            step (
                "clip.addAutomation",
                object ({ { "lane", lane + ".lane" },
                          { "start", offset + juce::roundToInt (first * (double)barTicks) },
                          { "length", std::max<juce::int64> (1, juce::roundToInt ((last - first) * (double)barTicks)) },
                          { "target", target },
                          { "points", curve } }));
        }
        // Samples from the Spinus Starter Pack (an impact's crash and 808), through one channel.
        const auto pack = dsp::starter::findPack();
        juce::var sampleLane, sampleChannel;
        for (const auto& entry : arrayOf (production["samples"]))
        {
            const auto file = pack.getChildFile (entry["file"].toString());
            if (! file.existsAsFile() || ! file.isAChildOf (pack))
                continue;
            if (sampleLane.isVoid())
                sampleLane = step ("lane.add", object ({ { "name", options.name + ": Samples" } })) + ".lane";
            auto clip = object ({ { "lane", sampleLane },
                                  { "file", file.getFullPathName() },
                                  { "start", offset + juce::roundToInt ((double)entry["bar"] * (double)barTicks) },
                                  { "length", 2 * barTicks } });
            if (sampleChannel.isVoid())
                clip.getDynamicObject()->setProperty ("name", options.name + ": Samples");
            else
                clip.getDynamicObject()->setProperty ("channel", sampleChannel);
            const auto added = step ("clip.addAudio", clip);
            if (sampleChannel.isVoid())
                sampleChannel = added + ".channel";
        }
    }

    const auto tempo = juce::jlimit (20.0, 999.0, (double)song["tempo"]);
    return object (
        { { "summary", "a new song, " + juce::String ((int)song["bars"]) + " bars at " + juce::String (tempo, 0) +
                           " BPM in " + song["key"]["name"].toString() + ", " + juce::String ((int)layers.size()) +
                           " channels" +
                           (options.replace.isNotEmpty() ? ", replacing " + options.replace : juce::String()) },
          { "steps", steps } });
}
} // namespace

std::vector<OfflineInstrument::Note> layerExcerpt (const juce::var& song, const Layer& layer,
                                                   const std::map<juce::String, int>& drums, double seconds)
{
    const auto beatsPerBar = std::max (1, (int)song["beatsPerBar"]);
    const auto ticksPerBeat = std::max<juce::int64> (1, (juce::int64)song["barTicks"] / beatsPerBar);
    const auto secondsPerTick = 60.0 / (juce::jlimit (20.0, 999.0, (double)song["tempo"]) * (double)ticksPerBeat);
    juce::Array<juce::var> busiest;
    for (const auto& section : arrayOf (song["sections"]))
        if (const auto notes = layerNotesOf (song, layer, section["id"].toString(), drums);
            notes.size() > busiest.size())
            busiest = notes;
    std::vector<OfflineInstrument::Note> excerpt;
    for (const auto& n : busiest)
    {
        const auto start = (double)(juce::int64)n["start"] * secondsPerTick;
        if (start >= seconds)
            continue;
        excerpt.push_back ({ (int)n["pitch"], start, (double)(juce::int64)n["length"] * secondsPerTick,
                             (float)(double)n["velocity"] });
    }
    return excerpt;
}

double roleLevelDb (const juce::String& role)
{
    static const std::map<juce::String, double> levels {
        { "drums", 0.0 },
        { "bass", -1.0 },
        { "sub", -2.0 },
        { "rhythm-guitar", -3.0 },
        { "lead-guitar", -2.0 },
        { "clean-guitar", -4.0 },
        { "acoustic-guitar", -4.0 },
        { "lead", -2.0 },
        { "counter", -5.0 },
        { "chords", -5.0 },
        { "piano", -4.0 },
        { "organ", -6.0 },
        { "pad", -8.0 },
        { "strings", -6.0 },
        { "choir", -7.0 },
        { "brass", -4.0 },
        { "woodwinds", -5.0 },
        { "arp", -6.0 },
        { "percussion", -6.0 },
        { "riser", -8.0 },
        { "impact", -4.0 },
    };
    const auto found = levels.find (role);
    return found != levels.end() ? found->second : -5.0;
}

std::map<juce::String, double> startingFaders (const std::vector<Layer>& layers,
                                               const std::map<juce::String, double>& measured)
{
    std::map<juce::String, double> faders;
    for (const auto& layer : layers)
        if (const auto m = measured.find (layer.id); m != measured.end() && m->second > -90.0)
            faders[layer.id] = roleLevelDb (layer.role) - m->second;
    if (faders.empty())
        return faders;
    double loudest = -1000.0;
    for (const auto& [id, db] : faders)
        loudest = std::max (loudest, db);
    for (auto& [id, db] : faders)
        db = std::max (-30.0, std::round ((db - loudest) * 10.0) / 10.0);
    return faders;
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
    if (isFull (song))
        return makeFullProposal (song, project, options, steps, error);

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
    if (full)
    {
        if (layerPatterns.empty())
            return false;
        for (const auto& [layer, bySection] : layerPatterns)
            for (const auto& [section, id] : bySection)
                if (project.findPattern (id) == nullptr)
                    return false;
        for (const auto& [layer, id] : layerChannels)
            if (project.findChannel (id) == nullptr)
                return false;
        return true;
    }
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
    if (isFull (song))
    {
        // Each layer's patterns by name (the newest of a name), its channel from their notes, its clips on the
        // Playlist.
        applied.full = true;
        for (const auto& layer : layersOf (song))
        {
            auto& bySection = applied.layerPatterns[layer.id];
            auto remember = [&] (const juce::String& wanted, const juce::String& section)
            {
                for (const auto& p : project.patterns)
                    if (p.name == wanted)
                        bySection[section] = p.id;
            };
            if (isEffect (layer))
                remember (fxPatternName (name, layer), {});
            else
                for (const auto& section : arrayOf (song["sections"]))
                    remember (layerPatternName (name, section, layer), section["id"].toString());
            if (bySection.empty())
            {
                applied.layerPatterns.erase (layer.id);
                continue;
            }
            for (const auto& [section, id] : bySection)
                if (const auto* p = project.findPattern (id); p != nullptr && ! p->notes.empty())
                {
                    applied.layerChannels[layer.id] = p->notes.begin()->first;
                    break;
                }
            if (const auto c = applied.layerChannels.find (layer.id); c != applied.layerChannels.end() && layer.drums)
                if (const auto* channel = project.findChannel (c->second))
                    applied.layerDrums[layer.id] = drumNotesFor (*channel);
            for (const auto& clip : project.playlist.clips)
                if (const auto* pc = std::get_if<model::PatternClip> (&clip.content))
                    for (const auto& [section, id] : bySection)
                        if (pc->pattern == id)
                            applied.layerClips[layer.id][section].push_back (clip.id);
        }
        return applied;
    }
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

namespace
{
/** makeRedoProposal for a full arrangement: the layers made from the part (or every layer of the section). */
juce::var makeFullRedo (const juce::var& song, const Applied& applied, const juce::String& part,
                        const juce::String& section, juce::String* error)
{
    auto fail = [error] (const juce::String& why)
    {
        if (error != nullptr)
            *error = why;
        return juce::var();
    };
    juce::Array<juce::var> steps;
    auto add = [&steps] (const char* command, const juce::var& parameters)
    { steps.add (object ({ { "command", command }, { "parameters", parameters } })); };
    const auto barTicks = (juce::int64)song["barTicks"];
    juce::String sectionName;
    for (const auto& s : arrayOf (song["sections"]))
    {
        const auto id = s["id"].toString();
        if (section.isNotEmpty() && id != section)
            continue;
        sectionName = s["name"].toString();
        const auto length = std::max<juce::int64> (1, (juce::int64)s["bars"]) * barTicks;
        // A part of the list: the first drums, bass, chords or lead stands for that part (and what is made from
        // it); another part only for itself.
        juce::String group = part, only;
        for (const auto& layer : layersOf (song))
            if (layer.id == part)
            {
                group = layer.instance == 0 && layer.role == layer.part ? layer.part : juce::String();
                only = group.isEmpty() ? layer.id : juce::String();
            }
        for (const auto& layer : layersOf (song))
        {
            if (isEffect (layer) || (only.isNotEmpty() && layer.id != only) ||
                (only.isEmpty() && part.isNotEmpty() && layer.part != group))
                continue;
            const auto drums = applied.layerDrums.contains (layer.id) ? applied.layerDrums.at (layer.id)
                                                                      : std::map<juce::String, int>();
            const auto notes = layerNotesOf (song, layer, id, drums);
            const auto patterns = applied.layerPatterns.find (layer.id);
            const auto channel = applied.layerChannels.find (layer.id);
            std::optional<model::PatternId> pattern;
            if (patterns != applied.layerPatterns.end())
                if (const auto p = patterns->second.find (id); p != patterns->second.end())
                    pattern = p->second;
            if (! pattern || channel == applied.layerChannels.end())
            {
                if (notes.isEmpty())
                    continue;
                return fail ("The " + layer.name.toLowerCase() + " in the " + sectionName.toLowerCase() +
                             " has no pattern in the project: apply the whole song again.");
            }
            add ("pattern.setNotes", object ({ { "pattern", (int)pattern->value },
                                               { "channel", (int)channel->second.value },
                                               { "notes", notes } }));
            add ("pattern.setLength", object ({ { "pattern", (int)pattern->value }, { "length", length } }));
            if (const auto byLayer = applied.layerClips.find (layer.id); byLayer != applied.layerClips.end())
                if (const auto clips = byLayer->second.find (id); clips != byLayer->second.end())
                    for (const auto& clip : clips->second)
                        add ("clip.set", object ({ { "clip", (int)clip.value }, { "length", length } }));
        }
    }
    if (steps.isEmpty())
        return fail (section.isNotEmpty() ? "The song has no section \"" + section + "\"."
                                          : juce::String ("Nothing of the " + displayName (part).toLowerCase() +
                                                          " is in the project: apply the whole song again."));
    return object ({ { "summary", part.isNotEmpty() ? "new " + displayName (part).toLowerCase()
                                                    : "the " + sectionName.toLowerCase() + " written again" },
                     { "steps", steps } });
}
} // namespace

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
    if (applied.full)
        return makeFullRedo (song, applied, part, section, error);
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
