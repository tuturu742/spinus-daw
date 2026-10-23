#pragma once

#include <set>

#include "DeviceFactory.h"
#include "model/Freeze.h"
#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

namespace spinus::engine
{

/** Compiles the Spinus Model into a Tracktion Edit and keeps it in step.

    sync() compares the project with the one applied last time and changes only what differs, so it
    can run after every command while the transport plays. Mapping:

      - Mixer insert   -> AudioTrack with no clips; effects in its plugin list; volume/pan/mute/solo on
                          the track; output to another insert's track. The master insert is the Edit's
                          master (master plugin list and volume).
      - Send           -> AuxSendPlugin after the fader on the source; one AuxReturnPlugin bus per target.
      - Sidechain      -> sidechain source of every effect on the insert that supports one.
      - Channel        -> AudioTrack with the instrument; output to its insert's track.
      - Every device   -> followed by an AudioGuardPlugin that silences NaN/infinite output.
      - Pattern clip   -> one MidiClip per channel with notes in the pattern; a pattern edit rewrites
                          only the clips of channels whose notes changed.
      - Notes from     -> copies of that channel's MidiClips on the insert's track, so its effects
                          receive the notes (Spinus Tune's target notes).
      - Launcher clip  -> clips in the clip slots of the channel tracks it plays on (a MidiClip per channel
                          with notes in its pattern, or a WaveAudioClip on its channel's track), at a slot
                          index kept for the clip's lifetime.

    Tracktion resolves track-to-track routing by track position, so routing is re-asserted after any
    track is deleted.

    Device state: setSavedDeviceStates() supplies states loaded from plugin-states/; each is applied
    when its device is created. captureDeviceStates() returns every device's current state for saving,
    including the saved state of devices whose plugin is missing on this computer, so it is never lost.

    Not mapped yet: audio and automation clips, channel pitch, insert stereo width,
    effect dry/wet mix.

    Message thread only.
*/
class EngineAdapter
{
public:
    EngineAdapter (tracktion::Edit&, DeviceFactory&);
    ~EngineAdapter();

    /** Makes the Edit match the project. */
    void sync (const model::Project&);

    /** FL's two play modes. Song mode plays the playlist. Pattern mode silences the
        playlist and plays one pattern on its own, looped over its length. Call sync() afterwards. */
    enum class PlayMode
    {
        song,
        pattern
    };

    void setPlayMode (PlayMode, model::PatternId currentPattern);
    PlayMode getPlayMode() const noexcept { return playMode; }

    /** The range the transport should loop: the pattern in pattern mode, empty in song mode. */
    tracktion::TimeRange getPatternLoopRange() const;

    /** A canonical text description of the Edit as Spinus set it up, read back from Tracktion objects
        (not from the model). Two adapters that synced the same project describe the same text. */
    juce::String describe() const;

    /** States to apply when devices are created (call before the first sync after loading). */
    void setSavedDeviceStates (std::map<model::DeviceId, juce::MemoryBlock>);

    /** The current state of every device in the last synced project. */
    /** A device's settings now (its plugin's parameters, 0 to 1, by Spinus's parameter ID, ParameterIds.h), e.g.
        to copy them to another device with device.setParameters. Empty when the plugin isn't loaded. */
    std::map<juce::String, float> readParameters (model::DeviceId) const;
    std::map<model::DeviceId, juce::MemoryBlock> captureDeviceStates() const;

    /** Devices whose output contained NaN/infinity since the last call; those blocks were replaced by
        silence. Poll from the message thread to flag the devices in the UI. */
    std::vector<model::DeviceId> takeSilencedDevices();

    /** Devices whose plugin runs in a separate process and stopped working there (crashed or hung) since
        the last call; each is reported once. Their slots pass audio through (effects) or are
        silent (instruments) until reloaded. */
    std::vector<model::DeviceId> takeFailedDevices();
    /** Devices whose plugin runs in a separate process and has stopped working there, until reloaded (for the
        Mixer and Channel Rack to show). */
    std::vector<model::DeviceId> getStoppedDevices() const;
    /** True if the device's plugin runs in a separate process. */
    bool runsSeparately (model::DeviceId) const;
    /** Makes these devices' plugins again, with the settings they have now (after a plugin failed, or to
        move it into or out of a separate process). */
    void reloadDevices (const std::vector<model::DeviceId>&, const model::Project&);

    /** True if the channel's instrument gets per-note fine pitch, pan and volume. */
    static bool playsNoteExpressions (const model::Project&, model::ChannelId);

    /** Devices in the project whose plugin could not be created here (shown as "Missing: <name>").
        Devices unloaded because they are frozen are not missing. */
    std::vector<model::DeviceId> getMissingDevices() const;

    /** Inserts that are processed but go nowhere: a stem export keeps the kick's insert playing this way,
        so a Ducker following it still dips, without the kick being in the stem. */
    void setUnheardInserts (std::set<model::InsertId> inserts) { unheardInserts = std::move (inserts); }

    /** A level change on the master for listening only (the automixer's A/B at matched loudness): not part
        of the project, not in exports. */
    void setMonitorTrim (float db, const model::Project&);
    float getMonitorTrim() const noexcept { return monitorTrimDb; }

    /** Frozen channels and inserts that play rendered audio instead of their plugins (model/Freeze.h):
        the file to play from the start of the song (or of the pattern, in pattern mode), or no file for
        silence. Their plugins, and those of everything playing into a frozen insert, are unloaded;
        unfreezing loads them again with their settings. Targets not listed play live. Call sync()
        afterwards. */
    void setFrozenAudio (std::map<model::FreezeTarget, juce::File>);

    /** Audio clips that play another file than their own: Spinus Tune's rendering of them. */
    void setTunedAudio (std::map<model::ClipId, juce::File> files) { tunedAudio = std::move (files); }

    /** Where audio clip files with relative paths are (the project's audio/ folder). Changing it
        reloads the audio clips at the next sync. */
    void setAudioFolder (const juce::File&);
    juce::File resolveAudioFile (const juce::String& path) const;

    /** The Tracktion parameter an automation target controls, or nullptr (device missing, unknown ID). */
    tracktion::AutomatableParameter* findParameter (const model::ParameterTarget&) const;

    /** Audio clips whose file is not there (shown in the Playlist). */
    std::vector<model::ClipId> getMissingAudioClips() const;

    tracktion::AudioTrack* getChannelTrack (model::ChannelId) const;
    tracktion::AudioTrack* getInsertTrack (model::InsertId) const; // nullptr for the master
    tracktion::Plugin* getDevicePlugin (model::DeviceId) const;
    /** The device a plugin plays, if it is one of the project's devices. */
    std::optional<model::DeviceId> findDevice (const tracktion::Plugin*) const;
    std::optional<model::DeviceId> findDevice (const tracktion::Plugin&) const;

    /** Levels after an insert's fader, for meters. The master's exist only while the Edit has a
        playback context (an audio device); nullptr otherwise. */
    tracktion::LevelMeasurer* getInsertLevels (model::InsertId) const;

    /** What a device does to the sound right now, measured around it: how far it is turning the sound down
        below its usual gain (dB, <= 0) and its share of each audio block's time (1 = all of it). Nothing for
        a device that isn't loaded. */
    struct DeviceActivity
    {
        float gainReductionDb = 0.0f;
        float load = 0.0f;
    };
    std::optional<DeviceActivity> getDeviceActivity (model::DeviceId) const;
    int getNumMidiClips() const;

    tracktion::Edit& getEdit() noexcept { return edit; }

    //==============================================================================
    // The clip launcher. What plays is transport state, not part of the project.

    /** Starts a launcher clip on the next bar, beat or at once (the project's launch quantisation) and
        stops the other clips of its launcher track at the same moment. With the transport stopped it
        starts with playback. False when the clip has nothing that can play (an empty pattern, a missing
        file, a frozen channel). */
    bool launch (model::ClipId);
    /** Launches every clip of a scene; launcher tracks with an empty slot in it stop (Ableton). */
    void launchScene (model::SceneId);
    /** Stops a launcher track's clip, on the next bar or beat like a launch. */
    void stopLauncherTrack (model::LauncherTrackId);
    /** Stops every launched clip: quantised, or at once. */
    void stopLauncher (bool now);
    /** Stops every launched clip at once, and every channel plays the Playlist again. */
    void backToPlaylist();

    enum class LaunchState
    {
        stopped,
        starting, // launched, waiting for the next bar or beat
        playing,
        stopping // stopped, playing on until the next bar or beat
    };
    LaunchState getLaunchState (model::ClipId) const;
    /** How far through its current pass a playing launcher clip is, from 0 to 1. */
    std::optional<double> getLaunchProgress (model::ClipId) const;
    /** True while some channel plays launched clips instead of its Playlist clips. */
    bool isLauncherActive() const;
    /** The Tracktion clips that play a launcher clip (tests). */
    std::vector<tracktion::Clip*> getLauncherClips (model::ClipId) const;

private:
    /** Back to the Playlist: keeps the Launcher off until its clips have really stopped. */
    struct ReturnToPlaylist final : private juce::Timer
    {
        void start (EngineAdapter&);
        using juce::Timer::stopTimer;

    private:
        void keepOff();
        void timerCallback() override;
        EngineAdapter* adapter = nullptr;
        int ticks = 0;
    };
    ReturnToPlaylist returning;

    struct ClipKey
    {
        model::ClipId clip;
        model::ChannelId channel;
        auto operator<=> (const ClipKey&) const = default;
    };

    tracktion::AudioTrack& createTrack (const juce::String& name);
    tracktion::AudioTrack& getUnheardSink();
    tracktion::PluginList& pluginListFor (model::InsertId);
    void routeTo (tracktion::AudioTrack&, model::InsertId);

    void syncSettings (const model::Project&);
    void syncInserts (const model::Project&);
    void syncInsert (const model::Insert&, const model::Project&);
    /** Places the insert's effects; returns the index in its plugin list after the last one. */
    int syncEffects (const model::Insert&);
    void syncSends (const model::Project&);
    void syncSidechains (const model::Project&);
    void syncChannels (const model::Project&);
    void syncChannel (const model::Channel&);
    void syncPatternClips (const model::Project&, bool timingChanged);
    void syncPreviewClips (const model::Project&, bool force);
    static void applyPlayback (tracktion::WaveAudioClip&, const model::AudioClip&);
    void syncAudioClips (const model::Project&, bool force);
    void syncAutomation (const model::Project&);
    void syncNoteSources (const model::Project&, bool force);
    void syncFrozenClips (const model::Project&, bool force);
    void syncLauncherClips (const model::Project&, bool force);
    std::optional<tracktion::MonotonicBeat> nextLaunchBeat() const;
    /** Rewrites a clip's notes in place. */
    void writeMidiClip (tracktion::MidiClip&, const model::Clip&, const model::PatternClip&,
                        const std::vector<model::Note>&, const model::Pattern&, bool muted, bool expressive);
    /** Writes the clip in `slot` on `track`, made when the slot is empty; one of many notes (before or after) is
        replaced by a new clip made with its notes in it. `slot` stays empty if the clip cannot be made. */
    void writeMidiClip (tracktion::MidiClip::Ptr& slot, tracktion::AudioTrack& track, const model::Clip&,
                        const model::PatternClip&, const std::vector<model::Note>&, const model::Pattern&, bool muted,
                        bool expressive);
    /** Adds the notes the clip shows to a SEQUENCE tree; true if any has per-note values. */
    bool addNotes (juce::ValueTree& sequence, const model::Clip&, const model::PatternClip&,
                   const std::vector<model::Note>&, const model::Pattern&, bool expressive) const;
    void reapplyRouting (const model::Project&);

    tracktion::Plugin::Ptr ensureDevice (const model::Device&);

    void applyParameters (tracktion::Plugin&, const model::Device&);
    void placeDevice (tracktion::PluginList&, model::DeviceId, int index);
    void dropDevice (model::DeviceId);

    tracktion::TimePosition toTime (model::Ticks) const;

    tracktion::Edit& edit;
    DeviceFactory& devices;
    std::unique_ptr<model::Project> applied;

    std::map<model::ChannelId, tracktion::AudioTrack::Ptr> channelTracks;
    std::map<model::InsertId, tracktion::AudioTrack::Ptr> insertTracks;
    std::set<model::InsertId> unheardInserts;
    std::map<model::FreezeTarget, juce::File> frozenAudio, appliedFrozenAudio;
    std::map<model::ClipId, juce::File> tunedAudio, appliedTunedAudio;
    model::FrozenParts frozen; // what frozenAudio silences in the project being synced
    std::map<model::FreezeTarget, tracktion::WaveAudioClip::Ptr> frozenClips;

    struct NoteSource // an insert's copy of a channel's notes (Insert::notesFrom)
    {
        juce::String signature; // what the clips were written from; rewritten when it changes
        std::vector<tracktion::MidiClip::Ptr> clips;
    };
    std::map<model::InsertId, NoteSource> noteSources;
    tracktion::AudioTrack::Ptr unheardSink; // where unheard inserts play into
    tracktion::Plugin::Ptr masterMute;      // MutePlugin after the Master's effects
    std::map<model::DeviceId, tracktion::Plugin::Ptr> devicePlugins;
    std::map<model::DeviceId, tracktion::Plugin::Ptr> guards; // AudioGuardPlugin right after each device
    std::map<model::DeviceId, tracktion::Plugin::Ptr> probes; // AudioGuardPlugin (probe) right before each device
    std::map<model::DeviceId, juce::MemoryBlock> savedStates; // not yet applied, or plugin missing
    std::set<model::DeviceId> reportedFailures;
    // Settings set through commands, as last applied, and what each was before.
    std::map<model::DeviceId, std::map<juce::String, float>> appliedParameters;
    float monitorTrimDb = 0.0f;
    std::map<model::DeviceId, std::map<juce::String, float>> originalParameters;
    std::map<model::InsertId, int> returnBuses; // target insert -> aux bus number
    std::map<ClipKey, tracktion::MidiClip::Ptr> midiClips;
    std::map<model::ChannelId, tracktion::MidiClip::Ptr> previewClips; // pattern mode only
    std::map<model::ClipId, tracktion::WaveAudioClip::Ptr> audioClips;
    std::map<ClipKey, tracktion::Clip::Ptr> launcherClips; // launcher clip and channel -> clip in a slot
    std::map<model::ClipId, int> launcherSlots;            // each launcher clip's slot index
    std::set<model::ClipId> missingAudioClips;
    std::vector<tracktion::AutomatableParameter::Ptr> automatedParameters; // whose curves the adapter wrote
    juce::File audioFolder;
    bool audioFolderChanged = false;

    PlayMode playMode = PlayMode::song, appliedPlayMode = PlayMode::song;
    model::PatternId currentPattern, appliedCurrentPattern;
};

} // namespace spinus::engine
