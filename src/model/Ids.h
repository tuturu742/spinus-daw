#pragma once

#include <cstdint>
#include <functional>

namespace spinus::model
{

/** A stable identifier for one kind of model object.

    IDs are allocated from one project-wide counter and never reused, so references (routing,
    automation targets, clip → pattern) survive reordering, renaming and deletion of other objects.
    The tag type only makes the different kinds of IDs incompatible at compile time.
*/
template <typename Tag> struct Id
{
    uint32_t value = 0;

    constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=> (const Id&) const = default;
};

using ChannelId = Id<struct ChannelTag>;
using PatternId = Id<struct PatternTag>;
using LaneId = Id<struct LaneTag>;
using ClipId = Id<struct ClipTag>;
using InsertId = Id<struct InsertTag>;
using DeviceId = Id<struct DeviceTag>;               // one instrument or effect instance
using LauncherTrackId = Id<struct LauncherTrackTag>; // a column of the clip launcher
using SceneId = Id<struct SceneTag>;                 // a row of the clip launcher
using MarkerId = Id<struct MarkerTag>;               // a named point on the Playlist's ruler

} // namespace spinus::model

template <typename Tag> struct std::hash<spinus::model::Id<Tag>>
{
    size_t operator() (spinus::model::Id<Tag> id) const noexcept { return std::hash<uint32_t>() (id.value); }
};
