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

/** Where each moment of a clip's file sounds once Spinus Tune's notes are moved in time or stretched
    (model::TuneNote::move and stretch): a line through points (seconds in the file, seconds where it sounds),
    from the file's start to its end, which stay where they are. Each moved note is a stretch of the line from
    its start to its end; the sound between notes is stretched or squeezed to fill the rest, so the notes'
    neighbours stay in place. Where two notes touch, a little of each note's edge (up to 20 ms) is lent to
    the gap between them, so there is always some sound to stretch.
*/
struct TimeMap
{
    std::vector<std::pair<double, double>> points; // both increasing; empty when no note is moved in time

    static TimeMap of (const std::vector<model::TuneNote>&, double seconds);
    bool isIdentity() const noexcept { return points.empty(); }
    double toOutput (double source) const;
    double toSource (double output) const;
};

/** Spinus Tune, graphical mode: how far each moment of a clip is moved, and the
    clip rendered with it.

    For a tuned note (model::TuneNote), with "curve" the pitch as sung and "slow" the curve averaged over
    0.2 s (without its vibrato), the correction in semitones is
        shift + (drift - 1) * (slow - pitch) + (vibrato - 1) * (curve - slow)
    eased in and out over 15 ms at the note's edges. Outside tuned notes nothing moves. A note's volume
    (gainDb) and formant are eased in and out the same way. All of these follow the file's own time; moving
    notes in time (TimeMap) is done on top, as the file is rendered.
*/
struct TuneRender
{
    /** The correction (semitones) every `frameSeconds`, from the start of the file. */
    static std::vector<float> corrections (const PitchTrack&, const std::vector<model::TuneNote>&, double seconds);

    /** Each note's volume (dB) and formant (semitones) every `frameSeconds`, from the start of the file. */
    struct Shaping
    {
        std::vector<float> gainDb, formant;
    };
    static Shaping shaping (const std::vector<model::TuneNote>&, double frameSeconds, double seconds);

    /** Renders `source` with the corrections into `destination` (a 32-bit float WAV): Rubber Band's R3
        engine, formant-preserving, the pitch scale (and, for notes moved in time, the time ratio) updated every 256
        samples. The rendering is as long as the source. `progress` (0..1) and
        `cancel` may be used from another thread. */
    static juce::Result render (const juce::File& source, const PitchTrack&, const std::vector<model::TuneNote>&,
                                const juce::File& destination, std::atomic<float>* progress = nullptr,
                                const std::atomic<bool>* cancel = nullptr);
};

} // namespace spinus::engine
