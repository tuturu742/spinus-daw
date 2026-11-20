#include "MelodyMatch.h"

#include "model/Tempo.h"

namespace spinus
{

namespace
{
constexpr double nearbySeconds = 0.15; // a melody note starting this close to a sung note with no overlap
constexpr double shortest = 0.25, longest = 4.0;
} // namespace

std::vector<MelodyMatch::Note> MelodyMatch::melodyFor (const model::Project& project, model::ClipId audioClip,
                                                       model::ChannelId channel)
{
    const auto* vocal = project.findClip (audioClip);
    const auto* audio = vocal != nullptr ? std::get_if<model::AudioClip> (&vocal->content) : nullptr;
    if (audio == nullptr || audio->reversed || ! channel)
        return {};

    const model::TempoMap tempo (project.settings);
    const auto from = (double)vocal->start, to = (double)(vocal->start + vocal->length);
    auto fileSeconds = [&] (double ticks)
    { return model::secondsInFile (tempo, *vocal, *audio, juce::jlimit (from, to, ticks) - from); };

    std::vector<Note> melody;
    for (auto& clip : project.playlist.clips)
    {
        const auto* pc = std::get_if<model::PatternClip> (&clip.content);
        const auto* pattern = pc != nullptr ? project.findPattern (pc->pattern) : nullptr;
        if (pattern == nullptr || clip.muted || project.isLaneMuted (clip.lane) || ! pattern->notes.contains (channel))
            continue;
        for (auto& n : pattern->notes.at (channel))
        {
            // As the Engine Adapter plays it: from the clip's offset, for its length; patterns don't loop.
            if (n.start < pc->offset || n.start >= pc->offset + clip.length || n.start >= pattern->length)
                continue;
            const auto start = (double)(clip.start + n.start + pattern->getSwingDelay (n.start) - pc->offset);
            const auto end = juce::jmin (start + (double)n.length, (double)(clip.start + clip.length));
            if (end <= from || start >= to)
                continue;
            melody.push_back ({ fileSeconds (start), fileSeconds (end), n.pitch });
        }
    }
    std::erase_if (melody, [] (const Note& n) { return n.end <= n.start; });
    std::sort (melody.begin(), melody.end(),
               [] (const Note& a, const Note& b) { return std::tie (a.start, a.pitch) < std::tie (b.start, b.pitch); });
    return melody;
}

MelodyMatch::Result MelodyMatch::match (std::vector<model::TuneNote> sung, const std::vector<Note>& melody, bool timing,
                                        const std::vector<int>& indices, double fileSeconds)
{
    const auto count = (int)sung.size();
    std::set<int> chosen;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, count))
            chosen.insert (i);
    if (indices.empty())
        for (int i = 0; i < count; ++i)
            chosen.insert (i);

    Result result;
    const auto original = sung;

    // Each sung note's melody note: the one it overlaps most (a quarter of either at least), else one starting
    // close by.
    std::map<int, int> partner;
    std::map<int, int> sungPer; // melody note -> how many sung notes it got
    for (auto i : chosen)
    {
        const auto& n = sung[(size_t)i];
        const auto a = n.outputStart(), b = n.outputEnd();
        int best = -1;
        double bestOverlap = 0.0;
        for (int m = 0; m < (int)melody.size(); ++m)
        {
            const auto& note = melody[(size_t)m];
            const auto overlap = juce::jmin (b, note.end) - juce::jmax (a, note.start);
            if (overlap > bestOverlap && overlap >= 0.25 * juce::jmin (b - a, note.end - note.start))
            {
                best = m;
                bestOverlap = overlap;
            }
        }
        if (best < 0)
        {
            auto closest = nearbySeconds;
            for (int m = 0; m < (int)melody.size(); ++m)
                if (const auto apart = std::abs (melody[(size_t)m].start - a); apart <= closest)
                {
                    best = m;
                    closest = apart;
                }
        }
        if (best < 0)
        {
            result.unmatched.push_back (i);
            continue;
        }
        partner[i] = best;
        ++sungPer[best];
    }

    // Its pitch, in the octave sung.
    for (auto [i, m] : partner)
    {
        auto& n = sung[(size_t)i];
        const auto written = (float)melody[(size_t)m].pitch;
        const auto target = written + 12.0f * std::round ((n.pitch - written) / 12.0f);
        n.shift = juce::jlimit (-24.0f, 24.0f, target - n.pitch);
        ++result.matched;
    }

    // Its time, where the sung note is its melody note's only one: in order, each after the one before and before
    // the next (where the next is placed too, before where it wants to start).
    if (timing)
    {
        std::map<int, const Note*> wanted;
        for (auto [i, m] : partner)
            if (sungPer[m] == 1)
                wanted[i] = &melody[(size_t)m];
        for (auto [i, target] : wanted)
        {
            auto& n = sung[(size_t)i];
            const auto length = n.end - n.start;
            const auto low = i > 0 ? sung[(size_t)i - 1].outputEnd() : 0.0;
            const auto next = i + 1 < count ? wanted.find (i + 1) : wanted.end();
            const auto high = i + 1 >= count         ? fileSeconds
                              : next != wanted.end() ? fileSeconds
                                                     : sung[(size_t)i + 1].outputStart();
            if (high - low < length * shortest)
                continue;
            const auto start = juce::jlimit (low, high - length * shortest, target->start);
            auto end =
                juce::jlimit (start + length * shortest, juce::jmin (high, start + length * longest), target->end);
            if (next != wanted.end())
                end = juce::jmax (start + length * shortest, juce::jmin (end, next->second->start));
            n.move = start - n.start;
            n.stretch = (end - start) / length;
            if (std::abs (n.stretch - 1.0) < 0.01)
                n.stretch = 1.0;
            if (std::abs (n.move) < 0.0005)
                n.move = 0.0;
        }

        // Where a note could not fit after all, it and the one before keep their own time, until none overlap.
        for (bool overlapping = true; overlapping;)
        {
            overlapping = false;
            for (int i = 1; i < count; ++i)
                if (sung[(size_t)i].outputStart() < sung[(size_t)i - 1].outputEnd() - 1.0e-6)
                {
                    overlapping = true;
                    for (auto k : { i, i - 1 })
                    {
                        sung[(size_t)k].move = original[(size_t)k].move;
                        sung[(size_t)k].stretch = original[(size_t)k].stretch;
                    }
                }
        }
        // Placed: on its melody note's start now (moved, or there already), as far as the neighbours allowed.
        for (auto [i, target] : wanted)
        {
            const auto& n = sung[(size_t)i];
            const auto& was = original[(size_t)i];
            if (! juce::exactlyEqual (n.move, was.move) || ! juce::exactlyEqual (n.stretch, was.stretch) ||
                std::abs (n.outputStart() - target->start) < 0.001)
                ++result.timed;
        }
    }

    result.notes = std::move (sung);
    return result;
}

} // namespace spinus
