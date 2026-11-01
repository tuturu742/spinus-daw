#include <catch2/catch_test_macros.hpp>

#include "dsp/threeosc/ThreeOscProcessor.h"
#include "engine/LiveInstrument.h"
#include "engine/SamplePreview.h"

using namespace spinus;

namespace
{
/** Mixes `samples` in blocks of `blockSize` (as a sound card asks); the first sample heard, or -1, and the peak. */
std::pair<int, float> mixFor (engine::SamplePreview& preview, int samples, int blockSize)
{
    juce::AudioBuffer<float> block (2, blockSize);
    int first = -1;
    float peak = 0.0f;
    for (int done = 0; done < samples; done += blockSize)
    {
        block.clear();
        preview.mix (block.getArrayOfWritePointers(), 2, blockSize);
        for (int s = 0; s < blockSize && first < 0; ++s)
            if (std::abs (block.getSample (0, s)) > 1.0e-4f)
                first = done + s;
        peak = juce::jmax (peak, block.getMagnitude (0, blockSize));
    }
    return { first, peak };
}
} // namespace

TEST_CASE ("A live instrument plays through the preview: notes at their times, cut and stopped", "[engine][preview]")
{
    dsp::ThreeOscProcessor synth;
    engine::LiveInstrument live (synth);
    live.prepare (44100.0);
    engine::SamplePreview preview; // no sound card: mixed by hand
    preview.setLive (&live);
    CHECK (preview.getLive() == &live);

    // Silent until played (what the user does in its window, here nothing).
    CHECK (mixFor (preview, 4410, 300).first == -1);

    // A note 0.1 s in, in blocks bigger than the instrument's (a sound card's 1024): heard from then on.
    REQUIRE (live.play ({ { 60, 0.1, 0.3, 0.8f } }));
    const auto [first, peak] = mixFor (preview, 44100, 1024);
    CHECK (first >= 4410);
    CHECK (first < 4410 + 64);
    CHECK (peak > 0.05f);
    CHECK (live.takePeak() > 0.05f);
    CHECK (live.takePeak() < 1.0e-6f); // taken

    // Played again: the notes before are cut. Stopped: silent once the release has rung out.
    REQUIRE (live.play ({ { 64, 0.0, 10.0, 0.8f } }));
    CHECK (mixFor (preview, 4410, 128).second > 0.05f);
    live.stopNotes();
    mixFor (preview, 44100, 128);
    CHECK (mixFor (preview, 4410, 128).first == -1);

    // Too many notes at once are refused, not cut short.
    std::vector<engine::LiveInstrument::Note> many (engine::LiveInstrument::maxEvents, { 60, 0.0, 0.1, 0.8f });
    CHECK_FALSE (live.play (many));

    // Taken off: nothing more is played on it.
    REQUIRE (live.play ({ { 60, 0.0, 1.0, 0.8f } }));
    preview.setLive (nullptr);
    CHECK (mixFor (preview, 4410, 128).first == -1);
}
