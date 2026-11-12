#include "Validation.h"

#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace spinus::model
{

namespace
{

/** One connection between inserts: the source's output, one of its sends, or the sidechain it feeds. */
struct Route
{
    enum class Kind
    {
        output,
        send,
        sidechain
    };
    Kind kind;
    InsertId from, to;
};

/** The connections leaving `insert`, always in the same order: its output, its sends in their order, then the
    inserts that follow it as a sidechain (the Master first, then the mixer's order). */
std::vector<Route> routesFrom (const Project& project, const Insert& insert)
{
    std::vector<Route> result;
    if (insert.output)
        result.push_back ({ Route::Kind::output, insert.id, insert.output });
    for (auto& send : insert.sends)
        result.push_back ({ Route::Kind::send, insert.id, send.target });
    auto addSidechained = [&] (const Insert& other)
    {
        if (std::find (other.sidechainSources.begin(), other.sidechainSources.end(), insert.id) !=
            other.sidechainSources.end())
            result.push_back ({ Route::Kind::sidechain, insert.id, other.id });
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
            for (const auto& route : routesFrom (project, *insert))
                stack.push_back (route.to);
    }

    return false;
}

/** The first routing loop found, as its connections in order (empty when there is none). The search goes
    through the inserts in the mixer's order (the Master first) and each insert's connections in routesFrom's
    order, so the same project always gives the same loop; the last connection is the one leading back to the
    insert the loop was entered from. */
std::vector<Route> findRoutingLoop (const Project& project)
{
    enum class State
    {
        unseen,
        open,
        done
    };
    std::map<InsertId, State> state;
    std::vector<Route> path;
    std::vector<Route> loop;

    std::function<bool (const Insert&)> visit = [&] (const Insert& insert)
    {
        state[insert.id] = State::open;
        for (const auto& route : routesFrom (project, insert))
        {
            const auto* next = project.findInsert (route.to);
            if (next == nullptr)
                continue;
            if (state[route.to] == State::open)
            {
                auto start =
                    std::find_if (path.begin(), path.end(), [&] (const Route& r) { return r.from == route.to; });
                loop.assign (start, path.end());
                loop.push_back (route);
                return true;
            }
            if (state[route.to] == State::unseen)
            {
                path.push_back (route);
                if (visit (*next))
                    return true;
                path.pop_back();
            }
        }
        state[insert.id] = State::done;
        return false;
    };

    if (visit (project.mixer.master))
        return loop;
    for (const auto& insert : project.mixer.inserts)
        if (state[insert.id] == State::unseen && visit (insert))
            return loop;
    return {};
}

void removeRoute (Project& project, const Route& route)
{
    if (route.kind == Route::Kind::send)
    {
        if (auto* insert = project.findInsert (route.from))
            std::erase_if (insert->sends, [&] (const Send& send) { return send.target == route.to; });
    }
    else if (route.kind == Route::Kind::sidechain)
    {
        if (auto* insert = project.findInsert (route.to))
            std::erase (insert->sidechainSources, route.from);
    }
}

/** Breaks every routing loop that goes through a send or a sidechain (loops of outputs alone must be broken
    before) and returns what it removed. The fewest sends and sidechains that break them all go; among as few,
    the first set in a fixed order: sends and sidechains leading back up the mixer (to an insert before their
    own) before those leading down, each group in the mixer's order. With more than 16 candidates, which takes
    too long to try, loops are broken one at a time instead, each losing its send or sidechain leading back to
    where the loop was entered. Either way, the same project always loses the same ones. */
std::vector<Route> removeLoopingRoutes (Project& project)
{
    std::vector<const Insert*> inserts { &project.mixer.master };
    for (auto& insert : project.mixer.inserts)
        inserts.push_back (&insert);
    std::map<InsertId, size_t> position;
    for (size_t i = 0; i < inserts.size(); ++i)
        position[inserts[i]->id] = i;

    std::vector<Route> routes;
    for (auto* insert : inserts)
        for (const auto& route : routesFrom (project, *insert))
            if (position.contains (route.to))
                routes.push_back (route);

    // The sends and sidechains on a loop: those whose target leads back to their source.
    std::vector<size_t> candidates;
    for (size_t i = 0; i < routes.size(); ++i)
        if (routes[i].kind != Route::Kind::output && reaches (project, routes[i].to, routes[i].from))
            candidates.push_back (i);
    std::stable_sort (candidates.begin(), candidates.end(),
                      [&] (size_t x, size_t y)
                      {
                          auto up = [&] (size_t r) { return position[routes[r].to] < position[routes[r].from]; };
                          return up (x) && ! up (y);
                      });

    // Whether the routes without the removed ones still loop (Kahn's algorithm).
    auto loops = [&] (const std::vector<bool>& removed)
    {
        std::vector<int> incoming (inserts.size(), 0);
        std::vector<std::vector<size_t>> next (inserts.size());
        for (size_t i = 0; i < routes.size(); ++i)
            if (! removed[i])
            {
                ++incoming[position[routes[i].to]];
                next[position[routes[i].from]].push_back (position[routes[i].to]);
            }
        std::vector<size_t> ready;
        for (size_t n = 0; n < inserts.size(); ++n)
            if (incoming[n] == 0)
                ready.push_back (n);
        size_t visited = 0;
        while (! ready.empty())
        {
            const auto n = ready.back();
            ready.pop_back();
            ++visited;
            for (auto m : next[n])
                if (--incoming[m] == 0)
                    ready.push_back (m);
        }
        return visited < inserts.size();
    };

    std::vector<Route> removed;
    if (candidates.empty())
        return removed;

    if (candidates.size() <= 16)
    {
        // Every set of k candidates in order, k = 1, 2...: the first that breaks every loop.
        for (size_t k = 1; k <= candidates.size(); ++k)
        {
            std::vector<size_t> chosen (k);
            std::iota (chosen.begin(), chosen.end(), size_t { 0 });
            for (;;)
            {
                std::vector<bool> without (routes.size(), false);
                for (auto c : chosen)
                    without[candidates[c]] = true;
                if (! loops (without))
                {
                    for (auto c : chosen)
                        removed.push_back (routes[candidates[c]]);
                    for (const auto& route : removed)
                        removeRoute (project, route);
                    return removed;
                }
                // The next set: the last index that can still move moves, those after it follow.
                auto i = k;
                while (i > 0 && chosen[i - 1] == candidates.size() - k + i - 1)
                    --i;
                if (i == 0)
                    break;
                ++chosen[i - 1];
                for (auto j = i; j < k; ++j)
                    chosen[j] = chosen[j - 1] + 1;
            }
        }
    }

    for (auto loop = findRoutingLoop (project); ! loop.empty(); loop = findRoutingLoop (project))
    {
        const auto last =
            std::find_if (loop.rbegin(), loop.rend(), [] (const Route& r) { return r.kind != Route::Kind::output; });
        if (last == loop.rend())
            break; // outputs alone: broken before, never here
        removed.push_back (*last);
        removeRoute (project, *last);
    }
    return removed;
}

/** True if following outputs only from `start` arrives at `goal`. */
bool reachesByOutputs (const Project& project, InsertId start, InsertId goal)
{
    std::set<InsertId> seen;
    for (auto current = start; current.isValid() && seen.insert (current).second;)
    {
        if (current == goal)
            return true;
        const auto* insert = project.findInsert (current);
        if (insert == nullptr)
            return false;
        current = insert->output;
    }
    return false;
}

juce::String describe (InsertId id) { return "insert " + juce::String (id.value); }

/** An insert as the user knows it: "the Master", "'Reverb'", or "insert 3" (its place in the mixer). */
juce::String insertName (const Project& project, InsertId id)
{
    if (id == masterInsertId)
        return "the Master";
    for (size_t i = 0; i < project.mixer.inserts.size(); ++i)
        if (project.mixer.inserts[i].id == id)
            return project.mixer.inserts[i].name.isNotEmpty() ? "'" + project.mixer.inserts[i].name + "'"
                                                              : "insert " + juce::String ((int)i + 1);
    return describe (id);
}

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
    };

    checkInsert (project.mixer.master);
    for (auto& insert : project.mixer.inserts)
        checkInsert (insert);

    // Outputs, sends and sidechains together: one search for the whole mixer.
    for (const auto& route : findRoutingLoop (project))
        problems.add (describe (route.from) + " is part of a routing cycle");

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
    // Loops of outputs alone: the insert plays into the Master instead.
    for (auto& insert : project.mixer.inserts)
        if (reachesByOutputs (project, insert.output, insert.id))
        {
            insert.output = masterInsertId;
            notes.add ("where " + insertName (project, insert.id) +
                       " played (it made a feedback loop; it plays into the Master now)");
        }
    // Loops through sends or sidechains: as few sends and sidechains as possible go (removeLoopingRoutes).
    for (const auto& route : removeLoopingRoutes (project))
    {
        const auto from = insertName (project, route.from), to = insertName (project, route.to);
        notes.add (route.kind == Route::Kind::send
                       ? "the send from " + from + " to " + to + " (it made a feedback loop)"
                       : "the sidechain from " + from + " into " + to + " (it made a feedback loop)");
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
