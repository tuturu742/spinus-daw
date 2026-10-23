#pragma once

#include "model/Model.h"

#include <juce_events/juce_events.h>

namespace spinus
{

class Session;

/** "Record to Playlist" for the clip launcher: while armed and playing in song mode,
    notes when each launcher clip starts and stops playing (in song position), and finish() writes what
    played as Playlist clips, one undo step: a new lane per launcher track ("Launcher: <track>"), with a
    clip per pass of each launched clip (Playlist patterns do not loop, FL behaviour), so the jam plays
    back from the Playlist as it was heard.

    The Session finishes a recording when playback stops or recording is turned off. Message thread only. */
class LauncherRecorder final : private juce::Timer
{
public:
    explicit LauncherRecorder (Session&);
    ~LauncherRecorder() override;

    void setArmed (bool);
    bool isArmed() const noexcept { return armed; }

    /** Writes what was recorded as Playlist clips (one undo step) and starts again. Returns the number of
        clips written. */
    int finish();

    /** Looks at what plays now (the timer does this 30 times a second; tests call it). */
    void update();

    struct Played
    {
        model::ClipId clip;
        model::Ticks start = 0, end = -1; // song position; end -1 while still playing
    };
    const std::vector<Played>& getPlayed() const noexcept { return played; }

private:
    void timerCallback() override { update(); }

    Session& session;
    bool armed = false;
    std::vector<Played> played;
};

} // namespace spinus
