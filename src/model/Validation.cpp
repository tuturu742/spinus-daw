#include "Validation.h"

#include <cmath>
#include <set>

namespace spinus::model
{

namespace
{

/** Inserts `insert` feeds directly: its output, its send targets and the inserts it is a sidechain
    source for. */
std::vector<InsertId> downstreamOf (const Project& project, const Insert& insert)
{
    std::vector<InsertId> result;

    if (insert.output)
        result.push_back (insert.output);

    for (auto& send : insert.sends)
        result.push_back (send.target);

    auto addSidechained = [&] (const Insert& other)
    {
        if (std::find (other.sidechainSources.begin(), other.sidechainSources.end(), insert.id) !=
            other.sidechainSources.end())
            result.push_back (other.id);
    };
    addSidechained (project.mixer.master);
    for (auto& other : project.mixer.inserts)
        addSidechained (other);

    return result;
}

bool reaches (const Project& project, InsertId start, InsertId goal)
{
    std::vector<InsertId> stack { start };
    std::set<InsertId> seen;

    while (! stack.empty())
    {
        const auto current = stack.back();
        stack.pop_back();

        if (current == goal)
            return true;

        if (! seen.insert (current).second)
            continue;

        if (auto* insert = project.findInsert (current))
            for (auto next : downstreamOf (project, *insert))
                stack.push_back (next);
    }

    return false;
}

juce::String describe (InsertId id) { return "insert " + juce::String (id.value); }

juce::String deviceName (const Device& device)
{
    const auto name = device.kind == Device::Kind::plugin ? device.plugin.name : device.stockType;
    return name.isNotEmpty() ? "'" + name + "'" : juce::String ("a device");
}

template <typename Item, typename IdType> int indexOf (const std::vector<Item>& items, IdType id)
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id)
            return (int)i;
    return -1;
}

} // namespace

bool wouldCreateRoutingCycle (const Project& project, InsertId from, InsertId to)
{
    return from == to || reaches (project, to, from);
}

juce::StringArray validate (const Project& project)
{
    juce::StringArray problems;
    std::set<uint32_t> ids;
    std::set<DeviceId> devices;

    auto checkId = [&] (uint32_t id, const juce::String& what)
    {
        if (id == 0)
            problems.add (what + " has no ID");
        else if (! ids.insert (id).second)
            problems.add (what + " reuses ID " + juce::String (id));
        else if (id >= project.nextId)
            problems.add (what + " has ID " + juce::String (id) + ", not below nextId " +
                          juce::String (project.nextId));
    };

    auto checkDevice = [&] (const Device& device, const juce::String& where)
    {
        checkId (device.id.value, "device in " + where);
        devices.insert (device.id);

        if (device.kind == Device::Kind::stock && device.stockType.isEmpty())
            problems.add ("stock device in " + where + " has no type");
        for (auto& [parameter, value] : device.parameters)
            if (parameter.isEmpty() || ! (value >= 0.0f && value <= 1.0f))
                problems.add ("device in " + where + " has a setting outside 0 to 1, or without a name");

        if (device.kind == Device::Kind::plugin && device.plugin.format.isEmpty())
            problems.add ("plugin in " + where + " has no format");
    };

    // Mixer
    if (project.mixer.master.id != masterInsertId)
        problems.add ("the master insert must have ID " + juce::String (masterInsertId.value));

    if (project.mixer.master.output)
        problems.add ("the master insert cannot have an output");

    checkId (project.mixer.master.id.value, "master insert");

    for (auto& insert : project.mixer.inserts)
        checkId (insert.id.value, describe (insert.id));

    auto checkInsert = [&] (const Insert& insert)
    {
        const auto name = describe (insert.id);

        if (insert.id != masterInsertId && ! project.findInsert (insert.output))
            problems.add (name + " outputs to a missing insert");

        for (auto& send : insert.sends)
        {
            if (! project.findInsert (send.target))
                problems.add (name + " sends to a missing insert");
            else if (send.target == insert.id)
                problems.add (name + " sends to itself");
        }

        for (auto source : insert.sidechainSources)
            if (! project.findInsert (source))
                problems.add (name + " has a missing sidechain source");

        if (insert.notesFrom && (insert.id == masterInsertId || project.findChannel (insert.notesFrom) == nullptr))
            problems.add (name + " takes notes from a missing channel");

        for (auto& slot : insert.effects)
            checkDevice (slot.device, name);

        if (insert.id != masterInsertId && reaches (project, insert.output, insert.id))
            problems.add (name + " is part of a routing cycle");
    };

    checkInsert (project.mixer.master);
    for (auto& insert : project.mixer.inserts)
        checkInsert (insert);

    // Channels
    for (auto& channel : project.channels)
    {
        const auto name = "channel " + juce::String (channel.id.value);
        checkId (channel.id.value, name);

        if (! project.findInsert (channel.targetInsert))
            problems.add (name + " targets a missing insert");

        if (channel.instrument)
            checkDevice (*channel.instrument, name);
    }

    if (! project.settings.timeSignature.isValid())
        problems.add ("the song's time signature is invalid");

    // Patterns
    for (auto& pattern : project.patterns)
    {
        const auto name = "pattern " + juce::String (pattern.id.value);
        checkId (pattern.id.value, name);

        if (pattern.length <= 0)
            problems.add (name + " has no length");
        if (pattern.timeSignature && ! pattern.timeSignature->isValid())
            problems.add (name + " has an invalid time signature");

        for (auto& [channelId, notes] : pattern.notes)
        {
            if (! project.findChannel (channelId))
                problems.add (name + " has notes for a missing channel");

            for (auto& note : notes)
                if (note.pitch < 0 || note.pitch > 127 || note.length <= 0 || note.start < 0 || note.velocity < 0.0f ||
                    note.velocity > 1.0f)
                {
                    problems.add (name + " has an invalid note");
                    break;
                }
        }
    }

    // Playlist
    for (auto& lane : project.playlist.lanes)
        checkId (lane.id.value, "lane " + juce::String (lane.id.value));

    for (auto& clip : project.playlist.clips)
    {
        const auto name = "clip " + juce::String (clip.id.value);
        checkId (clip.id.value, name);

        if (! project.findLane (clip.lane))
            problems.add (name + " is on a missing lane");

        if (clip.length <= 0 || clip.start < 0)
            problems.add (name + " has an invalid position or length");

        if (auto* p = std::get_if<PatternClip> (&clip.content); p != nullptr && ! project.findPattern (p->pattern))
            problems.add (name + " plays a missing pattern");

        if (auto* a = std::get_if<AudioClip> (&clip.content))
        {
            if (a->file.isEmpty())
                problems.add (name + " has no audio file");
            if (a->channel.isValid() && project.findChannel (a->channel) == nullptr)
                problems.add (name + " plays through a missing channel");
            if (a->offset < 0 || a->fadeIn < 0 || a->fadeOut < 0)
                problems.add (name + " has a negative offset or fade");
        }

        if (auto* automation = std::get_if<AutomationClip> (&clip.content))
        {
            const auto& t = automation->target;
            using K = ParameterTarget::Kind;
            const bool found = (t.kind == K::channelVolume || t.kind == K::channelPan)
                                   ? project.findChannel (t.channel) != nullptr
                               : (t.kind == K::insertVolume || t.kind == K::insertPan)
                                   ? project.findInsert (t.insert) != nullptr
                                   : devices.contains (t.device) && t.parameterId.isNotEmpty();
            if (! found)
                problems.add (name + " automates a missing target");

            for (size_t i = 0; i < automation->points.size(); ++i)
            {
                auto& point = automation->points[i];
                if (point.value < 0.0f || point.value > 1.0f || (i > 0 && point.time < automation->points[i - 1].time))
                {
                    problems.add (name + " has invalid automation points");
                    break;
                }
            }
        }
    }

    // Clip launcher
    const auto& launcher = project.launcher;
    for (auto& track : launcher.tracks)
        checkId (track.id.value, "launcher track " + juce::String (track.id.value));
    for (auto& scene : launcher.scenes)
        checkId (scene.id.value, "scene " + juce::String (scene.id.value));

    std::set<std::pair<uint32_t, uint32_t>> filled;
    for (auto& clip : launcher.clips)
    {
        const auto name = "launcher clip " + juce::String (clip.id.value);
        checkId (clip.id.value, name);

        if (indexOf (launcher.tracks, clip.track) < 0 || indexOf (launcher.scenes, clip.scene) < 0)
            problems.add (name + " is in a missing track or scene");
        else if (! filled.insert ({ clip.track.value, clip.scene.value }).second)
            problems.add (name + " shares its slot with another clip");

        if (clip.length <= 0)
            problems.add (name + " has no length");

        if (auto* p = std::get_if<PatternClip> (&clip.content); p != nullptr && ! project.findPattern (p->pattern))
            problems.add (name + " plays a missing pattern");

        if (auto* a = std::get_if<AudioClip> (&clip.content))
        {
            if (a->file.isEmpty())
                problems.add (name + " has no audio file");
            if (a->channel.isValid() && project.findChannel (a->channel) == nullptr)
                problems.add (name + " plays through a missing channel");
            if (a->offset < 0 || a->fadeIn < 0 || a->fadeOut < 0)
                problems.add (name + " has a negative offset or fade");
        }
    }

    return problems;
}

juce::StringArray repair (Project& project)
{
    juce::StringArray notes;
    std::set<uint32_t> ids;
    auto claim = [&ids] (uint32_t id) { return id != 0 && ids.insert (id).second; };
    auto named = [] (const juce::String& kind, const juce::String& name)
    { return name.isNotEmpty() ? kind + " '" + name + "'" : "a " + kind; };

    // The master: always ID 1, no output.
    project.mixer.master.id = masterInsertId;
    project.mixer.master.output = {};
    ids.insert (masterInsertId.value);
    std::erase_if (project.mixer.inserts,
                   [&] (const Insert& insert)
                   {
                       if (claim (insert.id.value))
                           return false;
                       notes.add (named ("mixer insert", insert.name));
                       return true;
                   });

    // Devices: each with its own ID, a type, and settings from 0 to 1.
    auto deviceProblem = [&] (Device& device) -> bool
    {
        if (! claim (device.id.value) || (device.kind == Device::Kind::stock && device.stockType.isEmpty()) ||
            (device.kind == Device::Kind::plugin && device.plugin.format.isEmpty()))
            return true;
        bool reset = false;
        for (auto it = device.parameters.begin(); it != device.parameters.end();)
        {
            if (it->first.isEmpty() || ! std::isfinite (it->second))
            {
                it = device.parameters.erase (it);
                reset = true;
                continue;
            }
            if (it->second < 0.0f || it->second > 1.0f)
            {
                it->second = juce::jlimit (0.0f, 1.0f, it->second);
                reset = true;
            }
            ++it;
        }
        if (reset)
            notes.add ("some settings of " + deviceName (device) + " (set back into range)");
        return false;
    };
    auto repairEffects = [&] (Insert& insert, const juce::String& where)
    {
        std::erase_if (insert.effects,
                       [&] (EffectSlot& slot)
                       {
                           if (! deviceProblem (slot.device))
                               return false;
                           notes.add ("the effect " + deviceName (slot.device) + " on " + where);
                           return true;
                       });
    };
    repairEffects (project.mixer.master, "the Master");
    for (auto& insert : project.mixer.inserts)
        repairEffects (insert, insert.name.isNotEmpty() ? "insert '" + insert.name + "'" : juce::String ("an insert"));

    // Channels.
    std::erase_if (project.channels,
                   [&] (const Channel& channel)
                   {
                       if (claim (channel.id.value))
                           return false;
                       notes.add (named ("channel", channel.name));
                       return true;
                   });
    for (auto& channel : project.channels)
    {
        if (channel.instrument && deviceProblem (*channel.instrument))
        {
            notes.add ("the instrument of " + named ("channel", channel.name));
            channel.instrument.reset();
        }
        if (project.findInsert (channel.targetInsert) == nullptr)
            channel.targetInsert = masterInsertId; // silently: nothing is lost
    }

    // Routing: outputs, sends and sidechains to inserts that are there, without cycles.
    auto repairRouting = [&] (Insert& insert)
    {
        if (insert.id != masterInsertId &&
            (project.findInsert (insert.output) == nullptr || insert.output == insert.id))
            insert.output = masterInsertId;
        std::erase_if (insert.sends, [&] (const Send& send)
                       { return send.target == insert.id || project.findInsert (send.target) == nullptr; });
        std::erase_if (insert.sidechainSources,
                       [&] (InsertId source) { return project.findInsert (source) == nullptr; });
        if (insert.notesFrom && (insert.id == masterInsertId || project.findChannel (insert.notesFrom) == nullptr))
            insert.notesFrom = {};
    };
    repairRouting (project.mixer.master);
    for (auto& insert : project.mixer.inserts)
        repairRouting (insert);
    for (auto& insert : project.mixer.inserts)
        if (reaches (project, insert.output, insert.id))
        {
            insert.output = masterInsertId;
            if (reaches (project, insert.output, insert.id)) // through sends or sidechains: drop those too
            {
                insert.sends.clear();
                for (auto& other : project.mixer.inserts)
                    std::erase (other.sidechainSources, insert.id);
                std::erase (project.mixer.master.sidechainSources, insert.id);
            }
        }

    if (! project.settings.timeSignature.isValid())
    {
        project.settings.timeSignature = {};
        notes.add ("the song's time signature (set back to 4/4)");
    }

    // Patterns.
    std::erase_if (project.patterns,
                   [&] (const Pattern& pattern)
                   {
                       if (claim (pattern.id.value))
                           return false;
                       notes.add (named ("pattern", pattern.name));
                       return true;
                   });
    for (auto& pattern : project.patterns)
    {
        if (pattern.length <= 0)
            pattern.length = Pattern {}.length;
        if (pattern.timeSignature && ! pattern.timeSignature->isValid())
            pattern.timeSignature.reset();
        for (auto it = pattern.notes.begin(); it != pattern.notes.end();)
        {
            if (project.findChannel (it->first) == nullptr)
            {
                notes.add ("the notes of a missing channel in " + named ("pattern", pattern.name));
                it = pattern.notes.erase (it);
                continue;
            }
            const auto removed = std::erase_if (it->second,
                                                [] (const Note& note)
                                                {
                                                    return note.pitch < 0 || note.pitch > 127 || note.length <= 0 ||
                                                           note.start < 0 || ! (note.velocity >= 0.0f) ||
                                                           note.velocity > 1.0f;
                                                });
            if (removed > 0)
                notes.add (juce::String ((int)removed) + (removed == 1 ? " note" : " notes") + " in " +
                           named ("pattern", pattern.name));
            ++it;
        }
    }

    // Playlist.
    std::erase_if (project.playlist.lanes,
                   [&] (const Lane& lane)
                   {
                       if (claim (lane.id.value))
                           return false;
                       notes.add (named ("Playlist track", lane.name));
                       return true;
                   });

    std::set<DeviceId> devices;
    auto addDevice = [&devices] (const Device& d) { devices.insert (d.id); };
    for (auto& channel : project.channels)
        if (channel.instrument)
            addDevice (*channel.instrument);
    for (auto& slot : project.mixer.master.effects)
        addDevice (slot.device);
    for (auto& insert : project.mixer.inserts)
        for (auto& slot : insert.effects)
            addDevice (slot.device);

    /** Why a clip's content cannot be kept, or empty. Fixes what can be fixed. */
    auto contentProblem = [&] (auto& content) -> juce::String
    {
        if (auto* p = std::get_if<PatternClip> (&content); p != nullptr && project.findPattern (p->pattern) == nullptr)
            return "its pattern is missing";
        if (auto* a = std::get_if<AudioClip> (&content))
        {
            if (a->file.isEmpty())
                return "it has no audio file";
            if (a->channel.isValid() && project.findChannel (a->channel) == nullptr)
                return "its audio track is missing";
            a->offset = std::max<Ticks> (0, a->offset);
            a->fadeIn = std::max<Ticks> (0, a->fadeIn);
            a->fadeOut = std::max<Ticks> (0, a->fadeOut);
        }
        return {};
    };

    const auto bar = (Ticks)project.settings.timeSignature.barTicks();
    std::erase_if (project.playlist.clips,
                   [&] (Clip& clip)
                   {
                       juce::String why;
                       if (! claim (clip.id.value))
                           why = "it has no ID of its own";
                       else if (project.findLane (clip.lane) == nullptr)
                           why = "its track is missing";
                       else if (clip.length <= 0 || clip.start < 0)
                           why = "its position is invalid";
                       else if (auto* automation = std::get_if<AutomationClip> (&clip.content))
                       {
                           const auto& t = automation->target;
                           using K = ParameterTarget::Kind;
                           const bool found = (t.kind == K::channelVolume || t.kind == K::channelPan)
                                                  ? project.findChannel (t.channel) != nullptr
                                              : (t.kind == K::insertVolume || t.kind == K::insertPan)
                                                  ? project.findInsert (t.insert) != nullptr
                                                  : devices.contains (t.device) && t.parameterId.isNotEmpty();
                           if (! found)
                               why = "what it automates is missing";
                           auto& points = automation->points;
                           std::erase_if (points, [] (const AutomationPoint& p) { return ! std::isfinite (p.value); });
                           for (auto& point : points)
                               point.value = juce::jlimit (0.0f, 1.0f, point.value);
                           std::stable_sort (points.begin(), points.end(),
                                             [] (auto& a, auto& b) { return a.time < b.time; });
                       }
                       else
                           why = contentProblem (clip.content);
                       if (why.isEmpty())
                           return false;
                       const auto* lane = project.findLane (clip.lane);
                       notes.add ("a clip at bar " + juce::String (juce::jmax<Ticks> (0, clip.start) / bar + 1) +
                                  (lane != nullptr && lane->name.isNotEmpty() ? " on '" + lane->name + "'" : "") +
                                  " (" + why + ")");
                       return true;
                   });

    // Clip launcher.
    auto& launcher = project.launcher;
    std::erase_if (launcher.tracks, [&] (const LauncherTrack& t) { return ! claim (t.id.value); });
    std::erase_if (launcher.scenes, [&] (const Scene& s) { return ! claim (s.id.value); });
    std::set<std::pair<uint32_t, uint32_t>> filled;
    std::erase_if (launcher.clips,
                   [&] (LauncherClip& clip)
                   {
                       const bool keep = claim (clip.id.value) && indexOf (launcher.tracks, clip.track) >= 0 &&
                                         indexOf (launcher.scenes, clip.scene) >= 0 && clip.length > 0 &&
                                         contentProblem (clip.content).isEmpty() &&
                                         filled.insert ({ clip.track.value, clip.scene.value }).second;
                       if (! keep)
                           notes.add ("a clip in the Clip Launcher");
                       return ! keep;
                   });

    // New IDs after every one in use.
    const auto highest = ids.empty() ? 0u : *ids.rbegin();
    if (project.nextId <= highest)
        project.nextId = highest + 1;

    return notes;
}

} // namespace spinus::model
