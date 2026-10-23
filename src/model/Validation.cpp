#include "Validation.h"

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

} // namespace spinus::model
