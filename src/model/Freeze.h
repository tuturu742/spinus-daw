#pragma once

#include "Model.h"

#include <set>

/*  Freeze: a frozen channel or insert plays audio rendered from its plugins
    instead of the plugins themselves, which are unloaded to save CPU.

      - A frozen channel plays its instrument's output (with its audio clips and the instrument's
        automation), before the channel's own volume and pan, which stay live.
      - A frozen insert plays everything that flows into it, through its effects, before its fader. Its
        fader, mute, solo and sends stay live. The channels and inserts that play into it go quiet.

    The model only says what is frozen; the rendered audio is a cache (the project's cache/freeze/)
    found again by freezeKey(). These helpers are pure functions of the project.
*/

namespace spinus::model
{

/** A channel or a mixer insert. */
using FreezeTarget = std::variant<ChannelId, InsertId>;

/** The inserts that reach these ones through outputs or sends, including themselves. */
std::set<InsertId> insertsReaching (const Project&, std::set<InsertId>);

/** The inserts that play only to feed a sidechain inside `group` (e.g. the kick's insert, for a
    Ducker on a bass bus), with everything that reaches them, outside the group. */
std::set<InsertId> sidechainFeedsOf (const Project&, const std::set<InsertId>& group);

/** Why the insert cannot be frozen, or an empty string if it can. Something inside it must not also
    play somewhere else (an output or send leaving it, or a sidechain outside following it): that
    sound would be lost once its plugins are unloaded. */
juce::String whyCannotFreeze (const Project&, InsertId);

/** What a frozen target renders: a project with only what it plays, routed straight to an empty
    master, with names, colours and unrelated objects left out, so that the same sound gives the same
    project. `unheard` are inserts that play only to feed sidechains (see EngineAdapter). */
struct FreezePlan
{
    Project project;
    std::set<InsertId> unheard;
    std::optional<PatternId> pattern; // render this pattern on its own (pattern mode), else the song
    bool silent = false;              // nothing to play: no render needed
};

FreezePlan makeFreezePlan (const Project&, FreezeTarget, std::optional<PatternId> pattern);

/** A name for the rendered audio: the same for the same plan and sample rate. Device settings are
    not part of it: a frozen device's plugin is unloaded, so they cannot change while it is frozen. */
juce::String freezeKey (const FreezePlan&, double sampleRate, const juce::String& extra = {});

/** The frozen targets that render their own audio: frozen channels and inserts that can be frozen,
    except those inside a frozen insert (which already plays their sound). */
std::vector<FreezeTarget> getEffectiveFreezeTargets (const Project&);

/** What goes quiet when targets play frozen audio: channels whose instrument is unloaded and inserts
    whose effects are unloaded (including the frozen inserts themselves). */
struct FrozenParts
{
    std::set<ChannelId> channels;
    std::set<InsertId> inserts;
    std::set<DeviceId> devices;
};

FrozenParts getFrozenParts (const Project&, const std::vector<FreezeTarget>&);

juce::String describe (FreezeTarget);

} // namespace spinus::model
