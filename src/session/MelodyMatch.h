#pragma once

#include "model/Model.h"

namespace spinus
{

/** Spinus Tune following a melody: a channel's notes laid over a vocal clip (shown as ghosts in Spinus Tune's
    window), and the clip's sung notes matched to them, in pitch and, if wanted, in time.

    Used by Spinus Tune's window and by coding agents (match_vocal_to_melody), so both match the same way. */
struct MelodyMatch
{
    /** A melody note, in seconds into the vocal clip's file. */
    struct Note
    {
        double start = 0.0, end = 0.0;
        int pitch = 60;

        bool operator== (const Note&) const = default;
    };

    /** The notes `channel` plays on the Playlist while the audio clip plays (muted clips and lanes left out,
        the patterns' swing included), in time order; each cut to the clip. Empty when the clip is not an audio
        clip, is reversed, or the channel plays nothing there. */
    static std::vector<Note> melodyFor (const model::Project&, model::ClipId audioClip, model::ChannelId channel);

    struct Result
    {
        std::vector<model::TuneNote> notes;
        int matched = 0;            // sung notes given a melody note
        int timed = 0;              // of those, placed in time on their melody note
        std::vector<int> unmatched; // sung notes left as they were: no melody note clearly theirs
    };

    /** Each of the sung notes `indices` (all when empty) goes to the melody note it overlaps most (or, if none,
        one starting within 150 ms of it): to its pitch, in the octave sung. With `timing`, a sung note that is
        its melody note's only one also starts and ends with it (moved and stretched, 0.25 to 4 times as long),
        as far as its neighbours and the clip allow. Drift, vibrato, volume and formant stay. */
    static Result match (std::vector<model::TuneNote> sung, const std::vector<Note>& melody, bool timing,
                         const std::vector<int>& indices, double fileSeconds);
};

} // namespace spinus
