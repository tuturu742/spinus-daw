#pragma once

#include "model/Model.h"

#include <atomic>

namespace spinus::engine
{

/** A vocal's pitch as sung (ai.v1.listen.vocal's "track"): one value per frame, in Hz, 0 where there is no
    pitch. */
struct PitchTrack
{
    double frameSeconds = 0.005;
    std::vector<float> hz;

    static PitchTrack fromJson (const juce::var& track);

    /** The pitch (MIDI, fractional) at a time, or nothing where there is none. */
    std::optional<float> pitchAt (double seconds) const;
};

/** Spinus Tune, graphical mode: how far each moment of a clip is moved, and the
    clip rendered with it.

    For a tuned note (model::TuneNote), with "curve" the pitch as sung and "slow" the curve averaged over
    0.2 s (without its vibrato), the correction in semitones is
        shift + (drift - 1) * (slow - pitch) + (vibrato - 1) * (curve - slow)
    eased in and out over 15 ms at the note's edges. Outside tuned notes nothing moves.
*/
struct TuneRender
{
    /** The correction (semitones) every `frameSeconds`, from the start of the file. */
    static std::vector<float> corrections (const PitchTrack&, const std::vector<model::TuneNote>&, double seconds);

    /** Renders `source` with the corrections into `destination` (a 32-bit float WAV): Rubber Band's R3
        engine, formant-preserving, the pitch scale updated every 256 samples. `progress` (0..1) and
        `cancel` may be used from another thread. */
    static juce::Result render (const juce::File& source, const PitchTrack&, const std::vector<model::TuneNote>&,
                                const juce::File& destination, std::atomic<float>* progress = nullptr,
                                const std::atomic<bool>* cancel = nullptr);
};

} // namespace spinus::engine
