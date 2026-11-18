#include "model/Tempo.h"

#include <cmath>

namespace spinus::model
{

TempoMap::TempoMap (const ProjectSettings& settings)
{
    segments.push_back ({ 0.0, 0.0, settings.tempoBpm });
    for (auto& change : settings.tempoChanges)
    {
        const auto& last = segments.back();
        const auto at = (double)change.at;
        if (at <= last.fromTicks)
            continue; // not after the previous one: validation refuses such lists
        segments.push_back ({ at, last.fromSeconds + (at - last.fromTicks) * secondsPerTick (last.bpm), change.bpm });
    }
}

double TempoMap::bpmAt (double ticks) const
{
    auto bpm = segments.front().bpm;
    for (auto& s : segments)
        if (s.fromTicks <= ticks)
            bpm = s.bpm;
    return bpm;
}

double TempoMap::seconds (double ticks) const
{
    const Segment* in = &segments.front();
    for (auto& s : segments)
        if (s.fromTicks <= ticks)
            in = &s;
    return in->fromSeconds + (ticks - in->fromTicks) * secondsPerTick (in->bpm);
}

double TempoMap::ticks (double seconds) const
{
    const Segment* in = &segments.front();
    for (auto& s : segments)
        if (s.fromSeconds <= seconds)
            in = &s;
    return in->fromTicks + (seconds - in->fromSeconds) / secondsPerTick (in->bpm);
}

juce::String tempoChangesProblem (const std::vector<TempoChange>& changes)
{
    Ticks previous = 0;
    for (auto& change : changes)
    {
        if (change.at <= previous)
            return "tempo changes need to be after the start, in order, one per position";
        if (! std::isfinite (change.bpm) || change.bpm < minimumBpm || change.bpm > maximumBpm)
            return "a tempo needs to be " + juce::String (minimumBpm, 0) + " to " + juce::String (maximumBpm, 0) +
                   " BPM";
        previous = change.at;
    }
    return {};
}

juce::String defaultOnTempoChange (const Project& p, const Clip& clip)
{
    const auto* audio = std::get_if<AudioClip> (&clip.content);
    if (audio == nullptr)
        return "beat";
    const TempoMap map (p.settings);
    const auto seconds = map.seconds ((double)(clip.start + clip.length)) - map.seconds ((double)clip.start);
    return seconds > 20.0 ? "time" : "beat";
}

void retimeAudioClips (Project& p, const ProjectSettings& before)
{
    const TempoMap was (before), now (p.settings);
    for (auto& clip : p.playlist.clips)
    {
        auto* audio = std::get_if<AudioClip> (&clip.content);
        if (audio == nullptr)
            continue;
        const auto from = was.seconds ((double)clip.start);
        const auto to = was.seconds ((double)(clip.start + clip.length));
        const auto start = audio->onTempoChange == "time" ? (Ticks)std::llround (now.ticks (from)) : clip.start;
        const auto end = (Ticks)std::llround (now.ticks (now.seconds ((double)start) + (to - from)));

        // Offset and fades stay as long in seconds, at the tempo where the clip starts.
        const auto scale = TempoMap::secondsPerTick (was.bpmAt ((double)clip.start)) /
                           TempoMap::secondsPerTick (now.bpmAt ((double)start));
        auto scaled = [scale] (Ticks t) { return (Ticks)std::llround ((double)t * scale); };
        audio->offset = scaled (audio->offset);
        audio->fadeIn = scaled (audio->fadeIn);
        audio->fadeOut = scaled (audio->fadeOut);

        clip.start = juce::jmax<Ticks> (0, start);
        clip.length = juce::jmax<Ticks> (1, end - start);
    }
}

} // namespace spinus::model
