#include "SongSections.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace spinus::songsections
{

namespace
{
constexpr double alike = 0.7; // phrases at least this similar are one kind

/** What one stem plays in one bar: its harmony (pitch classes, by duration), its rhythm (onsets on sixteenths)
    and how busy it is. */
struct BarFeature
{
    std::array<double, 12> pitches {};
    std::array<double, 16> onsets {};
    double busy = 0.0;
    bool silent() const { return busy <= 0.0; }
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
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt (aa * bb) : (aa <= 0.0 && bb <= 0.0 ? 1.0 : 0.0);
}

/** How alike two bars of one stem are, 0..1; nullopt when both are silent (nothing to compare). */
std::optional<double> similarity (const BarFeature& a, const BarFeature& b)
{
    if (a.silent() && b.silent())
        return std::nullopt;
    if (a.silent() != b.silent())
        return 0.0;
    return 0.5 * cosine (a.pitches.data(), b.pitches.data(), 12) + 0.5 * cosine (a.onsets.data(), b.onsets.data(), 16);
}

using Features = std::vector<std::vector<BarFeature>>; // [stem][bar]

double barSimilarity (const Features& features, int a, int b)
{
    double sum = 0.0;
    int counted = 0;
    for (const auto& stem : features)
        if (const auto s = similarity (stem[(size_t)a], stem[(size_t)b]))
        {
            sum += s.value();
            ++counted;
        }
    return counted > 0 ? sum / counted : 1.0; // silent everywhere in both
}

double busyness (const Features& features, int firstBar, int bars)
{
    double sum = 0.0;
    for (const auto& stem : features)
        for (int b = firstBar; b < firstBar + bars; ++b)
            sum += stem[(size_t)b].busy;
    return bars > 0 ? sum / bars : 0.0;
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
    Features features;
    for (const auto& [name, notes] : stems)
    {
        std::vector<BarFeature> perBar ((size_t)bars);
        for (const auto& n : notes)
        {
            const auto start = std::max<model::Ticks> (0, n.start);
            const auto bar = (int)(start / barLength);
            if (bar >= bars)
                continue;
            auto& f = perBar[(size_t)bar];
            const auto within = start - (model::Ticks)bar * barLength;
            f.pitches[(size_t)(((n.pitch % 12) + 12) % 12)] += (double)std::min (n.length, barLength - within);
            f.onsets[(size_t)juce::jlimit (0, 15, (int)(within * 16 / barLength))] += 1.0;
            f.busy += std::max (0.1, (double)n.velocity);
        }
        features.push_back (std::move (perBar));
    }

    // Phrases, each the kind of the first phrase it is like (or a new kind).
    struct Phrase
    {
        int firstBar, bars, kind;
    };
    std::vector<Phrase> phrases;
    std::vector<int> firstOfKind; // the phrase each kind is compared by
    for (int first = 0; first < bars; first += phraseBars)
    {
        Phrase phrase { first, std::min (phraseBars, bars - first), -1 };
        double best = 0.0;
        for (int k = 0; k < (int)firstOfKind.size(); ++k)
        {
            const auto& other = phrases[(size_t)firstOfKind[(size_t)k]];
            const auto shared = std::min (phrase.bars, other.bars);
            double sum = 0.0;
            for (int b = 0; b < shared; ++b)
                sum += barSimilarity (features, phrase.firstBar + b, other.firstBar + b);
            if (const auto s = sum / shared; s >= alike && s > best)
            {
                best = s;
                phrase.kind = k;
            }
        }
        if (phrase.kind < 0)
        {
            phrase.kind = (int)firstOfKind.size();
            firstOfKind.push_back ((int)phrases.size());
        }
        phrases.push_back (phrase);
    }

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
    const auto kinds = (int)firstOfKind.size();
    std::vector<int> count ((size_t)kinds, 0);
    std::vector<double> busy ((size_t)kinds, 0.0);
    for (const auto& r : runs)
    {
        ++count[(size_t)r.kind];
        busy[(size_t)r.kind] += busyness (features, r.firstBar, r.bars);
    }
    std::vector<int> repeated;
    for (int k = 0; k < kinds; ++k)
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
        // The shortest loop that most of the section repeats; none: the section is one piece.
        int loopBars = section.bars;
        std::optional<PieceKey> loop; // the notes it loops
        for (const int bars : { 1, 2, 4 })
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
            if (most != chunks.end() && most->second >= 2 && most->second * 2 >= whole && ! most->first.second.empty())
            {
                loopBars = bars;
                loop = most->first;
                break;
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
