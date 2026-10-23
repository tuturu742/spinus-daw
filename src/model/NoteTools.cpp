#include "NoteTools.h"

#include <map>

namespace spinus::model::noteTools
{

namespace
{
bool byStart (const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; }

/** The notes grouped by their start, each group low to high. */
std::map<Ticks, std::vector<Note>> byStartGroups (const std::vector<Note>& notes)
{
    std::map<Ticks, std::vector<Note>> groups;
    for (auto& n : notes)
        groups[n.start].push_back (n);
    for (auto& [start, group] : groups)
        std::sort (group.begin(), group.end(), [] (const Note& a, const Note& b) { return a.pitch < b.pitch; });
    return groups;
}
} // namespace

std::vector<Note> quantize (std::vector<Note> notes, Ticks grid)
{
    if (grid > 1)
        for (auto& n : notes)
            n.start = juce::jmax<Ticks> (0, (Ticks)std::llround ((double)n.start / (double)grid) * grid);
    std::sort (notes.begin(), notes.end(), byStart);
    return notes;
}

std::vector<Note> strum (const std::vector<Note>& notes, Ticks spacing, bool up)
{
    std::vector<Note> result;
    for (auto& [start, group] : byStartGroups (notes))
    {
        if (! up)
            std::reverse (group.begin(), group.end());
        for (size_t i = 0; i < group.size(); ++i)
        {
            auto n = group[i];
            const auto end = n.start + n.length;
            n.start += (Ticks)i * spacing;
            n.length = juce::jmax<Ticks> (1, end - n.start);
            result.push_back (n);
        }
    }
    std::sort (result.begin(), result.end(), byStart);
    return result;
}

std::vector<Note> arpeggiate (const std::vector<Note>& notes, Ticks step)
{
    std::vector<Note> result;
    step = juce::jmax<Ticks> (1, step);
    for (auto& [start, group] : byStartGroups (notes))
    {
        if (group.size() < 2)
        {
            result.push_back (group.front());
            continue;
        }
        Ticks length = 0;
        for (auto& n : group)
            length = std::max (length, n.length);
        size_t i = 0;
        for (Ticks at = start; at < start + length; at += step, ++i)
        {
            auto n = group[i % group.size()];
            n.start = at;
            n.length = juce::jmin (step, start + length - at);
            result.push_back (n);
        }
    }
    std::sort (result.begin(), result.end(), byStart);
    return result;
}

std::vector<Note> chop (const std::vector<Note>& notes, Ticks step)
{
    std::vector<Note> result;
    step = juce::jmax<Ticks> (1, step);
    for (auto& note : notes)
        for (Ticks at = note.start; at < note.start + note.length; at += step)
        {
            auto n = note;
            n.start = at;
            n.length = juce::jmin (step, note.start + note.length - at);
            result.push_back (n);
        }
    std::sort (result.begin(), result.end(), byStart);
    return result;
}

std::vector<Note> legato (std::vector<Note> notes)
{
    std::sort (notes.begin(), notes.end(), byStart);
    for (size_t i = 0; i < notes.size(); ++i)
        for (size_t j = i + 1; j < notes.size(); ++j)
            if (notes[j].start > notes[i].start)
            {
                notes[i].length = notes[j].start - notes[i].start;
                break;
            }
    return notes;
}

std::vector<Note> humanizeVelocity (std::vector<Note> notes, float amount, juce::Random& random)
{
    for (auto& n : notes)
        n.velocity = juce::jlimit (0.05f, 1.0f, n.velocity + (random.nextFloat() * 2.0f - 1.0f) * amount);
    return notes;
}

} // namespace spinus::model::noteTools
