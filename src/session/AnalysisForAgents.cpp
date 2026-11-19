#include "AnalysisForAgents.h"

namespace spinus::agents
{
namespace
{
juce::File settingsFile (const juce::File& aiFolder) { return aiFolder.getChildFile ("analyzer.json"); }

/** What was measured of a song, by name: what song_analysis can give. */
juce::StringArray measuredOf (const juce::var& entry)
{
    juce::StringArray measured;
    for (auto* name : { "rhythm", "key", "loudness", "features" })
        if (entry[name].isObject())
            measured.add (name);
    for (auto* kind : { "notes", "drums" })
        if (auto* parts = entry[kind].getDynamicObject())
            for (auto& part : parts->getProperties())
                measured.add (juce::String (kind) + ": " + part.name.toString());
    return measured;
}

juce::String withoutExtension (const juce::String& name)
{
    return name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name;
}

juce::var round2 (const juce::var& value) { return std::round ((double)value * 100.0) / 100.0; }
} // namespace

bool fullDataAllowed (const juce::File& aiFolder)
{
    if (aiFolder == juce::File())
        return false;
    return (bool)juce::JSON::parse (settingsFile (aiFolder))["fullDataThroughMcp"];
}

void setFullDataAllowed (const juce::File& aiFolder, bool allowed)
{
    if (aiFolder == juce::File())
        return;
    const auto file = settingsFile (aiFolder);
    auto settings = juce::JSON::parse (file);
    if (! settings.isObject())
        settings = juce::var (new juce::DynamicObject());
    settings.getDynamicObject()->setProperty ("fullDataThroughMcp", allowed);
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (settings));
}

juce::var listSongs (const AnalysisLibrary& library)
{
    juce::Array<juce::var> songs;
    for (auto& entry : library.list())
    {
        if (! entry.current)
            continue;
        auto* song = new juce::DynamicObject();
        song->setProperty ("song", entry.key);
        song->setProperty ("name", entry.name);
        juce::Array<juce::var> measured;
        for (auto& m : measuredOf (library.read (entry.key)))
            measured.add (m);
        song->setProperty ("measured", measured);
        song->setProperty ("analyzed", entry.modified.toISO8601 (true).substring (0, 10));
        songs.add (juce::var (song));
    }
    return songs;
}

juce::var describeSong (const AnalysisLibrary& library, const juce::String& wanted, bool fullData,
                        const juce::String& part, double from, double to, juce::String& error)
{
    error.clear();
    const auto entries = library.list();
    const auto query = wanted.trim();
    const AnalysisLibrary::Entry* found = nullptr;
    std::vector<const AnalysisLibrary::Entry*> partly;
    for (auto& e : entries)
    {
        if (! e.current)
            continue;
        if (e.key.equalsIgnoreCase (query) || e.name.equalsIgnoreCase (query) ||
            withoutExtension (e.name).equalsIgnoreCase (query))
        {
            found = &e;
            break;
        }
        if (query.isNotEmpty() && e.name.containsIgnoreCase (query))
            partly.push_back (&e);
    }
    if (found == nullptr && partly.size() == 1)
        found = partly.front();
    if (found == nullptr)
    {
        if (partly.size() > 1)
        {
            juce::StringArray names;
            for (auto* e : partly)
                names.add (e->name);
            error = "Several analyzed songs match \"" + query + "\": " + names.joinIntoString (", ") +
                    ". Name one (or give its song key from list_analyzed_songs).";
        }
        else
            error = "No analyzed song is called \"" + query + "\". list_analyzed_songs lists them.";
        return {};
    }

    const auto entry = library.read (found->key);
    auto* result = new juce::DynamicObject();
    juce::var answer (result);
    result->setProperty ("song", found->key);
    result->setProperty ("name", found->name);

    // The beat: tempo, metre and where the bars start (seconds), not every beat.
    if (const auto& rhythm = entry["rhythm"]; rhythm.isObject())
    {
        juce::Array<juce::var> bars;
        if (auto* downbeats = rhythm["downbeats"].getArray())
            for (auto& d : *downbeats)
                bars.add (round2 (d));
        result->setProperty ("rhythm", juce::var (new juce::DynamicObject()));
        auto* r = result->getProperty ("rhythm").getDynamicObject();
        r->setProperty ("tempo", rhythm["tempo"]);
        r->setProperty ("beatsPerBar", rhythm["beatsPerBar"]);
        r->setProperty ("bars", bars.size());
        r->setProperty ("barStarts", bars);
    }
    for (auto* name : { "key", "loudness", "features" })
        if (entry[name].isObject())
            result->setProperty (name, entry[name]);

    juce::Array<juce::var> parts;
    for (auto* kind : { "notes", "drums" })
        if (auto* kept = entry[kind].getDynamicObject())
            for (auto& p : kept->getProperties())
                parts.add (p.name.toString() + (juce::String (kind) == "drums" ? " (drum hits)" : ""));
    result->setProperty ("parts", parts);
    result->setProperty ("fullData", fullData);

    if (part.isEmpty())
    {
        if (! parts.isEmpty())
            result->setProperty (
                "exactNotes",
                fullData ? "Ask for one part's notes or drum hits with `part` (and `from`/`to` seconds for a stretch)."
                         : "The exact notes and drum hits are not shared: the user can allow them in the Analyzer "
                           "(Provide full data through MCP).");
        return answer;
    }
    if (! fullData)
    {
        error = "The exact notes and drum hits of analyzed songs are not shared. The user can allow it in the "
                "Analyzer: \"Provide full data through MCP\". Everything else song_analysis gives without `part`.";
        return {};
    }

    // The part: "bass" (any separation's), "htdemucs/bass", or "drums" (the drum hits, kept per separation model:
    // "drums" takes the first, "drums htdemucs" or "htdemucs" that one).
    const juce::var* events = nullptr;
    juce::String partName;
    bool hits = false;
    const auto ends = [&part] (const juce::String& name)
    { return name.equalsIgnoreCase (part) || name.fromLastOccurrenceOf ("/", false, false).equalsIgnoreCase (part); };
    if (auto* kept = entry["notes"].getDynamicObject())
        for (auto& p : kept->getProperties())
            if (events == nullptr && ends (p.name.toString()))
            {
                events = &p.value;
                partName = p.name.toString();
            }
    if (auto* kept = entry["drums"].getDynamicObject(); kept != nullptr && events == nullptr)
    {
        const auto model = part.removeCharacters (" ").replace ("drums", "", true).trimCharactersAtStart ("/:");
        for (auto& p : kept->getProperties())
            if (events == nullptr && (model.isEmpty() || p.name.toString().equalsIgnoreCase (model)))
            {
                events = &p.value;
                partName = "drums (" + p.name.toString() + ")";
                hits = true;
            }
    }
    if (events == nullptr || ! events->isArray())
    {
        juce::StringArray names;
        for (auto& name : parts)
            names.add (name.toString());
        error = "This song has no part \"" + part + "\". Its parts: " + names.joinIntoString (", ") + ".";
        return {};
    }
    const juce::Identifier timeField (hits ? "time" : "start");

    juce::Array<juce::var> chosen;
    int left = 0;
    double next = -1.0;
    for (auto& e : *events->getArray())
    {
        const auto time = (double)e[timeField];
        if ((from >= 0.0 && time < from) || (to >= 0.0 && time >= to))
            continue;
        if (chosen.size() < maxEvents)
            chosen.add (e);
        else if (left++ == 0)
            next = time;
    }
    result->setProperty ("part", partName);
    result->setProperty (hits ? "hits" : "notes", chosen);
    if (left > 0)
        result->setProperty ("more", juce::String (left) + " more from " + juce::String (next, 2) +
                                         " s: ask again with from = " + juce::String (next, 2));
    return answer;
}

} // namespace spinus::agents
