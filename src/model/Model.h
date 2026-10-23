#pragma once

#include "Ids.h"

#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <variant>
#include <vector>

/*  The Spinus Model: the project as the user sees it, FL Studio-style.

    Plain value types: copyable, comparable, no listeners and no engine objects. All changes go through
    Commands; the Engine Adapter compiles the model into a Tracktion Edit;
    ModelJson.h reads and writes it as project.json.

    Time is in ticks at 960 PPQ. Levels are in decibels; pan is -1 (left) to +1 (right).
*/

namespace spinus::model
{

inline constexpr int64_t ticksPerQuarterNote = 960;

/** The master insert always has this ID; a new project starts with master only. */
inline constexpr InsertId masterInsertId { 1 };

using Ticks = int64_t;

//==============================================================================
struct TimeSignature
{
    int numerator = 4;
    int denominator = 4;

    bool operator== (const TimeSignature&) const = default;

    /** Beats per bar 1..32 over 1, 2, 4, 8, 16 or 32. */
    bool isValid() const noexcept
    {
        return numerator >= 1 && numerator <= 32 &&
               (denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16 ||
                denominator == 32);
    }

    /** A bar in ticks (3840 for 4/4, 2880 for 3/4, 2880 for 6/8). */
    int64_t barTicks() const noexcept { return isValid() ? (int64_t)numerator * 960 * 4 / denominator : 3840; }

    /** A beat in ticks (960 for x/4, 480 for x/8). */
    int64_t beatTicks() const noexcept { return isValid() ? (int64_t)960 * 4 / denominator : 960; }
};

/** Optional musical key, used by AI features and Spinus Tune. */
struct Key
{
    int tonic = 0;                // pitch class, 0 = C
    juce::String scale = "major"; // "major", "minor", or another named scale

    bool operator== (const Key&) const = default;
};

struct ProjectSettings
{
    double tempoBpm = 120.0;
    TimeSignature timeSignature;
    std::optional<Key> key;
    double sampleRate = 48000.0;
    juce::String title, author, comments;

    bool operator== (const ProjectSettings&) const = default;
};

//==============================================================================
/** What fills an instrument or effect slot. */
struct Device
{
    enum class Kind
    {
        stock, // Spinus-built or bundled device, identified by `stockType`
        plugin // third-party plugin, identified by `plugin`
    };

    /** Enough to find the plugin again on this or another machine (mirrors juce::PluginDescription). */
    struct PluginRef
    {
        juce::String format; // "VST3", "CLAP", "LV2", "VST", ...
        juce::String name;
        juce::String manufacturer;
        juce::String fileOrIdentifier; // path or URI at save time
        int uniqueId = 0;

        bool operator== (const PluginRef&) const = default;
    };

    DeviceId id;
    Kind kind = Kind::stock;
    juce::String stockType; // e.g. "spinus.sampler", "bundled.surge-xt", "bundled.lsp.compressor-stereo"
    PluginRef plugin;
    bool bypassed = false;

    /** Settings set through commands (device.setParameters): Spinus's parameter ID (the one
        automation uses: a Spinus device's own parameter ID, another plugin's parameter position "0", "1"...;
        engine/ParameterIds.h) to a value from 0 to 1. The engine applies them over the device's state; a setting
        taken out again gets back the value it had before. Older projects may hold the plugin's own IDs here: the
        engine still finds those (a key is read as Spinus's ID first). */
    std::map<juce::String, float> parameters;

    /** The device's binary state lives in plugin-states/<id>.bin, not in project.json. */

    bool operator== (const Device&) const = default;
};

//==============================================================================
/** A channel rack entry: one instrument (or sampler), routed to one mixer insert. */
struct Channel
{
    ChannelId id;
    juce::String name;
    juce::uint32 colour = 0xff5c8dd6;
    std::optional<Device> instrument;
    float volumeDb = 0.0f;
    float pan = 0.0f;
    float pitchCents = 0.0f;
    bool muted = false;
    bool solo = false;
    bool frozen = false; // plays its instrument's rendered audio instead of the plugin (see Freeze.h)
    /** An audio track: a channel without an instrument that audio clips (vocals, stems, long recordings)
        play through. Listed in the Audio tracks panel instead of the Channel Rack. */
    bool audioTrack = false;
    InsertId targetInsert = masterInsertId;
    /** Its group in the Channel Rack ("Drums"), which can show one group at a time (FL's filter groups); empty
        for none. */
    juce::String group;
    /** An audio track's recording (several tracks record at once, each from its own input of a sound card
        with many): the sound card input it records from, counted from 1 (0: the input chosen in Options ›
        Audio), that input and the next as a stereo pair, and whether Record audio records it. */
    int recordInput = 0;
    bool recordStereo = false;
    bool recordArmed = false;

    bool operator== (const Channel&) const = default;
};

//==============================================================================
struct Note
{
    Ticks start = 0;
    Ticks length = ticksPerQuarterNote / 4;
    int pitch = 60;        // MIDI note number 0..127
    float velocity = 0.8f; // 0..1
    float releaseVelocity = 0.5f;
    float pan = 0.0f;
    float finePitchCents = 0.0f;

    /** Per-note modulation (e.g. CLAP note expressions). The Piano Roll edits "volume" (see getVolume);
        other names are kept as they are. */
    std::map<juce::String, float> expressions;

    /** Per-note volume, 0..1 (linear gain; 1 when not set). Stored in `expressions` as "volume", the
        CLAP note expression it becomes, so the project format does not change. */
    static constexpr const char* volumeExpression = "volume";
    float getVolume() const
    {
        auto it = expressions.find (volumeExpression);
        return it != expressions.end() ? it->second : 1.0f;
    }
    void setVolume (float volume)
    {
        if (juce::approximatelyEqual (volume, 1.0f))
            expressions.erase (volumeExpression);
        else
            expressions[volumeExpression] = volume;
    }

    bool operator== (const Note&) const = default;
};

/** A pattern holds notes per channel (the step sequencer is a view of the same notes). */
struct Pattern
{
    PatternId id;
    juce::String name;
    juce::uint32 colour = 0xff6aa84f;
    Ticks length = ticksPerQuarterNote * 16; // four 4/4 bars (one bar = 3840 ticks)
    std::map<ChannelId, std::vector<Note>> notes;
    /** The pattern's own time signature (its bars in the Piano Roll and Channel Rack); none: the song's. */
    std::optional<TimeSignature> timeSignature;
    /** The Channel Rack's steps are sixteenth-note triplets (six per quarter note) instead of sixteenths. */
    bool tripletSteps = false;

    /** Swing, 0 (straight) to 1: notes on every second step play later, by up to half a step (a dotted
        feel). Only how the pattern plays: the notes themselves stay where they are. */
    float swing = 0.0f;

    /** One Channel Rack step: a sixteenth, or a sixteenth-note triplet. */
    Ticks getStepTicks() const noexcept { return ticksPerQuarterNote / (tripletSteps ? 6 : 4); }
    /** How much later a note starting at `start` plays because of the swing: notes on every second step. */
    Ticks getSwingDelay (Ticks start) const noexcept
    {
        const auto step = getStepTicks();
        if (swing <= 0.0f || step <= 0 || start % (2 * step) != step)
            return 0;
        return (Ticks)std::llround ((double)swing * (double)step / 2.0);
    }

    bool operator== (const Pattern&) const = default;
};

//==============================================================================
/** Something automation can control. */
struct ParameterTarget
{
    enum class Kind
    {
        channelVolume,
        channelPan,
        insertVolume,
        insertPan,
        deviceParameter // a parameter of an instrument or effect, by the plugin's parameter ID
    };

    Kind kind = Kind::channelVolume;
    ChannelId channel;
    InsertId insert;
    DeviceId device;
    juce::String parameterId;

    bool operator== (const ParameterTarget&) const = default;
};

struct AutomationPoint
{
    Ticks time = 0;
    float value = 0.0f; // normalized 0..1
    float curve = 0.0f; // -1..1, shape of the segment that starts here (0 = linear)

    bool operator== (const AutomationPoint&) const = default;
};

struct PatternClip
{
    PatternId pattern;
    Ticks offset = 0; // where in the pattern the clip starts playing

    bool operator== (const PatternClip&) const = default;
};

/** Spinus Tune, graphical: one sung note of an audio clip, as heard, and how it
    is corrected. Times are seconds in the clip's file. The corrected pitch curve is
        pitch + shift + drift * (slow curve - pitch) + vibrato * (curve - slow curve)
    where "curve" is the pitch as sung and "slow curve" the curve without its wobble (vibrato). */
struct TuneNote
{
    double start = 0.0, end = 0.0;
    float pitch = 60.0f;  // as sung: the note's centre (MIDI, fractional)
    float shift = 0.0f;   // semitones to move it
    float drift = 1.0f;   // 1 = the slow wandering as sung, 0 = straight on the note
    float vibrato = 1.0f; // 1 = as sung, 0 = none, up to 2 = twice as wide

    bool isUnchanged() const noexcept
    {
        return juce::exactlyEqual (shift, 0.0f) && juce::exactlyEqual (drift, 1.0f) &&
               juce::exactlyEqual (vibrato, 1.0f);
    }
    bool operator== (const TuneNote&) const = default;
};

struct AudioClip
{
    juce::String file; // relative to the project's audio/ folder, or absolute when kept external
    ChannelId channel; // plays through this channel (its volume, pan and mixer insert), as in FL
    Ticks offset = 0;
    float gainDb = 0.0f;
    /** How the audio is played: "off" as recorded; "stretch" with its pitch and speed set
        apart (Rubber Band; "stretch-drums" keeps hits sharp); "resample" faster is higher, like a record;
        "tempo-sync" follows the song's tempo (not yet: plays as "off"). */
    float pitchSemitones = 0.0f; // -24..24
    juce::String stretchMode = "tempo-sync";
    Ticks fadeIn = 0, fadeOut = 0;
    std::vector<TuneNote> tune; // Spinus Tune's notes, in time order; empty when not tuned
    double speed = 1.0;         // 0.25..4: 2 plays twice as fast (half as long)
    bool reversed = false;

    bool operator== (const AudioClip&) const = default;
};

/** How fast an audio clip's file plays: its speed, times its pitch when resampling; 1 as recorded. */
inline double getPlaybackSpeed (const AudioClip& a)
{
    if (a.stretchMode == "stretch" || a.stretchMode == "stretch-drums")
        return juce::jlimit (0.25, 4.0, a.speed);
    if (a.stretchMode == "resample")
        return juce::jlimit (0.125, 8.0, a.speed * std::pow (2.0, (double)a.pitchSemitones / 12.0));
    return 1.0;
}

struct AutomationClip
{
    ParameterTarget target;
    std::vector<AutomationPoint> points;

    bool operator== (const AutomationClip&) const = default;
};

/** One placement on the playlist. Lanes are not tied to channels (FL behaviour). */
struct Clip
{
    ClipId id;
    LaneId lane;
    Ticks start = 0;
    Ticks length = ticksPerQuarterNote * 16;
    bool muted = false;
    std::variant<PatternClip, AudioClip, AutomationClip> content;

    bool operator== (const Clip&) const = default;
};

struct Lane
{
    LaneId id;
    juce::String name;
    juce::uint32 colour = 0xff3d4450;
    bool muted = false;
    /** The track this one is grouped under (FL's playlist track groups), or invalid. Groups nest; muting or
        folding a track does the same to the tracks under it. */
    LaneId group;
    bool collapsed = false; // folded: the tracks under it are hidden

    bool operator== (const Lane&) const = default;
};

/** A named point in the song, on the Playlist's ruler ("Chorus"): to see the song's parts and jump to them. */
struct Marker
{
    MarkerId id;
    Ticks at = 0;
    juce::String name;

    bool operator== (const Marker&) const = default;
};

struct Playlist
{
    std::vector<Lane> lanes; // display order
    std::vector<Clip> clips;
    std::vector<Marker> markers; // in time order

    bool operator== (const Playlist&) const = default;
};

//==============================================================================
/** The clip launcher, Ableton-style: a grid of scenes (rows) and
    launcher tracks (columns). A filled slot holds a pattern or an audio clip; launched, it plays from the
    next bar (or beat, or at once), looped over its length, until another clip on its track starts or it
    is stopped. Launcher tracks are not tied to channels, as Playlist lanes are not: a pattern plays on its
    channels, an audio clip through its channel. What plays is transport state, not part of the project. */
enum class LaunchQuantize
{
    none,
    beat,
    bar
};

struct LauncherTrack
{
    LauncherTrackId id;
    juce::String name;
    juce::uint32 colour = 0xff3d4450;

    bool operator== (const LauncherTrack&) const = default;
};

struct Scene
{
    SceneId id;
    juce::String name;

    bool operator== (const Scene&) const = default;
};

struct LauncherClip
{
    ClipId id; // from the same counter as the Playlist's clips
    LauncherTrackId track;
    SceneId scene;
    Ticks length = ticksPerQuarterNote * 16; // one pass
    bool loop = true;                        // false: plays once, then stops
    std::variant<PatternClip, AudioClip> content;

    bool operator== (const LauncherClip&) const = default;
};

struct Launcher
{
    std::vector<LauncherTrack> tracks; // display order, left to right
    std::vector<Scene> scenes;         // display order, top to bottom
    std::vector<LauncherClip> clips;   // at most one per track and scene
    LaunchQuantize quantize = LaunchQuantize::bar;

    bool isEmpty() const noexcept { return tracks.empty() && scenes.empty(); }

    LauncherClip* findClip (ClipId);
    const LauncherClip* findClip (ClipId) const;
    const LauncherClip* findClip (LauncherTrackId, SceneId) const;

    bool operator== (const Launcher&) const = default;
};

//==============================================================================
struct Send
{
    InsertId target;
    float levelDb = 0.0f;

    bool operator== (const Send&) const = default;
};

struct EffectSlot
{
    Device device;
    float mix = 1.0f; // dry/wet

    bool operator== (const EffectSlot&) const = default;
};

/** A mixer insert: created on demand, any number, stable ID independent of position. */
struct Insert
{
    InsertId id;
    juce::String name;
    juce::uint32 colour = 0xff8a8f99;
    juce::String icon;
    std::vector<EffectSlot> effects; // in processing order; any number
    float volumeDb = 0.0f;
    float pan = 0.0f;
    float stereoWidth = 1.0f; // 0 = mono, 1 = unchanged, 2 = extra wide
    bool muted = false;
    bool solo = false;
    bool frozen = false;              // plays the rendered audio of everything into it and its effects (see Freeze.h)
    InsertId output = masterInsertId; // the master's own output is invalid (it feeds the audio device)
    std::vector<Send> sends;
    std::vector<InsertId> sidechainSources;
    /** A channel whose notes the insert's effects receive as MIDI (e.g. Spinus Tune's target notes);
        invalid for none. Not for the master. */
    ChannelId notesFrom;

    bool operator== (const Insert&) const = default;
};

struct Mixer
{
    Insert master;
    std::vector<Insert> inserts; // display order, master excluded

    bool operator== (const Mixer&) const = default;
};

//==============================================================================
struct Project
{
    static constexpr int currentSchemaVersion = 1;

    ProjectSettings settings;
    std::vector<Channel> channels; // channel rack order
    std::vector<Pattern> patterns;
    Playlist playlist;
    Mixer mixer;

    Launcher launcher;

    /** The next ID to hand out. IDs are unique across all kinds and never reused. */
    uint32_t nextId = 2;

    Project();

    template <typename IdType> IdType allocateId() { return IdType { nextId++ }; }

    //==============================================================================
    Channel* findChannel (ChannelId);
    const Channel* findChannel (ChannelId) const;
    Pattern* findPattern (PatternId);
    const Pattern* findPattern (PatternId) const;

    /** A pattern's time signature: its own, or the song's. */
    TimeSignature getTimeSignature (PatternId id) const
    {
        const auto* pattern = findPattern (id);
        return pattern != nullptr && pattern->timeSignature ? *pattern->timeSignature : settings.timeSignature;
    }

    /** How long a pattern is, as in FL Studio: one bar of its time signature, or as many whole bars as its
        notes reach. Note and time signature changes keep each pattern at this length. */
    Ticks fittedLength (const Pattern&) const;
    Lane* findLane (LaneId);
    const Lane* findLane (LaneId) const;
    /** A lane is muted when it or a track it is grouped under is muted. */
    bool isLaneMuted (LaneId) const;
    /** Hidden: a track it is grouped under is folded. */
    bool isLaneHidden (LaneId) const;
    /** How many groups deep a lane is (0: not grouped). */
    int getLaneDepth (LaneId) const;
    /** True if `ancestor` is the lane's group, or its group's group, and so on. */
    bool isLaneGroupedUnder (LaneId lane, LaneId ancestor) const;
    Clip* findClip (ClipId);
    const Clip* findClip (ClipId) const;

    /** Finds the master or any insert. */
    Insert* findInsert (InsertId);
    const Insert* findInsert (InsertId) const;

    /** Finds a channel's instrument or an effect on any insert. */
    const Device* findDevice (DeviceId) const;

    bool operator== (const Project& other) const;
};

} // namespace spinus::model
