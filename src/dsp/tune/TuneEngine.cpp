#include "TuneEngine.h"

namespace spinus::dsp
{

namespace
{
using Scale = TuneSettings::Scale;
using Range = TuneSettings::Range;

/** The pitch classes in each scale, counted from its first note: bit n is n semitones up. */
unsigned stepsOf (Scale scale)
{
    auto bits = [] (std::initializer_list<unsigned> steps)
    {
        unsigned result = 0;
        for (auto step : steps)
            result |= 1u << step;
        return result;
    };

    switch (scale)
    {
    case Scale::chromatic:
        return 0xfffu;
    case Scale::major:
        return bits ({ 0, 2, 4, 5, 7, 9, 11 });
    case Scale::minor:
        return bits ({ 0, 2, 3, 5, 7, 8, 10 });
    case Scale::harmonicMinor:
        return bits ({ 0, 2, 3, 5, 7, 8, 11 });
    case Scale::majorPentatonic:
        return bits ({ 0, 2, 4, 7, 9 });
    case Scale::minorPentatonic:
        return bits ({ 0, 3, 5, 7, 10 });
    case Scale::blues:
        return bits ({ 0, 3, 5, 6, 7, 10 });
    case Scale::dorian:
        return bits ({ 0, 2, 3, 5, 7, 9, 10 });
    case Scale::mixolydian:
        return bits ({ 0, 2, 4, 5, 7, 9, 10 });
    }
    return 1u;
}

float lowestHz (Range range) { return range == Range::low ? 48.0f : 75.0f; }
constexpr float highestHz = 1200.0f;

/** Where nothing is voiced, grains are this far apart (a neutral period). */
constexpr double neutralHz = 200.0;

/** Where grains overlap little, the output is divided by their summed window, but not by less than this. */
constexpr float minimumWindowSum = 0.3f;
} // namespace

int TuneEngine::getLatency (double sampleRate, Range range)
{
    // An odd number of samples, so a grain's half length is (latency - 1) / 2 (see placeGrains()).
    const auto samples = (int)std::floor (sampleRate * (range == Range::low ? 0.0195 : 0.0098));
    return samples % 2 == 0 ? samples + 1 : samples;
}

bool TuneEngine::isInScale (int note, int key, Scale scale)
{
    const auto step = (unsigned)(((note - key) % 12 + 12) % 12);
    return (stepsOf (scale) & (1u << step)) != 0;
}

int TuneEngine::chooseTarget (float pitch, int current, int key, Scale scale, const std::array<bool, 128>& heldNotes,
                              bool anyHeld)
{
    auto allowed = [&] (int note)
    { return note >= 0 && note < 128 && (anyHeld ? heldNotes[(size_t)note] : isInScale (note, key, scale)); };

    // Stay on the current note until the voice is clearly closer to another (no flicker in between).
    if (allowed (current) && std::abs (pitch - (float)current) < 0.65f)
        return current;

    int best = -1;
    float distance = 1000.0f;
    for (int note = 0; note < 128; ++note)
        if (allowed (note) && std::abs (pitch - (float)note) < distance)
        {
            distance = std::abs (pitch - (float)note);
            best = note;
        }
    return best;
}

//==============================================================================
void TuneEngine::prepare (double rate, int, int numChannels)
{
    sampleRate = rate;
    channels = juce::jmax (1, numChannels);
    detectors[0].prepare (rate, lowestHz (Range::normal), highestHz);
    detectors[1].prepare (rate, lowestHz (Range::low), highestHz);
    frame.assign ((size_t)juce::jmax (detectors[0].getFrameLength(), detectors[1].getFrameLength()), 0.0f);

    const auto needed = 4 * juce::jmax ((int)frame.size(), getLatency (rate, Range::low));
    const auto size = (size_t)juce::nextPowerOfTwo (needed);
    mask = size - 1;
    input.assign ((size_t)channels, std::vector<float> ((size_t)size, 0.0f));
    output.assign ((size_t)channels, std::vector<float> ((size_t)size, 0.0f));
    mono.assign ((size_t)size, 0.0f);
    windowSum.assign ((size_t)size, 0.0f);

    hop = juce::jmax (16, juce::roundToInt (rate * 0.0025)); // a pitch estimate every 2.5 ms
    wetStep = (float)(1.0 / (0.008 * rate));                 // 8 ms between dry and corrected
    reset();
}

void TuneEngine::reset()
{
    for (auto* rings : { &input, &output })
        for (auto& ring : *rings)
            std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (mono.begin(), mono.end(), 0.0f);
    std::fill (windowSum.begin(), windowSum.end(), 0.0f);

    written = 0;
    sinceAnalysis = 0;
    period = (float)(sampleRate / neutralHz);
    voiced = false;
    recent = {};
    target = -1;
    shift = 0.0f;
    heldSeconds = 0.0f;
    wet = 0.0f;
    held = {};
    numHeld = 0;
    numMarks = 0;
    newestMark = -1;
    nextAnalysisMark = 0.0;
    nextSynthesisMark = 0.0;
    inputPitch = 0.0f;
    targetNote = -1;
    correction = 0.0f;
}

//==============================================================================
void TuneEngine::process (juce::AudioBuffer<float>& audio, const juce::MidiBuffer& midi,
                          const TuneSettings& settings) noexcept
{
    if (mask == 0)
        return;

    for (const auto event : midi)
    {
        const auto message = event.getMessage();
        if (message.isNoteOn())
            held[(size_t)message.getNoteNumber()] = true;
        else if (message.isNoteOff())
            held[(size_t)message.getNoteNumber()] = false;
        else if (message.isAllNotesOff() || message.isAllSoundOff())
            held = {};
    }
    numHeld = (int)std::count (held.begin(), held.end(), true);

    if (settings.range != currentRange)
    {
        currentRange = settings.range; // a different delay: start over
        reset();
    }
    latency = getLatency (sampleRate, currentRange);

    const auto numChannels = juce::jmin (audio.getNumChannels(), channels);
    const auto mix = juce::jlimit (0.0f, 1.0f, settings.mix);

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto n = written;
        float sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto x = audio.getSample (ch, i);
            input[(size_t)ch][at (n)] = x;
            sum += x;
        }
        mono[at (n)] = sum / (float)juce::jmax (1, numChannels);
        ++written;

        if (++sinceAnalysis >= hop)
        {
            sinceAnalysis = 0;
            analyse (settings);
        }

        placeAnalysisMarks (n);
        placeGrains (n, settings);

        // Between dry and shifted: shifted only while there is a pitch to correct.
        const auto wanted = voiced && target >= 0 ? 1.0f : 0.0f;
        wet = wet < wanted ? juce::jmin (wanted, wet + wetStep) : juce::jmax (wanted, wet - wetStep);

        const auto t = n - latency;
        if (t < 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                audio.setSample (ch, i, 0.0f);
            continue;
        }

        const auto index = at (t);
        const auto sumOfWindows = windowSum[index];
        const auto gain = sumOfWindows > 1.0e-4f ? 1.0f / juce::jmax (sumOfWindows, minimumWindowSum) : 0.0f;
        windowSum[index] = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto dry = input[(size_t)ch][index];
            const auto shifted = output[(size_t)ch][index] * gain;
            output[(size_t)ch][index] = 0.0f;
            audio.setSample (ch, i, dry + mix * wet * (shifted - dry));
        }
    }
}

void TuneEngine::analyse (const TuneSettings& settings) noexcept
{
    auto& detector = detectors[currentRange == Range::low ? 1 : 0];
    const auto length = detector.getFrameLength();
    if (written < length)
        return;

    for (int i = 0; i < length; ++i)
        frame[(size_t)i] = readMono (written - length + i);

    const auto result = detector.analyse (frame.data());
    const auto seconds = (float)hop / (float)sampleRate;

    if (! result.isVoiced())
    {
        voiced = false;
        recent = {};
        target = -1;
        heldSeconds = 0.0f;
        inputPitch = 0.0f;
        targetNote = -1;
        return;
    }

    // The median of the last three estimates: one wrong octave does not jump.
    recent = { recent[1], recent[2], result.period };
    auto sorted = recent;
    std::sort (sorted.begin(), sorted.end());
    period = sorted[0] > 0.0f ? sorted[1] : result.period;
    voiced = true;

    const auto pitch = 69.0f + 12.0f * std::log2 ((float)sampleRate / period / 440.0f);
    const auto chosen = chooseTarget (pitch, target, settings.key, settings.scale, held, numHeld > 0);
    if (chosen != target)
    {
        target = chosen;
        heldSeconds = 0.0f;
    }
    else
    {
        heldSeconds += seconds;
    }

    if (target >= 0)
    {
        // Retune speed, slower on held notes when humanized (keeps their vibrato).
        const auto ms = juce::jmax (0.0f, settings.retuneMs) +
                        juce::jlimit (0.0f, 1.0f, settings.humanize) * juce::jmin (heldSeconds, 1.0f) * 300.0f;
        const auto alpha = ms <= 0.0f ? 1.0f : 1.0f - std::exp (-seconds * 1000.0f / ms);
        shift += alpha * ((float)target - pitch - shift);
    }

    inputPitch = pitch;
    targetNote = target;
    correction = shift;
}

int TuneEngine::grainHalfLength (double inputPeriod) const noexcept
{
    // Two periods long, but not longer than the delay allows without falling behind the input.
    return juce::jmax (8, juce::jmin ((int)inputPeriod, (latency - 1) / 2));
}

void TuneEngine::placeAnalysisMarks (int64_t upTo) noexcept
{
    // A mark on each period of the input, on its peak. A mark is placed once the stretch searched for
    // the peak, and a whole grain around it, have arrived.
    const auto p = (double)(voiced ? period : (float)(sampleRate / neutralHz));
    const auto reach = voiced ? (int64_t)(p * 0.25) : 0;
    const auto half = grainHalfLength (p);

    while ((int64_t)nextAnalysisMark + reach + half <= upTo)
    {
        auto mark = (int64_t)nextAnalysisMark;
        auto best = mark;
        for (auto k = mark - reach; k <= mark + reach; ++k)
            if (k >= 0 && readMono (k) > readMono (best))
                best = k;
        mark = best;

        if (newestMark >= 0 && mark <= marks[(size_t)newestMark])
            mark = marks[(size_t)newestMark] + juce::jmax<int64_t> (1, (int64_t)(p * 0.5));

        newestMark = (newestMark + 1) % maxMarks;
        marks[(size_t)newestMark] = mark;
        numMarks = juce::jmin (numMarks + 1, maxMarks);
        nextAnalysisMark = (double)mark + p;
    }
}

int64_t TuneEngine::nearestMark (double time) const noexcept
{
    auto best = (int64_t)time;
    auto distance = std::numeric_limits<double>::max();
    for (int i = 0; i < numMarks; ++i)
        if (const auto d = std::abs ((double)marks[(size_t)i] - time); d < distance)
        {
            distance = d;
            best = marks[(size_t)i];
        }
    return best;
}

void TuneEngine::placeGrains (int64_t upTo, const TuneSettings& settings) noexcept
{
    // Each grain is laid down at the last moment: when its first sample is about to be played. That
    // leaves the most input after it, so its mark (the input period it copies) is usually the one at
    // the same time; for long periods it may be the newest one before.
    const auto inputPeriod = (double)(voiced ? period : (float)(sampleRate / neutralHz));
    const auto ratio = voiced && target >= 0 ? std::pow (2.0, (double)shift / 12.0) : 1.0;
    const auto outputPeriod = juce::jmax (8.0, inputPeriod / ratio);
    const auto half = grainHalfLength (inputPeriod);
    const auto numChannels = (int)input.size();
    const auto oldestUnplayed = upTo - latency + 1;

    if ((int64_t)std::llround (nextSynthesisMark) - half < oldestUnplayed)
        nextSynthesisMark = (double)(oldestUnplayed + half); // fell behind (e.g. at the start)

    while ((int64_t)std::llround (nextSynthesisMark) - half <= oldestUnplayed)
    {
        const auto s = (int64_t)std::llround (nextSynthesisMark);
        const auto a = numMarks > 0 ? nearestMark ((double)s) : s - half;

        // With formants following the pitch, the grain is read faster or slower (resampled), never
        // reaching past the input that has arrived.
        const auto step = settings.keepFormants ? 1.0 : ratio;
        const auto available = (double)juce::jmax<int64_t> (0, juce::jmin<int64_t> (half, upTo - 1 - a));
        const auto reach = (int)juce::jmin ((double)half, step > 1.0 ? available / step : available);
        const auto edge = (float)(half + 1);

        for (int k = -reach; k <= reach; ++k)
        {
            const auto w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float)k / edge));
            const auto into = at (s + k);
            windowSum[into] += w;

            const auto position = (double)a + (double)k * step;
            const auto whole = (int64_t)std::floor (position);
            const auto fraction = (float)(position - (double)whole);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto& ring = input[(size_t)ch];
                const auto x0 = ring[at (whole)], x1 = ring[at (whole + 1)];
                output[(size_t)ch][into] += w * (x0 + fraction * (x1 - x0));
            }
        }

        nextSynthesisMark += outputPeriod;
    }
}

} // namespace spinus::dsp
