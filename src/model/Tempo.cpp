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

juce::String onTempoChangeProblem (const AudioClip& audio)
{
    if (audio.onTempoChange == "time" || audio.onTempoChange == "beat")
        return {};
    if (audio.onTempoChange != "stretch")
        return "has an unknown onTempoChange '" + audio.onTempoChange + "'";
    if (! std::isfinite (audio.sourceBpm) || audio.sourceBpm < minimumBpm || audio.sourceBpm > maximumBpm)
        return "is stretched to the tempo but its sourceBpm is not " + juce::String (minimumBpm, 0) + " to " +
               juce::String (maximumBpm, 0);
    return {};
}

std::optional<double> bpmFromFileName (const juce::String& file)
{
    const auto name =
        file.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false).toLowerCase();
    auto separator = [] (juce::juce_wchar c) { return c == ' ' || c == '_' || c == '-'; };
    auto number = [] (juce::juce_wchar c) { return juce::CharacterFunctions::isDigit (c) || c == '.'; };
    for (int at = name.indexOf ("bpm"); at >= 0; at = name.indexOf (at + 1, "bpm"))
    {
        // "120bpm", "120 BPM", "120_bpm" before it, or "bpm120", "BPM 120" after it.
        auto end = at;
        while (end > 0 && separator (name[end - 1]))
            --end;
        auto from = end;
        while (from > 0 && number (name[from - 1]))
            --from;
        auto digits = name.substring (from, end);
        if (! digits.containsAnyOf ("0123456789"))
        {
            from = at + 3;
            while (from < name.length() && separator (name[from]))
                ++from;
            end = from;
            while (end < name.length() && number (name[end]))
                ++end;
            digits = name.substring (from, end);
        }
        digits = digits.trimCharactersAtStart (".").trimCharactersAtEnd (".");
        if (const auto bpm = digits.getDoubleValue();
            digits.containsOnly ("0123456789.") && bpm >= 40.0 && bpm <= 300.0)
            return bpm;
    }
    return std::nullopt;
}

double secondsInFile (const TempoMap& map, const Clip& clip, const AudioClip& audio, double ticksIntoClip)
{
    const auto speed = getPlaybackSpeed (audio);
    if (audio.onTempoChange == "stretch" && audio.sourceBpm > 0.0)
        return ((double)audio.offset + ticksIntoClip) * TempoMap::secondsPerTick (audio.sourceBpm) * speed;
    const auto start = (double)clip.start;
    return ((double)audio.offset * TempoMap::secondsPerTick (map.bpmAt (start)) + map.seconds (start + ticksIntoClip) -
            map.seconds (start)) *
           speed;
}

Ticks audioClipLength (const TempoMap& map, Ticks start, const AudioClip& audio, double seconds)
{
    const auto speed = getPlaybackSpeed (audio);
    double end = 0.0;
    if (audio.onTempoChange == "stretch" && audio.sourceBpm > 0.0)
        end = (double)start + seconds / speed / TempoMap::secondsPerTick (audio.sourceBpm) - (double)audio.offset;
    else
        end = map.ticks (map.seconds ((double)start) + seconds / speed -
                         (double)audio.offset * TempoMap::secondsPerTick (map.bpmAt ((double)start)));
    return juce::jmax<Ticks> (1, (Ticks)std::llround (end) - start);
}

void keepSameAudio (const TempoMap& map, Clip& clip, const AudioClip& before)
{
    auto& audio = std::get<AudioClip> (clip.content);
    auto was = clip;
    was.content = before;
    const auto from = secondsInFile (map, was, before, 0.0);
    const auto to = secondsInFile (map, was, before, (double)clip.length);

    // Seconds of the file per tick where the clip starts, before and now: the offset and fades scale with it.
    const auto perTickBefore = secondsInFile (map, was, before, 1.0) - from;
    auto noOffset = audio;
    noOffset.offset = 0;
    const auto perTick = secondsInFile (map, clip, noOffset, 1.0) - secondsInFile (map, clip, noOffset, 0.0);
    auto scaled = [ratio = perTickBefore / perTick] (Ticks t) { return (Ticks)std::llround ((double)t * ratio); };

    audio.offset = (Ticks)std::llround (from / perTick);
    audio.fadeIn = scaled (before.fadeIn);
    audio.fadeOut = scaled (before.fadeOut);
    clip.length = audioClipLength (map, clip.start, audio, to);
}

void retimeAudioClips (Project& p, const ProjectSettings& before)
{
    const TempoMap was (before), now (p.settings);
    for (auto& clip : p.playlist.clips)
    {
        auto* audio = std::get_if<AudioClip> (&clip.content);
        if (audio == nullptr || audio->onTempoChange == "stretch")
            continue; // a stretched clip stays on its beats, as long in beats
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
