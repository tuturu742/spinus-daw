#include "SongSections.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace spinus::songsections
{

namespace
{
/** What one stem plays in one phrase: its harmony (pitch classes, by duration, a note counting up to a bar), its
    rhythm (onsets by sixteenth of the bar, all its bars together) and how busy it is. Summed over the phrase, so a
    wrong note or two of the transcription matter little. */
struct PhraseFeature
{
    std::array<double, 12> pitches {};
    std::array<double, 16> onsets {};
    double busy = 0.0;
};

double cosine (const double* a, const double* b, size_t n)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt (aa * bb) : 0.0;
}

using Features = std::vector<std::vector<PhraseFeature>>; // [stem][phrase]

/** How alike two phrases are, 0..1: each stem's harmony (0.6) and rhythm (0.4), weighed by how much it plays in
    both (a stem that plays in one of them only counts against, as much as it plays: a part coming in starts a
    section). */
double phraseSimilarity (const Features& features, size_t a, size_t b)
{
    double sum = 0.0, weights = 0.0;
    for (const auto& stem : features)
    {
        const auto &x = stem[a], &y = stem[b];
        if (x.busy <= 0.0 && y.busy <= 0.0)
            continue;
        if (x.busy <= 0.0 || y.busy <= 0.0)
        {
            weights += std::sqrt (std::max (x.busy, y.busy));
            continue;
        }
        const auto weight = std::sqrt (std::min (x.busy, y.busy));
        sum += weight * (0.6 * cosine (x.pitches.data(), y.pitches.data(), 12) +
                         0.4 * cosine (x.onsets.data(), y.onsets.data(), 16));
        weights += weight;
    }
    return weights > 0.0 ? sum / weights : 1.0; // silent everywhere in both
}

/** Groups the phrases into kinds (average linkage: the two groups most alike on average join, while they are at
    least `alike`). Each phrase's kind, numbered in order of first appearance. */
std::vector<int> groupPhrases (const std::vector<std::vector<double>>& similarity, double alike)
{
    const auto n = similarity.size();
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < n; ++i)
        groups.push_back ({ i });
    auto sums = similarity; // between groups, summed over their members
    while (groups.size() > 1)
    {
        double best = -1.0;
        size_t a = 0, b = 0;
        for (size_t i = 0; i < groups.size(); ++i)
            for (size_t j = i + 1; j < groups.size(); ++j)
                if (const auto mean = sums[i][j] / (double)(groups[i].size() * groups[j].size()); mean > best)
                {
                    best = mean;
                    a = i;
                    b = j;
                }
        if (best < alike)
            break;
        groups[a].insert (groups[a].end(), groups[b].begin(), groups[b].end());
        for (size_t k = 0; k < groups.size(); ++k)
        {
            sums[a][k] += sums[b][k];
            sums[k][a] = sums[a][k];
        }
        groups.erase (groups.begin() + (std::ptrdiff_t)b);
        sums.erase (sums.begin() + (std::ptrdiff_t)b);
        for (auto& row : sums)
            row.erase (row.begin() + (std::ptrdiff_t)b);
    }
    std::sort (groups.begin(), groups.end(), [] (const auto& x, const auto& y)
               { return *std::min_element (x.begin(), x.end()) < *std::min_element (y.begin(), y.end()); });
    std::vector<int> kinds (n, 0);
    for (size_t k = 0; k < groups.size(); ++k)
        for (const auto i : groups[k])
            kinds[i] = (int)k;
    return kinds;
}

/** A note as pieces compare them: where it starts in the piece, how long, which key. */
using NoteKey = std::tuple<model::Ticks, model::Ticks, int>;
using PieceKey = std::pair<model::Ticks, std::vector<NoteKey>>;

std::vector<model::Note> notesIn (const std::vector<model::Note>& notes, model::Ticks from, model::Ticks to)
{
    std::vector<model::Note> inside;
    for (const auto& n : notes)
        if (const auto start = std::max<model::Ticks> (0, n.start); start >= from && start < to)
        {
            auto moved = n;
            moved.start = start - from;
            inside.push_back (moved);
        }
    std::sort (inside.begin(), inside.end(), [] (const model::Note& a, const model::Note& b)
               { return std::tie (a.start, a.pitch, a.length) < std::tie (b.start, b.pitch, b.length); });
    return inside;
}

PieceKey keyOf (const std::vector<model::Note>& notes, model::Ticks length)
{
    PieceKey key { length, {} };
    for (const auto& n : notes)
        key.second.emplace_back (n.start, n.length, n.pitch);
    return key;
}
} // namespace

std::vector<Section> find (const std::map<juce::String, std::vector<model::Note>>& stems, model::Ticks barLength,
                           int bars, int phraseBars)
{
    if (bars <= 0 || barLength <= 0)
        return {};
    phraseBars = std::max (1, phraseBars);
    struct Phrase
    {
        int firstBar, bars, kind;
    };
    std::vector<Phrase> phrases;
    for (int first = 0; first < bars; first += phraseBars)
        phrases.push_back ({ first, std::min (phraseBars, bars - first), 0 });
    Features features;
    for (const auto& [name, notes] : stems)
    {
        std::vector<PhraseFeature> perPhrase (phrases.size());
        for (const auto& n : notes)
        {
            const auto start = std::max<model::Ticks> (0, n.start);
            const auto bar = (int)(start / barLength);
            if (bar >= bars)
                continue;
            auto& f = perPhrase[(size_t)(bar / phraseBars)];
            const auto within = start - (model::Ticks)bar * barLength;
            f.pitches[(size_t)(((n.pitch % 12) + 12) % 12)] += (double)std::min (n.length, barLength);
            f.onsets[(size_t)juce::jlimit (0, 15, (int)(within * 16 / barLength))] += 1.0;
            f.busy += std::max (0.1, (double)n.velocity);
        }
        features.push_back (std::move (perPhrase));
    }

    // Phrases alike are one kind. How alike counts as the same differs from song to song (how clean the
    // transcription is, how much the parts vary), so the bar is the song's own: its 60th percentile of how alike
    // two phrases are, within 0.6..0.85 (tried on 56 transcribed songs: fewer and the verses and choruses split
    // in kinds of their own, more and unlike parts join).
    const auto phraseCount = phrases.size();
    std::vector<std::vector<double>> similarity (phraseCount, std::vector<double> (phraseCount, 1.0));
    std::vector<double> pairs;
    for (size_t a = 0; a < phraseCount; ++a)
        for (size_t b = a + 1; b < phraseCount; ++b)
        {
            similarity[a][b] = similarity[b][a] = phraseSimilarity (features, a, b);
            pairs.push_back (similarity[a][b]);
        }
    double alike = 0.85;
    if (pairs.size() >= 10)
    {
        const auto at = pairs.begin() + (std::ptrdiff_t)(pairs.size() * 3 / 5);
        std::nth_element (pairs.begin(), at, pairs.end());
        alike = juce::jlimit (0.6, 0.85, *at);
    }
    const auto kinds = groupPhrases (similarity, alike);
    const auto kindsFound = (size_t)*std::max_element (kinds.begin(), kinds.end()) + 1;
    for (size_t i = 0; i < phraseCount; ++i)
        phrases[i].kind = kinds[i];
    // A phrase of its own kind between two of one kind is that kind, varied (a fill, a different last line).
    for (size_t i = 1; i + 1 < phraseCount; ++i)
        if (phrases[i - 1].kind == phrases[i + 1].kind && phrases[i].kind != phrases[i - 1].kind &&
            std::count (kinds.begin(), kinds.end(), kinds[i]) == 1)
            phrases[i].kind = phrases[i - 1].kind;
    // Two kinds taking turns (ABAB: a chorus of two different halves) are one section, of a kind of their own.
    std::map<std::pair<int, int>, int> pairKinds;
    for (size_t i = 0; i + 3 < phraseCount;)
    {
        const auto x = phrases[i].kind, y = phrases[i + 1].kind;
        if (x == y || phrases[i + 2].kind != x || phrases[i + 3].kind != y)
        {
            ++i;
            continue;
        }
        auto end = i + 4;
        while (end + 1 < phraseCount && phrases[end].kind == x && phrases[end + 1].kind == y)
            end += 2;
        const auto kind =
            pairKinds.emplace (std::make_pair (x, y), (int)kindsFound + (int)pairKinds.size()).first->second;
        for (auto p = i; p < end; ++p)
            phrases[p].kind = kind;
        i = end;
    }

    const auto busyness = [&features, phraseBars] (int firstBar, int barsLong)
    {
        double sum = 0.0;
        for (const auto& stem : features)
            for (int p = firstBar / phraseBars; p * phraseBars < firstBar + barsLong; ++p)
                sum += stem[(size_t)p].busy;
        return barsLong > 0 ? sum / barsLong : 0.0;
    };

    // Runs of one kind are sections.
    struct Run
    {
        int firstBar, bars, kind;
    };
    std::vector<Run> runs;
    for (const auto& p : phrases)
        if (! runs.empty() && runs.back().kind == p.kind)
            runs.back().bars += p.bars;
        else
            runs.push_back ({ p.firstBar, p.bars, p.kind });

    // Name each kind: how often it comes back and how busy it is.
    int kindCount = 0;
    for (const auto& p : phrases)
        kindCount = std::max (kindCount, p.kind + 1);
    std::vector<int> count ((size_t)kindCount, 0);
    std::vector<double> busy ((size_t)kindCount, 0.0);
    for (const auto& r : runs)
    {
        ++count[(size_t)r.kind];
        busy[(size_t)r.kind] += busyness (r.firstBar, r.bars);
    }
    std::vector<int> repeated;
    for (int k = 0; k < kindCount; ++k)
        if (count[(size_t)k] > 1)
            repeated.push_back (k);
    std::sort (repeated.begin(), repeated.end(),
               [&] (int a, int b) { return busy[(size_t)a] / count[(size_t)a] > busy[(size_t)b] / count[(size_t)b]; });
    std::map<int, juce::String> names;
    for (size_t i = 0; i < repeated.size(); ++i)
        names[repeated[i]] = i == 0   ? juce::String ("Chorus")
                             : i == 1 ? juce::String ("Verse")
                                      : "Part " + juce::String::charToString ((juce::juce_wchar)('C' + (i - 2) % 24));
    const auto firstRepeated =
        std::find_if (runs.begin(), runs.end(), [&] (const Run& r) { return count[(size_t)r.kind] > 1; });
    const auto lastRepeated =
        std::find_if (runs.rbegin(), runs.rend(), [&] (const Run& r) { return count[(size_t)r.kind] > 1; });
    std::vector<Section> sections;
    for (auto r = runs.begin(); r != runs.end(); ++r)
    {
        juce::String name;
        if (const auto found = names.find (r->kind); found != names.end())
            name = found->second;
        else if (firstRepeated == runs.end())
            name = runs.size() == 1 ? "Song" : "Part " + juce::String (sections.size() + 1);
        else if (r < firstRepeated)
            name = "Intro";
        else if (r > lastRepeated.base() - 1)
            name = "Outro";
        else
            name = "Bridge";
        // Two sections in a row of one name (two intros) are one.
        if (! sections.empty() && sections.back().name == name)
            sections.back().bars += r->bars;
        else
            sections.push_back ({ name, r->firstBar, r->bars });
    }
    return sections;
}

juce::StringArray markerNames (const std::vector<Section>& sections)
{
    std::map<juce::String, int> total, seen;
    for (const auto& s : sections)
        ++total[s.name];
    juce::StringArray names;
    for (const auto& s : sections)
        names.add (total[s.name] > 1 ? s.name + " " + juce::String (++seen[s.name]) : s.name);
    return names;
}

Split split (const std::vector<model::Note>& notes, const std::vector<Section>& sections, model::Ticks barLength)
{
    Split result;
    std::map<PieceKey, int> known; // the same notes anywhere: the same piece
    for (const auto& section : sections)
    {
        if (section.bars <= 0)
            continue;
        const auto from = (model::Ticks)section.firstBar * barLength;
        // The loop of 1, 2, 4 or 8 bars that covers the most of the section, at least half (the shorter of two
        // that cover as much); none: the section is one piece.
        int loopBars = section.bars;
        std::optional<PieceKey> loop; // the notes it loops
        double covered = 0.0;
        for (const int bars : { 1, 2, 4, 8 })
        {
            if (bars * 2 > section.bars)
                break;
            std::map<PieceKey, int> chunks;
            int whole = 0;
            for (int b = 0; b + bars <= section.bars; b += bars, ++whole)
            {
                const auto start = from + (model::Ticks)b * barLength, length = (model::Ticks)bars * barLength;
                ++chunks[keyOf (notesIn (notes, start, start + length), length)];
            }
            const auto most = std::max_element (chunks.begin(), chunks.end(),
                                                [] (const auto& a, const auto& b) { return a.second < b.second; });
            if (most == chunks.end() || most->second < 2 || most->first.second.empty())
                continue;
            if (const auto share = (double)most->second / whole; share >= 0.5 && share > covered)
            {
                covered = share;
                loopBars = bars;
                loop = most->first;
            }
        }
        for (int b = 0; b < section.bars; b += loopBars)
        {
            const auto bars = std::min (loopBars, section.bars - b);
            const auto start = from + (model::Ticks)b * barLength, length = (model::Ticks)bars * barLength;
            auto inside = notesIn (notes, start, start + length);
            if (inside.empty())
                continue;
            auto key = keyOf (inside, length);
            auto found = known.find (key);
            if (found == known.end())
            {
                const auto name =
                    loop.has_value() ? section.name + (key == loop.value() ? " loop" : " fill") : section.name;
                found = known.emplace (std::move (key), (int)result.pieces.size()).first;
                result.pieces.push_back ({ name, length, std::move (inside) });
            }
            result.placed.push_back ({ found->second, start });
        }
    }
    return result;
}

} // namespace spinus::songsections
