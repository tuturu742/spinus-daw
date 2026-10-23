#include "Model.h"

namespace spinus::model
{

namespace
{
template <typename Container, typename IdType> auto* findById (Container& items, IdType id)
{
    auto it = std::find_if (items.begin(), items.end(), [id] (const auto& item) { return item.id == id; });
    return it != items.end() ? &*it : nullptr;
}
} // namespace

Project::Project()
{
    mixer.master.id = masterInsertId;
    mixer.master.name = "Master";
    mixer.master.output = {};
}

Channel* Project::findChannel (ChannelId id) { return findById (channels, id); }
const Channel* Project::findChannel (ChannelId id) const { return findById (channels, id); }
Pattern* Project::findPattern (PatternId id) { return findById (patterns, id); }
const Pattern* Project::findPattern (PatternId id) const { return findById (patterns, id); }
Lane* Project::findLane (LaneId id) { return findById (playlist.lanes, id); }
const Lane* Project::findLane (LaneId id) const { return findById (playlist.lanes, id); }
bool Project::isLaneMuted (LaneId id) const
{
    // Bounded: a damaged file could hold a cycle.
    for (int depth = 0; depth < 64; ++depth)
    {
        const auto* lane = findLane (id);
        if (lane == nullptr)
            return false;
        if (lane->muted)
            return true;
        id = lane->group;
    }
    return false;
}

bool Project::isLaneHidden (LaneId id) const
{
    const auto* lane = findLane (id);
    for (int depth = 0; lane != nullptr && depth < 64; ++depth)
    {
        lane = findLane (lane->group);
        if (lane != nullptr && lane->collapsed)
            return true;
    }
    return false;
}

int Project::getLaneDepth (LaneId id) const
{
    int depth = 0;
    for (const auto* lane = findLane (id); lane != nullptr && lane->group.isValid() && depth < 64; ++depth)
        lane = findLane (lane->group);
    return depth;
}

bool Project::isLaneGroupedUnder (LaneId id, LaneId ancestor) const
{
    const auto* lane = findLane (id);
    for (int depth = 0; lane != nullptr && depth < 64; ++depth)
    {
        if (lane->group == ancestor)
            return true;
        lane = findLane (lane->group);
    }
    return false;
}

Clip* Project::findClip (ClipId id) { return findById (playlist.clips, id); }
const Clip* Project::findClip (ClipId id) const { return findById (playlist.clips, id); }

Insert* Project::findInsert (InsertId id)
{
    return id == mixer.master.id ? &mixer.master : findById (mixer.inserts, id);
}

Ticks Project::fittedLength (const Pattern& pattern) const
{
    const auto bar = juce::jmax<Ticks> (1, getTimeSignature (pattern.id).barTicks());
    Ticks end = 0;
    for (auto& [channel, notes] : pattern.notes)
        for (auto& n : notes)
            end = juce::jmax (end, n.start + n.length);
    return juce::jmax<Ticks> (1, (end + bar - 1) / bar) * bar;
}

const Insert* Project::findInsert (InsertId id) const
{
    return id == mixer.master.id ? &mixer.master : findById (mixer.inserts, id);
}

const Device* Project::findDevice (DeviceId id) const
{
    for (auto& c : channels)
        if (c.instrument && c.instrument->id == id)
            return &*c.instrument;

    for (auto* insert : { &mixer.master })
        for (auto& slot : insert->effects)
            if (slot.device.id == id)
                return &slot.device;

    for (auto& insert : mixer.inserts)
        for (auto& slot : insert.effects)
            if (slot.device.id == id)
                return &slot.device;
    return nullptr;
}

bool Project::operator== (const Project& other) const
{
    return settings == other.settings && channels == other.channels && patterns == other.patterns &&
           playlist == other.playlist && mixer == other.mixer && launcher == other.launcher && nextId == other.nextId;
}

//==============================================================================
LauncherClip* Launcher::findClip (ClipId id)
{
    for (auto& c : clips)
        if (c.id == id)
            return &c;
    return nullptr;
}

const LauncherClip* Launcher::findClip (ClipId id) const { return const_cast<Launcher*> (this)->findClip (id); }

const LauncherClip* Launcher::findClip (LauncherTrackId track, SceneId scene) const
{
    for (auto& c : clips)
        if (c.track == track && c.scene == scene)
            return &c;
    return nullptr;
}

} // namespace spinus::model
