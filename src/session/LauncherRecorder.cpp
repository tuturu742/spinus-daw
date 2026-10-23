#include "LauncherRecorder.h"

#include "Session.h"
#include "commands/CommandRegistry.h"
#include "engine/Beats.h"

namespace spinus
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

using LaunchState = engine::EngineAdapter::LaunchState;

/** Starts and stops are seen at audio block edges, a few ticks off the bar or beat they happened on:
    a position that close to a 32nd note is put on it. */
model::Ticks tidy (model::Ticks t)
{
    constexpr model::Ticks grid = model::ticksPerQuarterNote / 8, near = 16;
    const auto snapped = (t + grid / 2) / grid * grid;
    return std::abs (snapped - t) <= near ? snapped : t;
}
} // namespace

LauncherRecorder::LauncherRecorder (Session& s) : session (s) {}

LauncherRecorder::~LauncherRecorder() { stopTimer(); }

void LauncherRecorder::setArmed (bool shouldBeArmed)
{
    if (armed == shouldBeArmed)
        return;
    armed = shouldBeArmed;
    if (armed)
        startTimerHz (30);
    else
        stopTimer();
}

void LauncherRecorder::update()
{
    if (! armed || ! session.isPlaying() || session.getMode() != Session::Mode::song)
        return;

    auto& adapter = session.getAdapter();
    auto& edit = session.getEdit();
    auto toTicks = [&edit] (tracktion::BeatPosition beat)
    { return (model::Ticks)std::llround (engine::toTicks (edit, beat)); };

    for (auto& clip : session.getProject().launcher.clips)
    {
        auto open = std::find_if (played.begin(), played.end(),
                                  [&] (const Played& p) { return p.clip == clip.id && p.end < 0; });
        const auto state = adapter.getLaunchState (clip.id);
        const bool sounding = state == LaunchState::playing || state == LaunchState::stopping;
        const auto clips = adapter.getLauncherClips (clip.id);
        auto handle = clips.empty() ? nullptr : clips.front()->getLaunchHandle();
        if (handle == nullptr)
            continue;

        if (sounding && open == played.end())
        {
            if (auto range = handle->getPlayedRange())
                played.push_back ({ clip.id, toTicks (range->getStart()), -1 });
        }
        else if (! sounding && open != played.end())
        {
            // Stopped: it played for as long as its last played range says.
            if (auto last = handle->getLastPlayedRange())
                open->end = open->start + toTicks (tracktion::BeatPosition() + last->getLength());
            else
                open->end = open->start;
        }
    }
}

int LauncherRecorder::finish()
{
    update();
    auto& adapter = session.getAdapter();
    auto& edit = session.getEdit();
    // Still playing: up to now.
    for (auto& p : played)
        if (p.end < 0)
        {
            const auto clips = adapter.getLauncherClips (p.clip);
            auto handle = clips.empty() ? nullptr : clips.front()->getLaunchHandle();
            const auto range = handle != nullptr ? handle->getPlayedRange() : std::nullopt;
            p.end = range ? p.start + (model::Ticks)std::llround (
                                          engine::toTicks (edit, tracktion::BeatPosition() + range->getLength()))
                          : p.start;
        }

    // A lane per launcher track, in the launcher's order, and a Playlist clip per pass. Each command is
    // tried on a copy first, to learn the new lane's ID for its clips.
    const auto project = session.getProject();
    commands::CommandHistory trial (project);
    auto transaction = std::make_unique<commands::Transaction> ("Record the launcher to the Playlist");
    auto add = [&] (const juce::String& name, const juce::var& parameters) -> juce::var
    {
        juce::String error;
        auto onTrial = commands::CommandRegistry::get().create (name, parameters, error);
        auto real = commands::CommandRegistry::get().create (name, parameters, error);
        if (onTrial == nullptr || real == nullptr)
            return {};
        auto* command = onTrial.get();
        if (trial.perform (std::move (onTrial)).failed())
            return {};
        transaction->add (std::move (real));
        return command->getResult();
    };

    for (auto& p : played)
    {
        p.start = tidy (p.start);
        p.end = tidy (p.end);
    }

    int clipsWritten = 0;
    for (auto& track : project.launcher.tracks)
    {
        juce::var lane;
        for (auto& p : played)
        {
            const auto* clip = project.launcher.findClip (p.clip);
            if (clip == nullptr || clip->track != track.id || p.end <= p.start || clip->length <= 0)
                continue;
            if (lane.isVoid())
                lane = add ("lane.add", object ({ { "name", "Launcher: " + track.name } }))["lane"];
            if (lane.isVoid())
                break;
            const auto total = clip->loop ? p.end - p.start : juce::jmin (p.end - p.start, clip->length);
            for (model::Ticks pass = 0; pass < total; pass += clip->length)
            {
                const auto start = (juce::int64)p.start + (juce::int64)pass;
                const auto length = (juce::int64)juce::jmin (clip->length, total - pass);
                juce::var made;
                if (auto* pc = std::get_if<model::PatternClip> (&clip->content))
                    made = add ("clip.addPattern", object ({ { "lane", lane },
                                                             { "pattern", (int)pc->pattern.value },
                                                             { "start", start },
                                                             { "length", length } }));
                else
                {
                    const auto& audio = std::get<model::AudioClip> (clip->content);
                    made = add ("clip.addAudio", object ({ { "lane", lane },
                                                           { "file", audio.file },
                                                           { "channel", (int)audio.channel.value },
                                                           { "start", start },
                                                           { "length", length },
                                                           { "offset", (juce::int64)audio.offset },
                                                           { "gainDb", audio.gainDb } }));
                }
                clipsWritten += made.isVoid() ? 0 : 1;
            }
        }
    }
    played.clear();
    if (clipsWritten == 0 || transaction->size() == 0)
        return 0;
    return session.getHistory().perform (std::move (transaction)).wasOk() ? clipsWritten : 0;
}

} // namespace spinus
