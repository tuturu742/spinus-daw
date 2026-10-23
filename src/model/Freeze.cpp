#include "Freeze.h"

#include "ModelJson.h"

#include <juce_cryptography/juce_cryptography.h>

namespace spinus::model
{

std::set<InsertId> insertsReaching (const Project& p, std::set<InsertId> feeds)
{
    for (bool grew = true; grew;)
    {
        grew = false;
        for (auto& insert : p.mixer.inserts)
        {
            if (feeds.contains (insert.id))
                continue;
            const bool reaches =
                feeds.contains (insert.output) || std::any_of (insert.sends.begin(), insert.sends.end(),
                                                               [&] (auto& s) { return feeds.contains (s.target); });
            if (reaches)
            {
                feeds.insert (insert.id);
                grew = true;
            }
        }
    }
    return feeds;
}

std::set<InsertId> sidechainFeedsOf (const Project& p, const std::set<InsertId>& group)
{
    std::set<InsertId> keyers;
    for (bool grew = true; grew;)
    {
        grew = false;
        std::set<InsertId> sources;
        for (auto& insert : p.mixer.inserts)
            if (group.contains (insert.id) || keyers.contains (insert.id))
                for (auto source : insert.sidechainSources)
                    if (! group.contains (source) && ! keyers.contains (source))
                        sources.insert (source);
        for (auto id : insertsReaching (p, sources))
            if (! group.contains (id))
                grew |= keyers.insert (id).second;
    }
    return keyers;
}

juce::String whyCannotFreeze (const Project& p, InsertId id)
{
    const auto* frozen = p.findInsert (id);
    if (frozen == nullptr)
        return "That insert does not exist.";
    if (id == masterInsertId)
        return "The master cannot be frozen: freeze the inserts that play into it.";

    const auto group = insertsReaching (p, { id });
    auto nameOf = [&] (InsertId i)
    {
        const auto* insert = p.findInsert (i);
        return insert != nullptr ? "'" + insert->name + "'" : juce::String ("an insert");
    };

    for (auto& insert : p.mixer.inserts)
    {
        if (insert.id == id)
            continue;

        if (group.contains (insert.id))
        {
            if (! group.contains (insert.output))
                return nameOf (insert.id) + " plays into " + frozen->name + " and also into " + nameOf (insert.output) +
                       ", so it cannot be frozen.";
            for (auto& send : insert.sends)
                if (! group.contains (send.target))
                    return nameOf (insert.id) + " plays into " + frozen->name + " and also sends to " +
                           nameOf (send.target) + ", so it cannot be frozen.";
        }
        else
        {
            for (auto source : insert.sidechainSources)
                if (source != id && group.contains (source))
                    return nameOf (insert.id) + " follows " + nameOf (source) + " as a sidechain, so " + frozen->name +
                           " cannot be frozen.";
        }
    }

    for (auto source : p.mixer.master.sidechainSources)
        if (source != id && group.contains (source))
            return "The master follows " + nameOf (source) + " as a sidechain, so " + frozen->name +
                   " cannot be frozen.";

    return {};
}

//==============================================================================
namespace
{
/** Keeps only the named channels, inserts and the clips, patterns and automation they play. */
FreezePlan prune (const Project& original, const std::set<ChannelId>& channels, const std::set<InsertId>& inserts,
                  const std::function<bool (const ParameterTarget&)>& keepAutomation, std::optional<PatternId> pattern)
{
    FreezePlan plan;
    plan.pattern = pattern;
    auto& p = plan.project;

    p.settings.tempoBpm = original.settings.tempoBpm;
    p.settings.timeSignature = original.settings.timeSignature;
    p.settings.sampleRate = original.settings.sampleRate;
    p.nextId = 0; // allocation does not change the sound

    for (auto& c : original.channels)
        if (channels.contains (c.id))
        {
            auto copy = c;
            copy.name = {};
            copy.colour = 0;
            copy.solo = false;
            copy.frozen = false;
            p.channels.push_back (copy);
        }

    for (auto& i : original.mixer.inserts)
        if (inserts.contains (i.id))
        {
            auto copy = i;
            copy.name = {};
            copy.colour = 0;
            copy.icon = {};
            copy.solo = false;
            copy.frozen = false;
            std::erase_if (copy.sends, [&] (auto& s) { return ! inserts.contains (s.target); });
            if (! inserts.contains (copy.output))
                copy.output = masterInsertId; // only feeds a sidechain: it plays unheard
            std::erase_if (copy.sidechainSources, [&] (auto id) { return ! inserts.contains (id); });
            p.mixer.inserts.push_back (copy);
        }

    // Only the notes of kept channels; patterns are kept by ID (clips refer to them).
    auto keepPattern = [&] (const Pattern& original_)
    {
        Pattern copy;
        copy.id = original_.id;
        copy.length = original_.length;
        copy.colour = 0;
        for (auto& [channel, notes] : original_.notes)
            if (channels.contains (channel) && ! notes.empty())
                copy.notes[channel] = notes;
        return copy;
    };

    if (pattern)
    {
        // Pattern mode plays one pattern; the playlist (audio clips, automation) is silent.
        if (const auto* source = original.findPattern (*pattern))
        {
            auto copy = keepPattern (*source);
            plan.silent = copy.notes.empty();
            p.patterns.push_back (std::move (copy));
        }
        else
        {
            plan.silent = true;
        }
        return plan;
    }

    const LaneId lane { 1 };
    p.playlist.lanes.push_back ({ lane, {}, 0, false });
    std::set<PatternId> used;
    bool sounds = false;

    for (auto& clip : original.playlist.clips)
    {
        const auto* clipLane = original.findLane (clip.lane);
        if (clip.muted || (clipLane != nullptr && clipLane->muted))
            continue;

        bool keep = false;
        if (auto* pc = std::get_if<PatternClip> (&clip.content))
        {
            if (const auto* source = original.findPattern (pc->pattern))
                for (auto& [channel, notes] : source->notes)
                    keep |= channels.contains (channel) && ! notes.empty();
            if (keep)
                used.insert (pc->pattern);
        }
        else if (auto* audio = std::get_if<AudioClip> (&clip.content))
            keep = channels.contains (audio->channel);
        else if (auto* automation = std::get_if<AutomationClip> (&clip.content))
            keep = ! automation->points.empty() && keepAutomation (automation->target);

        if (! keep)
            continue;

        sounds |= ! std::holds_alternative<AutomationClip> (clip.content);
        auto copy = clip;
        copy.lane = lane;
        p.playlist.clips.push_back (copy);
    }

    for (auto& source : original.patterns)
        if (used.contains (source.id))
            p.patterns.push_back (keepPattern (source));

    plan.silent = ! sounds;
    return plan;
}

std::set<DeviceId> devicesOf (const Project& p, const std::set<ChannelId>& channels, const std::set<InsertId>& inserts)
{
    std::set<DeviceId> result;
    for (auto& c : p.channels)
        if (channels.contains (c.id) && c.instrument)
            result.insert (c.instrument->id);
    for (auto& i : p.mixer.inserts)
        if (inserts.contains (i.id))
            for (auto& slot : i.effects)
                result.insert (slot.device.id);
    return result;
}
} // namespace

FreezePlan makeFreezePlan (const Project& original, FreezeTarget target, std::optional<PatternId> pattern)
{
    if (auto* channelId = std::get_if<ChannelId> (&target))
    {
        const auto* channel = original.findChannel (*channelId);
        if (channel == nullptr)
            return { {}, {}, pattern, true };

        // The instrument alone, before the channel's volume and pan (they stay live).
        const auto devices = devicesOf (original, { *channelId }, {});
        auto plan = prune (
            original, { *channelId }, {}, [&] (const ParameterTarget& t)
            { return t.kind == ParameterTarget::Kind::deviceParameter && devices.contains (t.device); }, pattern);

        auto& c = plan.project.channels.front();
        c.volumeDb = 0.0f;
        c.pan = 0.0f;
        c.muted = false;
        c.targetInsert = masterInsertId;
        return plan;
    }

    const auto insertId = std::get<InsertId> (target);
    const auto group = insertsReaching (original, { insertId });
    const auto keyers = sidechainFeedsOf (original, group);

    std::set<InsertId> inserts = group;
    inserts.insert (keyers.begin(), keyers.end());

    std::set<ChannelId> channels;
    for (auto& c : original.channels)
        if (inserts.contains (c.targetInsert))
            channels.insert (c.id);

    // Channels whose notes only reach effects inside (Insert::notesFrom): their notes, not their sound.
    std::set<ChannelId> noteSources;
    for (auto& i : original.mixer.inserts)
        if (inserts.contains (i.id) && i.notesFrom && ! channels.contains (i.notesFrom))
            noteSources.insert (i.notesFrom);
    auto withNoteSources = channels;
    withNoteSources.insert (noteSources.begin(), noteSources.end());

    const auto devices = devicesOf (original, channels, inserts);
    auto plan = prune (
        original, withNoteSources, inserts,
        [&] (const ParameterTarget& t)
        {
            using K = ParameterTarget::Kind;
            switch (t.kind)
            {
            case K::channelVolume:
            case K::channelPan:
                return channels.contains (t.channel);
            case K::insertVolume:
            case K::insertPan:
                return t.insert != insertId && inserts.contains (t.insert); // the frozen insert's fader is live
            case K::deviceParameter:
                return devices.contains (t.device);
            }
            return false;
        },
        pattern);

    for (auto& c : plan.project.channels)
        if (noteSources.contains (c.id))
        {
            c.instrument.reset(); // silent: only its notes matter
            c.targetInsert = masterInsertId;
        }
    std::erase_if (plan.project.playlist.clips,
                   [&] (const Clip& clip)
                   {
                       auto* audio = std::get_if<AudioClip> (&clip.content);
                       return audio != nullptr && noteSources.contains (audio->channel);
                   });

    plan.unheard = keyers;
    for (auto& i : plan.project.mixer.inserts)
        if (i.id == insertId)
        {
            // Its effects' output, before its fader (which stays live), straight to the master.
            i.volumeDb = 0.0f;
            i.pan = 0.0f;
            i.muted = false;
            i.output = masterInsertId;
            i.sends.clear();
        }

    return plan;
}

juce::String freezeKey (const FreezePlan& plan, double sampleRate, const juce::String& extra)
{
    juce::StringArray unheard;
    for (auto id : plan.unheard)
        unheard.add (juce::String (id.value));

    // Bump the version when the rendering changes in a way that makes old files wrong.
    const auto text = "freeze 1\n" + juce::String (sampleRate) + "\n" +
                      (plan.pattern ? "pattern " + juce::String (plan.pattern->value) : juce::String ("song")) + "\n" +
                      unheard.joinIntoString (",") + "\n" + extra + "\n" + toJsonText (plan.project);
    return juce::SHA256 (text.toUTF8()).toHexString().substring (0, 32);
}

//==============================================================================
std::vector<FreezeTarget> getEffectiveFreezeTargets (const Project& p)
{
    std::vector<InsertId> frozenInserts;
    for (auto& i : p.mixer.inserts)
        if (i.frozen && whyCannotFreeze (p, i.id).isEmpty())
            frozenInserts.push_back (i.id);

    // Inside another frozen insert: that one plays its sound.
    std::set<InsertId> covered;
    for (auto id : frozenInserts)
        for (auto inside : insertsReaching (p, { id }))
            if (inside != id)
                covered.insert (inside);

    std::vector<FreezeTarget> result;
    std::set<InsertId> outer;
    for (auto id : frozenInserts)
        if (! covered.contains (id))
        {
            result.emplace_back (id);
            outer.insert (id);
        }

    for (auto& c : p.channels)
        if (c.frozen && ! covered.contains (c.targetInsert) && ! outer.contains (c.targetInsert))
            result.emplace_back (c.id);

    return result;
}

FrozenParts getFrozenParts (const Project& p, const std::vector<FreezeTarget>& targets)
{
    FrozenParts parts;
    for (auto& target : targets)
    {
        if (auto* channel = std::get_if<ChannelId> (&target))
        {
            parts.channels.insert (*channel);
            continue;
        }

        for (auto id : insertsReaching (p, { std::get<InsertId> (target) }))
            parts.inserts.insert (id);
    }

    for (auto& c : p.channels)
        if (parts.inserts.contains (c.targetInsert))
            parts.channels.insert (c.id);

    parts.devices = devicesOf (p, parts.channels, parts.inserts);
    return parts;
}

juce::String describe (FreezeTarget target)
{
    if (auto* channel = std::get_if<ChannelId> (&target))
        return "channel " + juce::String (channel->value);
    return "insert " + juce::String (std::get<InsertId> (target).value);
}

} // namespace spinus::model
