#include "EngineAdapter.h"
#include "Beats.h"

#include "ExpressiveMidi.h"

#include "AudioGuardPlugin.h"
#include "MutePlugin.h"
#include "ParameterIds.h"
#include "model/Automation.h"

#include "PluginCrashGuard.h"
#include "plugins/SandboxList.h"
#include "plugins/sandbox/SandboxPluginFormat.h"

namespace spinus::engine
{

namespace te = tracktion;
using namespace spinus::model;

namespace
{
/** Whether two versions of a pattern play their notes at the same times (swing and its steps). */
bool samePlaying (const Pattern& a, const Pattern& b)
{
    return juce::approximatelyEqual (a.swing, b.swing) && a.tripletSteps == b.tripletSteps;
}

/** Whether two versions of a clip are the same but for being muted (and its lane, which only mutes it). */
bool sameExceptMute (const Clip& a, const Clip& b)
{
    return a.id == b.id && a.start == b.start && a.length == b.length && a.content == b.content;
}

constexpr int maxAuxBuses = 32; // Tracktion's aux bus numbers are 0-31

bool sameIdentity (const Device& a, const Device& b)
{
    return a.kind == b.kind && a.stockType == b.stockType && a.plugin == b.plugin;
}

juce::String formatDb (float v) { return juce::String (v, 2); }

juce::MemoryBlock stateOf (te::Plugin& plugin)
{
    plugin.flushPluginStateToValueTree();
    juce::MemoryOutputStream out;
    plugin.state.writeToStream (out);
    return out.getMemoryBlock();
}

/** Restores a state captured by stateOf(). Each plugin type restores its own parameters (third-party
    plugins their state chunk, Tracktion's built-ins their parameter values). */
void restoreState (te::Plugin& plugin, const juce::MemoryBlock& data)
{
    auto saved = juce::ValueTree::readFromData (data.getData(), data.getSize());
    if (saved.isValid() && saved.getProperty (te::IDs::type) == plugin.state.getProperty (te::IDs::type))
        plugin.restorePluginStateFromValueTree (saved);
}
} // namespace

EngineAdapter::EngineAdapter (te::Edit& e, DeviceFactory& f) : edit (e), devices (f)
{
    // Spinus owns every track; drop Tracktion's default ones.
    for (auto* t : te::getAudioTracks (edit))
        edit.deleteTrack (t);
}

//==============================================================================
class EngineAdapter::GestureWatcher final : private juce::AudioProcessorListener
{
public:
    GestureWatcher (EngineAdapter& a, DeviceId d, juce::AudioProcessor& p) : adapter (a), device (d), instance (&p)
    {
        instance->addListener (this);
    }

    ~GestureWatcher() override
    {
        // Only from the instance it was added to: a plugin made again (reloaded) has a new one, and the old one
        // went with its listener list.
        if (isWatching (instance))
            instance->removeListener (this);
    }

    bool isWatching (const juce::AudioProcessor* p) const
    {
        auto plugin = adapter.getDevicePlugin (device);
        auto* external = dynamic_cast<te::ExternalPlugin*> (plugin);
        return external != nullptr && external->getAudioPluginInstance() == p && p == instance;
    }

    juce::AudioProcessor* getInstance() const noexcept { return instance; }

private:
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}

    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* p, int index) override
    {
        if (auto* parameter = parameterAt (p, index))
        {
            {
                const juce::SpinLock::ScopedLockType lock (startLock);
                started[index] = parameter->getValue();
            }
            juce::MessageManager::callAsync (
                [token = std::weak_ptr<bool> (adapter.alive), a = &adapter, d = device, index]
                {
                    if (token.lock() != nullptr)
                        a->holdByHand (d, index, true);
                });
        }
    }

    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* p, int index) override
    {
        auto* parameter = parameterAt (p, index);
        if (parameter == nullptr)
            return;
        float before = 0.0f;
        {
            const juce::SpinLock::ScopedLockType lock (startLock);
            auto it = started.find (index);
            if (it == started.end())
                return;
            before = it->second;
            started.erase (it);
        }
        const auto after = parameter->getValue();
        // Plugins may end a drag on their own threads: the undo step is made on the message thread.
        juce::MessageManager::callAsync (
            [token = std::weak_ptr<bool> (adapter.alive), a = &adapter, d = device, index, before, after]
            {
                if (token.lock() == nullptr)
                    return;
                if (! juce::approximatelyEqual (before, after))
                    a->handEnded (d, index, before, after);
                a->holdByHand (d, index, false);
            });
    }

    juce::AudioProcessorParameter* parameterAt (juce::AudioProcessor* p, int index) const
    {
        if (p != instance)
            return nullptr;
        const auto& parameters = p->getParameters();
        return juce::isPositiveAndBelow (index, parameters.size()) ? parameters[index] : nullptr;
    }

    EngineAdapter& adapter;
    DeviceId device;
    juce::AudioProcessor* instance;
    juce::SpinLock startLock;
    std::map<int, float> started; // parameter index -> value when its drag started
};

EngineAdapter::~EngineAdapter() { gestureWatchers.clear(); }

void EngineAdapter::watchGestures (DeviceId id, te::Plugin& plugin)
{
    auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin);
    auto* instance = external != nullptr ? external->getAudioPluginInstance() : nullptr;
    auto& watcher = gestureWatchers[id];
    if (watcher != nullptr && watcher->getInstance() == instance)
        return;
    watcher = nullptr;
    if (instance != nullptr)
        watcher = std::make_unique<GestureWatcher> (*this, id, *instance);
}

void EngineAdapter::handEnded (DeviceId id, int index, float before, float after)
{
    auto* external = dynamic_cast<te::ExternalPlugin*> (getDevicePlugin (id));
    auto* instance = external != nullptr ? external->getAudioPluginInstance() : nullptr;
    if (instance == nullptr || ! juce::isPositiveAndBelow (index, instance->getParameters().size()) || ! onHandChange)
        return;
    const auto parameterId = getParameterId (*instance->getParameters()[index]);
    // What the project holds is what the plugin plays now: a command applying it again changes nothing.
    appliedParameters[id][parameterId] = after;
    onHandChange (id, parameterId, before, after);
}

//==============================================================================
te::AudioTrack& EngineAdapter::createTrack (const juce::String& name)
{
    auto tracks = te::getAudioTracks (edit);
    auto track =
        edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, tracks.isEmpty() ? nullptr : tracks.getLast()),
                                  nullptr, true); // with Tracktion's volume/pan and level meter
    jassert (track != nullptr);
    track->setName (name);
    return *track;
}

te::PluginList& EngineAdapter::pluginListFor (InsertId id)
{
    if (id == masterInsertId)
        return edit.getMasterPluginList();

    return insertTracks.at (id)->pluginList;
}

void EngineAdapter::routeTo (te::AudioTrack& track, InsertId insert)
{
    if (insert == masterInsertId)
        track.getOutput().setOutputToDefaultDevice (false);
    else if (auto* destination = getInsertTrack (insert))
        track.getOutput().setOutputToTrack (destination);
    else
        jassertfalse;
}

void EngineAdapter::setAudioFolder (const juce::File& folder)
{
    if (folder != audioFolder)
    {
        audioFolder = folder;
        audioFolderChanged = true;
    }
}

juce::File EngineAdapter::resolveAudioFile (const juce::String& path) const
{
    if (juce::File::isAbsolutePath (path))
        return juce::File (path);
    return audioFolder == juce::File() ? juce::File() : audioFolder.getChildFile (path);
}

std::vector<ClipId> EngineAdapter::getMissingAudioClips() const
{
    return { missingAudioClips.begin(), missingAudioClips.end() };
}

void EngineAdapter::applyPlayback (te::WaveAudioClip& wave, const AudioClip& audio)
{
    // How the clip plays: the file's own speed, or Rubber Band's stretch, or resampling. Stretched as it plays (Rubber
    // Band's real-time mode), not from a copy written in the background, so a change is heard at once; about 4% of
    // the audio time per stretched clip. A reversed clip plays from a reversed copy Tracktion writes first.
    wave.setAutoTempo (false);
    wave.setUsesProxy (false);
    // Only when it changes: Tracktion turns the clip's offset around (to the same audio, backwards) whenever its
    // reversed setting is written, even "not reversed" on a new clip, which made every clip that starts later in
    // its file or ends before the file does play the wrong part of it.
    if (wave.getIsReversed() != audio.reversed)
        wave.setIsReversed (audio.reversed);
    const auto speed = juce::jlimit (0.25, 4.0, audio.speed);
    const auto pitch = juce::jlimit (-24.0f, 24.0f, audio.pitchSemitones);
    if (audio.stretchMode == "stretch" || audio.stretchMode == "stretch-drums")
    {
        wave.setTimeStretchMode (audio.stretchMode == "stretch" ? te::TimeStretcher::rubberbandMelodic
                                                                : te::TimeStretcher::rubberbandPercussive);
        wave.setSpeedRatio (speed);
        wave.setPitchChange (pitch);
    }
    else if (audio.stretchMode == "resample")
    {
        // Like a record: higher is faster.
        wave.setTimeStretchMode (te::TimeStretcher::disabled);
        wave.setPitchChange (0.0f);
        wave.setSpeedRatio (juce::jlimit (0.125, 8.0, speed * std::pow (2.0, (double)pitch / 12.0)));
    }
    else
    {
        wave.setTimeStretchMode (te::TimeStretcher::disabled);
        wave.setPitchChange (0.0f);
        wave.setSpeedRatio (1.0);
    }
}

void EngineAdapter::syncAudioClips (const Project& project, bool force)
{
    std::set<ClipId> wanted;
    missingAudioClips.clear();

    for (auto& clip : project.playlist.clips)
    {
        auto* audio = std::get_if<AudioClip> (&clip.content);
        if (audio == nullptr || ! channelTracks.contains (audio->channel) || frozen.channels.contains (audio->channel))
            continue;

        const auto tuned = tunedAudio.find (clip.id);
        const auto file = tuned != tunedAudio.end() ? tuned->second : resolveAudioFile (audio->file);
        if (! file.existsAsFile())
        {
            missingAudioClips.insert (clip.id);
            continue;
        }
        wanted.insert (clip.id);

        const auto* lane = project.findLane (clip.lane);
        const bool muted =
            clip.muted || (lane != nullptr && project.isLaneMuted (lane->id)) || playMode == PlayMode::pattern;

        // Rewrite only when the clip, its lane's mute or its channel changed.
        auto existing = audioClips.find (clip.id);
        if (existing != audioClips.end() && ! force && applied != nullptr)
        {
            const auto* old = applied->findClip (clip.id);
            const auto* oldLane = applied->findLane (clip.lane);
            const bool oldMuted =
                old != nullptr && (old->muted || (oldLane != nullptr && applied->isLaneMuted (oldLane->id)) ||
                                   appliedPlayMode == PlayMode::pattern);
            const auto wasTuned = appliedTunedAudio.find (clip.id);
            const auto oldFile = wasTuned != appliedTunedAudio.end() ? wasTuned->second : juce::File();
            const auto newFile = tuned != tunedAudio.end() ? tuned->second : juce::File();
            if (old != nullptr && *old == clip && oldMuted == muted && oldFile == newFile)
                continue;
        }

        if (existing != audioClips.end())
        {
            existing->second->removeFromParent();
            audioClips.erase (existing);
        }

        auto& track = *channelTracks.at (audio->channel);
        const te::ClipPosition position { { toTime (clip.start), toTime (clip.start + clip.length) },
                                          toTime (audio->offset) - te::TimePosition() };
        auto wave = track.insertWaveClip (file.getFileNameWithoutExtension(), file, position, false);
        if (wave == nullptr)
            continue;

        applyPlayback (*wave, *audio);
        wave->setGainDB (audio->gainDb);
        wave->setFadeIn (toTime (audio->fadeIn) - te::TimePosition());
        wave->setFadeOut (toTime (audio->fadeOut) - te::TimePosition());
        wave->setMuted (muted);
        audioClips[clip.id] = wave;
    }

    for (auto it = audioClips.begin(); it != audioClips.end();)
    {
        if (wanted.contains (it->first))
        {
            ++it;
            continue;
        }
        it->second->removeFromParent();
        it = audioClips.erase (it);
    }
    appliedTunedAudio = tunedAudio;
}

void EngineAdapter::setFrozenAudio (std::map<FreezeTarget, juce::File> audio) { frozenAudio = std::move (audio); }

void EngineAdapter::syncFrozenClips (const Project& project, bool force)
{
    const auto* pattern = playMode == PlayMode::pattern ? project.findPattern (currentPattern) : nullptr;

    std::map<FreezeTarget, juce::File> wanted;
    for (auto& [target, file] : frozenAudio)
    {
        const bool silenced = std::holds_alternative<ChannelId> (target)
                                  ? frozen.channels.contains (std::get<ChannelId> (target))
                                  : frozen.inserts.contains (std::get<InsertId> (target));
        if (silenced && file.existsAsFile() && (playMode == PlayMode::song || pattern != nullptr))
            wanted[target] = file;
    }

    for (auto it = frozenClips.begin(); it != frozenClips.end();)
    {
        auto w = wanted.find (it->first);
        const bool same = w != wanted.end() && ! force && appliedFrozenAudio.contains (it->first) &&
                          appliedFrozenAudio.at (it->first) == w->second;
        if (same)
        {
            wanted.erase (w);
            ++it;
            continue;
        }

        it->second->removeFromParent();
        it = frozenClips.erase (it);
    }

    for (auto& [target, file] : wanted)
    {
        auto* track = std::holds_alternative<ChannelId> (target) ? getChannelTrack (std::get<ChannelId> (target))
                                                                 : getInsertTrack (std::get<InsertId> (target));
        if (track == nullptr)
            continue;

        // From the start: the song, or the pattern that pattern mode loops over (its tail is cut there).
        const auto end = pattern != nullptr
                             ? toTime (pattern->length)
                             : te::TimePosition::fromSeconds (te::AudioFile (edit.engine, file).getLength());
        const te::ClipPosition position { { te::TimePosition(), end }, te::TimeDuration() };
        auto wave = track->insertWaveClip ("Frozen", file, position, false);
        if (wave == nullptr)
            continue;
        wave->setAutoTempo (false);
        frozenClips[target] = wave;
    }

    appliedFrozenAudio = frozenAudio;
}

te::AutomatableParameter* EngineAdapter::findParameter (const ParameterTarget& target) const
{
    using K = ParameterTarget::Kind;
    auto volumeOf = [] (te::AudioTrack* track, bool pan) -> te::AutomatableParameter*
    {
        auto* plugin = track != nullptr ? track->getVolumePlugin() : nullptr;
        if (plugin == nullptr)
            return nullptr;
        return pan ? plugin->panParam.get() : plugin->volParam.get();
    };

    switch (target.kind)
    {
    case K::channelVolume:
    case K::channelPan:
        return volumeOf (getChannelTrack (target.channel), target.kind == K::channelPan);
    case K::insertVolume:
    case K::insertPan:
        if (target.insert == masterInsertId)
        {
            auto plugin = edit.getMasterVolumePlugin();
            if (plugin == nullptr)
                return nullptr;
            return target.kind == K::insertPan ? plugin->panParam.get() : plugin->volParam.get();
        }
        return volumeOf (getInsertTrack (target.insert), target.kind == K::insertPan);
    case K::deviceParameter:
        if (auto it = devicePlugins.find (target.device); it != devicePlugins.end())
            return it->second->getAutomatableParameterByID (target.parameterId).get();
        return nullptr;
    }
    return nullptr;
}

void EngineAdapter::syncAutomation (const Project& project)
{
    // Rebuilt from the model after every change: automation curves are small. Each automated
    // parameter gets one curve made from all its clips; between them the value holds (as in FL). Before the first
    // and after the last, the curve holds the knob's own value, so a setting can still be turned by hand there.
    for (auto& a : automated)
        a.parameter->getCurve().clear (nullptr);
    automated.clear();
    knobFollower.follow (*this);

    if (playMode == PlayMode::pattern)
        return; // the playlist is silent in pattern mode, its automation too

    std::map<te::AutomatableParameter*, std::vector<const Clip*>> clipsOf;
    for (auto& clip : project.playlist.clips)
    {
        auto* automation = std::get_if<AutomationClip> (&clip.content);
        const auto* lane = project.findLane (clip.lane);
        if (automation == nullptr || automation->points.empty() || clip.muted ||
            (lane != nullptr && project.isLaneMuted (lane->id)))
            continue;
        if (auto* parameter = findParameter (automation->target))
            clipsOf[parameter].push_back (&clip);
    }

    for (auto& [parameter, clips] : clipsOf)
    {
        // By start; of two that start together, the later one in the Playlist counts as the later.
        std::stable_sort (clips.begin(), clips.end(), [] (auto* a, auto* b) { return a->start < b->start; });
        Automated a { parameter, {}, 0.0f };
        for (auto* clip : clips)
            a.clips.push_back ({ clip->start, clip->length, std::get<AutomationClip> (clip->content).points });
        if (a.clips.empty())
            continue;
        writeCurve (a);
        automated.push_back (std::move (a));
    }
    knobFollower.follow (*this);
}

void EngineAdapter::writeCurve (Automated& a) const
{
    auto& curve = a.parameter->getCurve();
    const auto& range = a.parameter->valueRange;
    a.knob = a.parameter->getCurrentExplicitValue(); // what was set by hand or command, never by automation
    curve.clear (nullptr);

    // Which clip plays when. Where clips overlap, the one that starts later plays until it ends; then the one
    // under it goes on from where its line is by then.
    struct Piece
    {
        Ticks from, to;
        const Automated::Span* clip;
    };
    std::vector<Ticks> edges;
    for (auto& clip : a.clips)
    {
        edges.push_back (clip.start);
        edges.push_back (clip.start + clip.length);
    }
    std::sort (edges.begin(), edges.end());
    edges.erase (std::unique (edges.begin(), edges.end()), edges.end());
    std::vector<Piece> pieces;
    for (size_t i = 0; i + 1 < edges.size(); ++i)
    {
        const Automated::Span* playing = nullptr;
        for (auto& clip : a.clips) // by start: the last one that covers the edge started last
            if (clip.start <= edges[i] && edges[i] < clip.start + clip.length)
                playing = &clip;
        if (playing == nullptr)
            continue; // between clips
        if (! pieces.empty() && pieces.back().clip == playing && pieces.back().to == edges[i])
            pieces.back().to = edges[i + 1];
        else
            pieces.push_back ({ edges[i], edges[i + 1], playing });
    }

    // A clip's line, as the Playlist and the Automation editor draw it: flat before its first point and after
    // its last, and through points past its end up to its end.
    auto valueAt = [&range] (const Automated::Span& clip, Ticks time)
    { return range.convertFrom0to1 (model::automationValueAt (clip.points, (double)(time - clip.start))); };

    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const auto& piece = pieces[i];
        const auto& clip = *piece.clip;
        if (i == 0 && piece.from > 0)
            curve.addPoint (toTime (piece.from - 1), a.knob, 0.0f, nullptr); // the knob up to the first clip
        else if (i > 0 && pieces[i - 1].to < piece.from - 1)
            curve.addPoint (toTime (piece.from - 1), valueAt (*pieces[i - 1].clip, pieces[i - 1].to), 0.0f,
                            nullptr); // FL: between clips the value holds

        // Where the piece starts, with the bend of the clip's line there; then the clip's points inside it.
        const auto from = piece.from - clip.start, to = piece.to - clip.start;
        float bend = 0.0f;
        for (auto& point : clip.points)
            if (point.time <= from)
                bend = point.curve;
        curve.addPoint (toTime (piece.from), valueAt (clip, piece.from), juce::jlimit (-1.0f, 1.0f, bend), nullptr);
        for (auto& point : clip.points)
            if (point.time > from && point.time < to)
                curve.addPoint (toTime (clip.start + point.time), range.convertFrom0to1 (point.value),
                                juce::jlimit (-1.0f, 1.0f, point.curve), nullptr);
        // Its end: just before the next piece if that follows at once.
        const bool next = i + 1 < pieces.size() && pieces[i + 1].from == piece.to;
        curve.addPoint (toTime (next ? piece.to - 1 : piece.to), valueAt (clip, next ? piece.to - 1 : piece.to), 0.0f,
                        nullptr);
    }
    if (! pieces.empty())
        curve.addPoint (toTime (pieces.back().to + 1), a.knob, 0.0f, nullptr); // the knob after the last clip
}

void EngineAdapter::KnobFollower::follow (EngineAdapter& a)
{
    adapter = &a;
    if (a.automated.empty())
        stopTimer();
    else if (! isTimerRunning())
        startTimerHz (30);
}

void EngineAdapter::KnobFollower::timerCallback()
{
    for (auto& a : adapter->automated)
        if (! juce::approximatelyEqual (a.parameter->getCurrentExplicitValue(), a.knob))
            adapter->writeCurve (a);
}

void EngineAdapter::holdByHand (DeviceId id, int index, bool held)
{
    auto* plugin = getDevicePlugin (id);
    auto* external = dynamic_cast<te::ExternalPlugin*> (plugin);
    auto* instance = external != nullptr ? external->getAudioPluginInstance() : nullptr;
    if (instance == nullptr || ! juce::isPositiveAndBelow (index, instance->getParameters().size()))
        return;
    auto* own = instance->getParameters()[index];
    auto parameter = plugin->getAutomatableParameterByID (getParameterId (*own));
    auto it = std::find_if (automated.begin(), automated.end(),
                            [&] (const Automated& a) { return a.parameter == parameter; });
    if (it == automated.end())
        return;
    if (! held)
    {
        // Where the hand left it is the knob's value now (the plugin's own word for it may still be on its way).
        parameter->setParameter (parameter->valueRange.convertFrom0to1 (own->getValue()), juce::dontSendNotification);
        writeCurve (*it);
    }
    parameter->getCurve().bypass.setValue (held, nullptr);
}

te::TimePosition EngineAdapter::toTime (Ticks ticks) const { return ticksToTime (edit, (double)ticks); }

te::AudioTrack* EngineAdapter::getChannelTrack (ChannelId id) const
{
    auto it = channelTracks.find (id);
    return it != channelTracks.end() ? it->second.get() : nullptr;
}

te::AudioTrack* EngineAdapter::getInsertTrack (InsertId id) const
{
    auto it = insertTracks.find (id);
    return it != insertTracks.end() ? it->second.get() : nullptr;
}

std::optional<DeviceId> EngineAdapter::findDevice (const te::Plugin* plugin) const
{
    for (auto& [id, p] : devicePlugins)
        if (p.get() == plugin)
            return id;
    return std::nullopt;
}

te::Plugin* EngineAdapter::getDevicePlugin (DeviceId id) const
{
    auto it = devicePlugins.find (id);
    return it != devicePlugins.end() ? it->second.get() : nullptr;
}

std::optional<DeviceId> EngineAdapter::findDevice (const te::Plugin& plugin) const
{
    for (auto& [id, p] : devicePlugins)
        if (p.get() == &plugin)
            return id;
    return std::nullopt;
}

std::optional<EngineAdapter::DeviceActivity> EngineAdapter::getDeviceActivity (DeviceId id) const
{
    const auto guard = guards.find (id);
    const auto* meter =
        guard == guards.end() ? nullptr : dynamic_cast<const AudioGuardPlugin&> (*guard->second).getMeter();
    if (meter == nullptr)
        return std::nullopt;
    return DeviceActivity { .gainReductionDb = meter->gainReductionDb.load (std::memory_order_relaxed),
                            .load = meter->load.load (std::memory_order_relaxed) };
}

te::LevelMeasurer* EngineAdapter::getInsertLevels (InsertId id) const
{
    if (id == masterInsertId)
    {
        auto* context = edit.getCurrentPlaybackContext();
        return context != nullptr ? &context->masterLevels : nullptr;
    }

    if (auto* track = getInsertTrack (id))
        if (auto* meter = track->getLevelMeterPlugin())
            return &meter->measurer;
    return nullptr;
}

int EngineAdapter::getNumMidiClips() const { return (int)midiClips.size(); }

void EngineAdapter::setSavedDeviceStates (std::map<DeviceId, juce::MemoryBlock> states)
{
    savedStates = std::move (states);
}

std::map<DeviceId, juce::MemoryBlock> EngineAdapter::captureDeviceStates() const
{
    std::map<DeviceId, juce::MemoryBlock> result;
    if (applied == nullptr)
        return result;

    auto add = [&] (const Device& device)
    {
        if (auto it = devicePlugins.find (device.id); it != devicePlugins.end())
            result[device.id] = stateOf (*it->second);
        else if (auto saved = savedStates.find (device.id); saved != savedStates.end())
            result[device.id] = saved->second; // plugin missing here: keep what was loaded
    };

    for (auto& channel : applied->channels)
        if (channel.instrument)
            add (*channel.instrument);

    for (auto* insert : { &applied->mixer.master })
        for (auto& slot : insert->effects)
            add (slot.device);

    for (auto& insert : applied->mixer.inserts)
        for (auto& slot : insert.effects)
            add (slot.device);

    return result;
}

//==============================================================================
void EngineAdapter::sync (const Project& project)
{
    const bool timingChanged = applied == nullptr ||
                               ! (applied->settings.timeSignature == project.settings.timeSignature) ||
                               ! juce::exactlyEqual (applied->settings.tempoBpm, project.settings.tempoBpm);

    // What frozen audio silences: only targets that still exist play frozen.
    std::vector<FreezeTarget> targets;
    for (auto& [target, file] : frozenAudio)
    {
        const bool exists = std::holds_alternative<ChannelId> (target)
                                ? project.findChannel (std::get<ChannelId> (target)) != nullptr
                                : project.findInsert (std::get<InsertId> (target)) != nullptr &&
                                      std::get<InsertId> (target) != masterInsertId;
        if (exists)
            targets.push_back (target);
    }
    frozen = getFrozenParts (project, targets);

    syncSettings (project);
    syncInserts (project);
    syncChannels (project);
    syncSends (project);
    syncSidechains (project);
    reapplyRouting (project);
    // The Playlist's clips only follow the play mode (pattern mode mutes them), which they check clip by clip;
    // a new current pattern changes only what pattern mode plays. Rewriting every clip of the song each time a
    // pattern was opened froze Spinus for seconds with many instruments.
    const bool modeChanged = playMode != appliedPlayMode || currentPattern != appliedCurrentPattern;
    syncPatternClips (project, timingChanged);
    syncPreviewClips (project, timingChanged || modeChanged);
    syncNoteSources (project, timingChanged || modeChanged);
    syncAudioClips (project, timingChanged || audioFolderChanged);
    syncFrozenClips (project, timingChanged || modeChanged);
    syncLauncherClips (project, timingChanged);
    audioFolderChanged = false;
    syncAutomation (project);
    appliedPlayMode = playMode;
    appliedCurrentPattern = currentPattern;

    // Tracktion recomputes "muted because the destination is muted" only when a mute or solo flag
    // changes, not when routing changes, so a channel moved away from a muted insert would stay silent.
    edit.updateMuteSoloStatuses();

    applied = std::make_unique<Project> (project);
}

void EngineAdapter::syncSettings (const Project& project)
{
    auto& s = project.settings;
    edit.tempoSequence.getTempo (0)->setBpm (s.tempoBpm);

    auto* timeSig = edit.tempoSequence.getTimeSig (0);
    timeSig->numerator = s.timeSignature.numerator;
    timeSig->denominator = s.timeSignature.denominator;
}

//==============================================================================
te::Plugin::Ptr EngineAdapter::ensureDevice (const Device& device)
{
    // Callers drop devices that changed identity first, so an existing plugin is the right one.
    auto plugin = getDevicePlugin (device.id);
    if (plugin == nullptr)
    {
        // A third-party plugin's libraries are noted, so a crash in them names it.
        if (onCreatingDevice)
            onCreatingDevice (device);
        const PluginCrashGuard::ModuleWatch watch;
        auto created = devices.create (edit, device);
        if (created == nullptr)
            return nullptr; // missing: the model keeps the device; the slot stays empty
        if (device.kind == Device::Kind::plugin)
            watch.addNewModules (plugins::SandboxList::keyOf (device.plugin.format, device.plugin.fileOrIdentifier),
                                 device.plugin.name, device.plugin.fileOrIdentifier);

        devicePlugins[device.id] = created;
        guards[device.id] = edit.getPluginCache().createNewPlugin (AudioGuardPlugin::xmlTypeName, {});
        probes[device.id] = edit.getPluginCache().createNewPlugin (AudioGuardPlugin::xmlTypeName, {});
        auto meter = std::make_shared<DeviceMeter>();
        if (auto* guard = dynamic_cast<AudioGuardPlugin*> (guards[device.id].get()))
            guard->setMeter (meter, false);
        if (auto* probe = dynamic_cast<AudioGuardPlugin*> (probes[device.id].get()))
            probe->setMeter (meter, true);
        plugin = created.get();

        if (auto saved = savedStates.find (device.id); saved != savedStates.end())
        {
            restoreState (*plugin, saved->second);
            savedStates.erase (saved);
        }
    }

    if (plugin->isEnabled() == device.bypassed)
        plugin->setEnabled (! device.bypassed);

    applyPresetState (*plugin, device);
    applyParameters (*plugin, device);
    watchGestures (device.id, *plugin);
    return plugin;
}

void EngineAdapter::applyPresetState (te::Plugin& plugin, const Device& device)
{
    auto& lastLoaded = appliedPresetStates[device.id];
    if (lastLoaded == device.presetState)
        return;
    auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin);
    auto* instance = external != nullptr ? external->getAudioPluginInstance() : nullptr;
    if (instance == nullptr)
        return;
    // The plugin's own state (as it gives it to a host), not Tracktion's.
    if (lastLoaded.isEmpty())
        instance->getStateInformation (statesBeforePresets[device.id]); // what an undo of every preset gives back
    const auto& next = device.presetState.isEmpty() ? statesBeforePresets[device.id] : device.presetState;
    if (! next.isEmpty())
    {
        instance->setStateInformation (next.getData(), (int)next.getSize());
        external->flushPluginStateToValueTree();
    }
    if (device.presetState.isEmpty())
        statesBeforePresets.erase (device.id);
    lastLoaded = device.presetState;
}

float EngineAdapter::masterFaderDb (float volumeDb) const noexcept
{
    // All the way down stays silence: a raised monitor level would otherwise lift -100 dB (Tracktion's silence)
    // back to a faint sound.
    return volumeDb <= silentDb ? silentDb : volumeDb + monitorTrimDb;
}

void EngineAdapter::setMonitorTrim (float db, const Project& project)
{
    monitorTrimDb = db;
    edit.getMasterVolumePlugin()->setVolumeDb (masterFaderDb (project.mixer.master.volumeDb));
}

std::map<juce::String, float> EngineAdapter::readParameters (DeviceId id) const
{
    auto* external = dynamic_cast<te::ExternalPlugin*> (getDevicePlugin (id));
    auto* instance = external != nullptr ? external->getAudioPluginInstance() : nullptr;
    if (instance == nullptr)
        return {};
    return readParameterValues (*instance);
}

void EngineAdapter::applyParameters (te::Plugin& plugin, const Device& device)
{
    // Settings set through commands: only the ones that changed since the last sync, so a knob the
    // user turned in the plugin's window stays where it is until a command sets it again.
    auto& lastApplied = appliedParameters[device.id];
    if (lastApplied == device.parameters)
        return;
    auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin);
    auto* instance = external != nullptr ? external->getAudioPluginInstance() : nullptr;
    if (instance == nullptr)
        return;
    // By Spinus's parameter IDs (ParameterIds.h), as automation finds them. A key an older version saved by the
    // plugin's own ID is still found, unless a key by Spinus's ID names the same setting: that one counts.
    const ParameterFinder finder (*instance);
    auto byParameter = [&finder] (const std::map<juce::String, float>& values)
    {
        std::map<juce::AudioProcessorParameter*, std::pair<bool, float>> found; // legacy?, value
        for (auto& [key, value] : values)
        {
            bool legacy = false;
            if (auto* parameter = finder.findSaved (key, &legacy))
                if (auto it = found.find (parameter); it == found.end() || (it->second.first && ! legacy))
                    found[parameter] = { legacy, value };
        }
        return found;
    };
    const auto now = byParameter (device.parameters);
    const auto before = byParameter (lastApplied);
    auto& originals = originalParameters[device.id]; // by Spinus's ID
    std::vector<std::pair<juce::AudioProcessorParameter*, float>> changed;
    for (auto& [parameter, value] : now)
        if (auto it = before.find (parameter);
            it == before.end() || ! juce::approximatelyEqual (it->second.second, value.second))
        {
            originals.try_emplace (getParameterId (*parameter), parameter->getValue()); // before any command set it
            changed.emplace_back (parameter, value.second);
        }
    for (auto& [parameter, value] : before)
        if (! now.contains (parameter))
            if (auto original = originals.find (getParameterId (*parameter)); original != originals.end())
            {
                changed.emplace_back (parameter, original->second);
                originals.erase (original);
            }
    // As one change: a plugin that puts its other settings back when one changes (an effect's type) still ends
    // with every one of them as set, while it is not being played too (ParameterIds.h).
    setParameterValues (*instance, changed);
    lastApplied = device.parameters;
}

void EngineAdapter::placeDevice (te::PluginList& list, DeviceId id, int index)
{
    auto place = [&] (const te::Plugin::Ptr& plugin, int at)
    {
        if (list.indexOf (plugin.get()) == at)
            return;

        // Tracktion closes a plugin's window when the plugin moves; reopen it where it was.
        const bool windowWasOpen = plugin->windowState->isWindowShowing();
        if (list.indexOf (plugin.get()) >= 0)
            plugin->removeFromParent();
        list.insertPlugin (plugin, at, nullptr);
        if (windowWasOpen)
            plugin->showWindowExplicitly();
    };

    place (probes.at (id), index);
    place (devicePlugins.at (id), index + 1);
    place (guards.at (id), index + 2);
}

namespace
{
const plugins::sandbox::SandboxedInstance* sandboxedOf (te::Plugin& plugin)
{
    auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin);
    return external != nullptr
               ? dynamic_cast<const plugins::sandbox::SandboxedInstance*> (external->getAudioPluginInstance())
               : nullptr;
}
} // namespace

std::vector<DeviceId> EngineAdapter::takeFailedDevices()
{
    std::vector<DeviceId> failed;
    for (auto& [id, plugin] : devicePlugins)
        if (auto* sandboxed = sandboxedOf (*plugin);
            sandboxed != nullptr && sandboxed->getFailure() != plugins::sandbox::SandboxedInstance::Failure::none &&
            reportedFailures.insert (id).second)
            failed.push_back (id);
    return failed;
}

std::vector<DeviceId> EngineAdapter::getStoppedDevices() const
{
    std::vector<DeviceId> stopped;
    for (auto& [id, plugin] : devicePlugins)
        if (auto* sandboxed = sandboxedOf (*plugin);
            sandboxed != nullptr && sandboxed->getFailure() != plugins::sandbox::SandboxedInstance::Failure::none)
            stopped.push_back (id);
    std::sort (stopped.begin(), stopped.end());
    return stopped;
}

bool EngineAdapter::runsSeparately (DeviceId id) const
{
    auto* plugin = getDevicePlugin (id);
    return plugin != nullptr && sandboxedOf (*plugin) != nullptr;
}

void EngineAdapter::reloadDevices (const std::vector<DeviceId>& ids, const Project& project)
{
    // Dropping keeps each one's state (a failed plugin's is the last it had); the sync makes them again.
    for (auto id : ids)
    {
        dropDevice (id);
        reportedFailures.erase (id);
    }
    sync (project);
}

std::vector<DeviceId> EngineAdapter::takeSilencedDevices()
{
    std::vector<DeviceId> result;
    for (auto& [id, guard] : guards)
        if (auto* g = dynamic_cast<AudioGuardPlugin*> (guard.get()); g != nullptr && g->takeSilencedBlockCount() > 0)
            result.push_back (id);
    return result;
}

std::vector<DeviceId> EngineAdapter::getMissingDevices() const
{
    std::vector<DeviceId> result;
    if (applied == nullptr)
        return result;

    auto check = [&] (const Device& d)
    {
        if (! devicePlugins.contains (d.id) && ! frozen.devices.contains (d.id))
            result.push_back (d.id);
    };

    for (auto& channel : applied->channels)
        if (channel.instrument)
            check (*channel.instrument);

    for (auto& slot : applied->mixer.master.effects)
        check (slot.device);

    for (auto& insert : applied->mixer.inserts)
        for (auto& slot : insert.effects)
            check (slot.device);

    return result;
}

void EngineAdapter::dropDevice (DeviceId id)
{
    if (auto it = devicePlugins.find (id); it != devicePlugins.end())
    {
        // Keep the state: if an undo brings the device back, it returns with its settings.
        savedStates[id] = stateOf (*it->second);
        gestureWatchers.erase (id);
        appliedParameters.erase (id); // a device brought back gets its command settings again
        it->second->deleteFromParent();
        devicePlugins.erase (it);

        if (auto guard = guards.find (id); guard != guards.end())
        {
            guard->second->deleteFromParent();
            guards.erase (guard);
        }
        if (auto probe = probes.find (id); probe != probes.end())
        {
            probe->second->deleteFromParent();
            probes.erase (probe);
        }
    }
}

//==============================================================================
void EngineAdapter::syncInserts (const Project& project)
{
    // Removed inserts: their effects and tracks go. Sends to them are dropped in syncSends().
    if (applied != nullptr)
    {
        for (auto& old : applied->mixer.inserts)
        {
            if (project.findInsert (old.id) != nullptr)
                continue;

            for (auto& slot : old.effects)
                dropDevice (slot.device.id);

            returnBuses.erase (old.id);
            frozenClips.erase (old.id);
            noteSources.erase (old.id);
            edit.deleteTrack (insertTracks.at (old.id).get());
            insertTracks.erase (old.id);
        }
    }

    for (auto& insert : project.mixer.inserts)
        if (! insertTracks.contains (insert.id))
            insertTracks[insert.id] = &createTrack (insert.name);

    syncInsert (project.mixer.master, project);
    for (auto& insert : project.mixer.inserts)
        syncInsert (insert, project);
}

void EngineAdapter::syncInsert (const Insert& insert, const Project& project)
{
    // Devices that left the project, or changed identity in place, are dropped first. An effect moved to another
    // insert is kept: syncing that insert moves it there, or has already (the master and the inserts before this
    // one are synced first), so dropping it here would take it out of the insert it now plays on.
    if (applied != nullptr)
        if (auto* old = applied->findInsert (insert.id))
            for (auto& oldSlot : old->effects)
            {
                const auto* now = project.findDevice (oldSlot.device.id);
                if (now == nullptr || ! sameIdentity (*now, oldSlot.device))
                    dropDevice (oldSlot.device.id);
            }

    const auto afterEffects = syncEffects (insert);

    if (insert.id == masterInsertId)
    {
        auto volume = edit.getMasterVolumePlugin();
        volume->setVolumeDb (masterFaderDb (insert.volumeDb));
        volume->setPan (insert.pan);

        // The Master is no Tracktion track, so it has no track mute: a MutePlugin after its effects silences it
        // (playback, offline export and export while playing, whose capture sits after it).
        auto& list = edit.getMasterPluginList();
        if (masterMute == nullptr)
            masterMute = edit.getPluginCache().createNewPlugin (MutePlugin::xmlTypeName, {});
        if (masterMute != nullptr && list.indexOf (masterMute.get()) != afterEffects)
        {
            if (list.indexOf (masterMute.get()) >= 0)
                masterMute->removeFromParent();
            list.insertPlugin (masterMute, afterEffects, nullptr);
        }
        if (auto* mute = dynamic_cast<MutePlugin*> (masterMute.get()))
            mute->setMuted (insert.muted);
        return;
    }

    auto& track = *insertTracks.at (insert.id);
    if (track.getName() != insert.name)
        track.setName (insert.name);

    if (auto* volume = track.getVolumePlugin())
    {
        volume->setVolumeDb (insert.volumeDb);
        volume->setPan (insert.pan);
    }

    track.setMute (insert.muted);
    track.setSolo (insert.solo);
}

int EngineAdapter::syncEffects (const Insert& insert)
{
    auto& list = pluginListFor (insert.id);

    // Effects sit after the aux return (if any) and before the track's fader.
    int index = list.size() > 0 && dynamic_cast<te::AuxReturnPlugin*> (list[0]) != nullptr ? 1 : 0;

    for (auto& slot : insert.effects)
    {
        if (frozen.inserts.contains (insert.id))
        {
            dropDevice (slot.device.id); // frozen: unloaded until unfrozen
            continue;
        }

        if (ensureDevice (slot.device) == nullptr)
            continue;

        placeDevice (list, slot.device.id, index);
        index += 3; // the probe, the device and its guard
    }
    return index;
}

//==============================================================================
void EngineAdapter::syncSends (const Project& project)
{
    // One bus per send target, with an AuxReturnPlugin first on the target.
    std::set<InsertId> targets;
    auto forEachInsert = [&] (auto&& fn)
    {
        fn (project.mixer.master);
        for (auto& i : project.mixer.inserts)
            fn (i);
    };

    forEachInsert (
        [&] (const Insert& i)
        {
            for (auto& s : i.sends)
                targets.insert (s.target);
        });

    for (auto it = returnBuses.begin(); it != returnBuses.end();)
    {
        if (targets.contains (it->first))
        {
            ++it;
            continue;
        }

        if (project.findInsert (it->first) != nullptr)
            for (auto* r : pluginListFor (it->first).getPluginsOfType<te::AuxReturnPlugin>())
                r->deleteFromParent();

        it = returnBuses.erase (it);
    }

    for (auto target : targets)
    {
        if (returnBuses.contains (target))
            continue;

        int bus = 0;
        while (bus < maxAuxBuses &&
               std::any_of (returnBuses.begin(), returnBuses.end(), [&] (auto& b) { return b.second == bus; }))
            ++bus;

        if (bus == maxAuxBuses)
        {
            jassertfalse; // more than 32 send targets: not supported by Tracktion's aux buses
            continue;
        }

        returnBuses[target] = bus;
        auto plugin = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {});
        dynamic_cast<te::AuxReturnPlugin&> (*plugin).busNumber = bus;
        pluginListFor (target).insertPlugin (plugin, 0, nullptr);
    }

    // Sends: AuxSendPlugins after the fader, one per target bus.
    forEachInsert (
        [&] (const Insert& i)
        {
            auto& list = pluginListFor (i.id);
            std::map<int, float> wanted;
            for (auto& s : i.sends)
                if (auto bus = returnBuses.find (s.target); bus != returnBuses.end())
                    wanted[bus->second] = s.levelDb;

            for (auto* send : list.getPluginsOfType<te::AuxSendPlugin>())
            {
                auto w = wanted.find (send->getBusNumber());
                if (w == wanted.end())
                {
                    send->deleteFromParent();
                    continue;
                }

                if (! juce::exactlyEqual (send->getGainDb(), w->second))
                    send->setGainDb (w->second);
                wanted.erase (w);
            }

            for (auto& [bus, levelDb] : wanted)
            {
                auto plugin = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {});
                auto& send = dynamic_cast<te::AuxSendPlugin&> (*plugin);
                send.busNumber = bus;
                send.setGainDb (levelDb);
                list.insertPlugin (plugin, -1, nullptr);
            }
        });
}

void EngineAdapter::syncSidechains (const Project& project)
{
    auto apply = [&] (const Insert& insert)
    {
        te::AudioTrack* source = nullptr;
        for (auto id : insert.sidechainSources)
        {
            source = getInsertTrack (id);
            if (source != nullptr)
                break;
        }

        for (auto& slot : insert.effects)
        {
            auto* plugin = getDevicePlugin (slot.device.id);
            if (plugin == nullptr || ! plugin->canSidechain())
                continue;

            const auto wanted = source != nullptr ? source->itemID : te::EditItemID();
            if (plugin->getSidechainSourceID() != wanted)
            {
                plugin->setSidechainSourceID (wanted);
                plugin->guessSidechainRouting();
            }
        }
    };

    apply (project.mixer.master);
    for (auto& i : project.mixer.inserts)
        apply (i);
}

//==============================================================================
void EngineAdapter::syncChannels (const Project& project)
{
    if (applied != nullptr)
    {
        for (auto& old : applied->channels)
        {
            if (project.findChannel (old.id) != nullptr)
                continue;

            if (old.instrument)
                dropDevice (old.instrument->id);

            for (auto it = midiClips.begin(); it != midiClips.end();)
                it = it->first.channel == old.id ? midiClips.erase (it) : std::next (it);
            for (auto it = launcherClips.begin(); it != launcherClips.end();)
                it = it->first.channel == old.id ? launcherClips.erase (it) : std::next (it);
            previewClips.erase (old.id);
            frozenClips.erase (old.id);

            edit.deleteTrack (channelTracks.at (old.id).get());
            channelTracks.erase (old.id);
        }
    }

    for (auto& channel : project.channels)
        syncChannel (channel);
}

void EngineAdapter::syncChannel (const Channel& channel)
{
    if (! channelTracks.contains (channel.id))
        channelTracks[channel.id] = &createTrack (channel.name);

    auto& track = *channelTracks.at (channel.id);
    if (track.getName() != channel.name)
        track.setName (channel.name);

    // Instrument replaced or removed: drop the old plugin.
    if (applied != nullptr)
        if (auto* old = applied->findChannel (channel.id); old != nullptr && old->instrument)
            if (! channel.instrument || channel.instrument->id != old->instrument->id ||
                ! sameIdentity (*channel.instrument, *old->instrument))
                dropDevice (old->instrument->id);

    if (channel.instrument && frozen.channels.contains (channel.id))
        dropDevice (channel.instrument->id); // frozen: unloaded until unfrozen
    else if (channel.instrument && ensureDevice (*channel.instrument) != nullptr)
        placeDevice (track.pluginList, channel.instrument->id, 0);

    if (auto* volume = track.getVolumePlugin())
    {
        volume->setVolumeDb (channel.volumeDb);
        volume->setPan (channel.pan);
    }

    track.setMute (channel.muted);
    track.setSolo (channel.solo);
}

te::AudioTrack& EngineAdapter::getUnheardSink()
{
    // Not "no output": an offline render adds up every track that goes to no other track. Nothing is
    // muted either, since a track playing into a muted one is muted too (and would not feed sidechains).
    if (unheardSink == nullptr)
    {
        unheardSink = &createTrack ("Unheard");
        if (auto* volume = unheardSink->getVolumePlugin())
            volume->setSliderPos (0.0f); // silence
    }
    return *unheardSink;
}

void EngineAdapter::reapplyRouting (const Project& project)
{
    // Tracktion stores track-to-track routing as "track <number>", so deleting a track silently
    // re-points the outputs of tracks after it. Re-assert the routing by stable ID every time; setting
    // an unchanged value is free.
    for (auto& channel : project.channels)
        routeTo (*channelTracks.at (channel.id), channel.targetInsert);

    for (auto& insert : project.mixer.inserts)
        if (unheardInserts.contains (insert.id))
            insertTracks.at (insert.id)->getOutput().setOutputToTrack (&getUnheardSink());
        else
            routeTo (*insertTracks.at (insert.id), insert.output);
}

//==============================================================================
void EngineAdapter::syncPatternClips (const Project& project, bool timingChanged)
{
    std::map<ClipKey, std::pair<const Clip*, const std::vector<Note>*>> wanted;

    for (auto& clip : project.playlist.clips)
    {
        auto* pc = std::get_if<PatternClip> (&clip.content);
        if (pc == nullptr)
            continue;

        if (auto* pattern = project.findPattern (pc->pattern))
            for (auto& [channelId, notes] : pattern->notes)
                if (! notes.empty() && channelTracks.contains (channelId) && ! frozen.channels.contains (channelId))
                    wanted[{ clip.id, channelId }] = { &clip, &notes };
    }

    for (auto it = midiClips.begin(); it != midiClips.end();)
    {
        if (wanted.contains (it->first))
        {
            ++it;
            continue;
        }

        removeMidiClip (*it->second);
        it = midiClips.erase (it);
    }

    for (auto& [key, value] : wanted)
    {
        auto& [clip, notes] = value;
        const auto& pc = std::get<PatternClip> (clip->content);
        const auto* pattern = project.findPattern (pc.pattern);
        const auto* lane = project.findLane (clip->lane);
        // Pattern mode silences the whole playlist.
        const bool muted =
            clip->muted || (lane != nullptr && project.isLaneMuted (lane->id)) || playMode == PlayMode::pattern;

        auto existing = midiClips.find (key);
        bool needsWrite = existing == midiClips.end() || timingChanged || applied == nullptr;

        if (! needsWrite)
        {
            // Rewrite only when something this clip shows changed (per-channel rewrites); a clip
            // muted or unmuted (also by the play mode) is only muted or unmuted.
            const auto* oldClip = applied->findClip (clip->id);
            const auto* oldPattern = applied->findPattern (pc.pattern);
            auto sameNotes = [&]
            {
                auto it = oldPattern->notes.find (key.channel);
                return it != oldPattern->notes.end() && it->second == *notes;
            };

            needsWrite = oldClip == nullptr || ! sameExceptMute (*oldClip, *clip) || oldPattern == nullptr ||
                         oldPattern->length != pattern->length || ! samePlaying (*oldPattern, *pattern) ||
                         ! sameNotes() ||
                         playsNoteExpressions (*applied, key.channel) != playsNoteExpressions (project, key.channel);
        }

        if (! needsWrite)
        {
            if (existing->second->isMuted() != muted)
                existing->second->setMuted (muted);
            continue;
        }

        if (existing == midiClips.end())
            existing = midiClips.emplace (key, nullptr).first;

        writeMidiClip (existing->second, *channelTracks.at (key.channel), *clip, pc, *notes, *pattern, muted,
                       playsNoteExpressions (project, key.channel));
        if (existing->second == nullptr)
            midiClips.erase (existing);
    }
}

void EngineAdapter::syncNoteSources (const Project& project, bool force)
{
    // What the copies are written from, as text: they are rewritten only when it changes.
    auto signatureOf = [&] (ChannelId channel, std::vector<std::pair<const Clip*, const Pattern*>>& clips)
    {
        juce::String text;
        auto addNotes = [&] (const Pattern& pattern)
        {
            text << " len " << pattern.length << ":";
            if (auto it = pattern.notes.find (channel); it != pattern.notes.end())
                for (auto& n : it->second)
                    text << " " << n.start << "/" << n.length << "/" << n.pitch << "/" << n.velocity;
        };

        if (playMode == PlayMode::pattern)
        {
            if (const auto* pattern = project.findPattern (currentPattern);
                pattern != nullptr && pattern->notes.contains (channel) && ! pattern->notes.at (channel).empty())
            {
                text << "pattern " << (int)pattern->id.value;
                addNotes (*pattern);
                clips.emplace_back (nullptr, pattern);
            }
            return text;
        }

        for (auto& clip : project.playlist.clips)
            if (auto* pc = std::get_if<PatternClip> (&clip.content))
                if (const auto* pattern = project.findPattern (pc->pattern);
                    pattern != nullptr && pattern->notes.contains (channel) && ! pattern->notes.at (channel).empty())
                {
                    const auto* lane = project.findLane (clip.lane);
                    text << " clip " << (int)clip.id.value << " " << clip.start << "+" << clip.length << " @"
                         << pc->offset
                         << (clip.muted || (lane != nullptr && project.isLaneMuted (lane->id)) ? " muted" : "");
                    addNotes (*pattern);
                    clips.emplace_back (&clip, pattern);
                }
        return text;
    };

    for (auto& insert : project.mixer.inserts)
    {
        std::vector<std::pair<const Clip*, const Pattern*>> clips;
        juce::String signature;
        if (insert.notesFrom && project.findChannel (insert.notesFrom) != nullptr &&
            ! frozen.inserts.contains (insert.id))
            signature = signatureOf (insert.notesFrom, clips);

        auto& source = noteSources[insert.id];
        if (! force && source.signature == signature)
            continue;

        for (auto& clip : source.clips)
            removeMidiClip (*clip);
        source.clips.clear();
        source.signature = signature;

        auto& track = *insertTracks.at (insert.id);
        for (auto& [clip, pattern] : clips)
        {
            const auto& notes = pattern->notes.at (insert.notesFrom);
            if (clip == nullptr) // pattern mode: the pattern on its own, from the start
            {
                const Clip whole { {}, {}, 0, pattern->length, false, PatternClip { pattern->id, 0 } };
                te::MidiClip::Ptr created;
                writeMidiClip (created, track, whole, std::get<PatternClip> (whole.content), notes, *pattern, false,
                               false);
                if (created != nullptr)
                    source.clips.push_back (created);
                continue;
            }

            const auto* lane = project.findLane (clip->lane);
            te::MidiClip::Ptr created;
            writeMidiClip (created, track, *clip, std::get<PatternClip> (clip->content), notes, *pattern,
                           clip->muted || (lane != nullptr && project.isLaneMuted (lane->id)), false);
            if (created != nullptr)
                source.clips.push_back (created);
        }
    }
}

void EngineAdapter::setPlayMode (PlayMode mode, PatternId pattern)
{
    playMode = mode;
    currentPattern = pattern;
}

te::TimeRange EngineAdapter::getPatternLoopRange() const
{
    if (playMode != PlayMode::pattern || applied == nullptr)
        return {};

    if (auto* pattern = applied->findPattern (currentPattern))
        return { toTime (0), toTime (pattern->length) };

    return {};
}

void EngineAdapter::syncPreviewClips (const Project& project, bool force)
{
    const auto* pattern = playMode == PlayMode::pattern ? project.findPattern (currentPattern) : nullptr;

    for (auto it = previewClips.begin(); it != previewClips.end();)
    {
        const bool keep = pattern != nullptr && pattern->notes.contains (it->first) &&
                          ! pattern->notes.at (it->first).empty() && channelTracks.contains (it->first) &&
                          ! frozen.channels.contains (it->first);
        if (keep)
        {
            ++it;
            continue;
        }

        removeMidiClip (*it->second);
        it = previewClips.erase (it);
    }

    if (pattern == nullptr)
        return;

    const auto* oldPattern = applied != nullptr ? applied->findPattern (pattern->id) : nullptr;

    for (auto& [channelId, notes] : pattern->notes)
    {
        if (notes.empty() || ! channelTracks.contains (channelId) || frozen.channels.contains (channelId))
            continue;

        auto existing = previewClips.find (channelId);
        const bool changed = oldPattern == nullptr || oldPattern->length != pattern->length ||
                             ! samePlaying (*oldPattern, *pattern) || ! oldPattern->notes.contains (channelId) ||
                             ! (oldPattern->notes.at (channelId) == notes) ||
                             playsNoteExpressions (*applied, channelId) != playsNoteExpressions (project, channelId);

        if (existing != previewClips.end() && ! force && ! changed)
            continue;

        // The pattern on its own, from the start of the timeline; the transport loops over it.
        const Clip whole { {}, {}, 0, pattern->length, false, PatternClip { pattern->id, 0 } };

        if (existing == previewClips.end())
            existing = previewClips.emplace (channelId, nullptr).first;

        writeMidiClip (existing->second, *channelTracks.at (channelId), whole, std::get<PatternClip> (whole.content),
                       notes, *pattern, false, playsNoteExpressions (project, channelId));
        if (existing->second == nullptr)
            previewClips.erase (existing);
    }
}

bool EngineAdapter::playsNoteExpressions (const Project& project, ChannelId channel)
{
    const auto* c = project.findChannel (channel);
    return c != nullptr && c->instrument && expressiveMidi::canPlay (*c->instrument);
}

bool EngineAdapter::addNotes (juce::ValueTree& sequence, const Clip& clip, const PatternClip& pc,
                              const std::vector<Note>& notes, const Pattern& pattern, bool expressive) const
{
    bool anyExpression = false;
    for (auto& n : notes)
    {
        // A clip shows the pattern from `offset` for `length` ticks; patterns do not loop (FL behaviour).
        if (n.start < pc.offset || n.start >= pc.offset + clip.length || n.start >= pattern.length)
            continue;

        // The pattern's swing: notes on every second step play a little later.
        const auto start = n.start + pattern.getSwingDelay (n.start);
        const auto beat = toBeats (edit, (double)(start - pc.offset));
        const auto length = toBeatDuration (edit, (double)n.length);

        // The note as MidiList::addNote and MidiNote::setNoteOffVelocity make it, whole before it goes in: Tracktion
        // finds a note by searching its whole list, so adding through MidiList (which then looks for the note just
        // added, and again for its release velocity) took time growing with the square of the notes.
        juce::ValueTree note (te::IDs::NOTE);
        note.setProperty (te::IDs::p, n.pitch, nullptr);
        note.setProperty (te::IDs::b, beat.inBeats(), nullptr);
        note.setProperty (te::IDs::l, std::max (0.0, length.inBeats()), nullptr);
        note.setProperty (te::IDs::v, juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0f)), nullptr);
        note.setProperty (te::IDs::c, 0, nullptr);
        note.setProperty (te::IDs::lift, juce::jlimit (0, 127, juce::roundToInt (n.releaseVelocity * 127.0f)), nullptr);
        if (expressive)
            anyExpression = expressiveMidi::setNoteExpressions (note, n) || anyExpression;
        sequence.addChild (note, -1, nullptr);
    }
    return anyExpression;
}

void EngineAdapter::removeMidiClip (te::MidiClip& clip)
{
    releaseSoundingNotes (clip);
    clip.removeFromParent();
}

void EngineAdapter::releaseSoundingNotes (te::MidiClip& clip)
{
    auto& transport = edit.getTransport();
    auto* track = dynamic_cast<te::AudioTrack*> (clip.getTrack());
    if (track != nullptr && transport.isPlaying())
    {
        // The notes playing now (give or take a block or two: the audio thread is a little ahead or behind), on
        // the channels the clip plays on. The note offs go to the instrument through the track's live input.
        const auto now = transport.getPosition();
        const auto margin = te::TimeDuration::fromSeconds (0.05);
        std::set<int> pitches;
        for (auto* n : clip.getSequence().getNotes())
        {
            const auto start = clip.getTimeOfContentBeat (n->getStartBeat());
            const auto end = clip.getTimeOfContentBeat (n->getEndBeat());
            if (start <= now + margin && end >= now - margin)
                pitches.insert (n->getNoteNumber());
        }
        const auto channels = clip.getMPEMode()
                                  ? juce::Range<int> (2, 16)
                                  : juce::Range<int>::withStartAndLength (clip.getMidiChannel().getChannelNumber(), 1);
        for (auto pitch : pitches)
            for (auto channel = channels.getStart(); channel < channels.getEnd(); ++channel)
                track->injectLiveMidiMessage (juce::MidiMessage::noteOff (channel, pitch), {});
    }
}

void EngineAdapter::writeMidiClip (te::MidiClip& midiClip, const Clip& clip, const PatternClip& pc,
                                   const std::vector<Note>& notes, const Pattern& pattern, bool muted, bool expressive)
{
    // Played in beats (Tracktion's LoopingMidiNode), which releases the notes an edit takes away while they play;
    // the default proxy in seconds (MidiNode) left them sounding until playback stopped.
    midiClip.setUsesProxy (false);
    // Moved: Tracktion no longer plays the clip where it was, so the notes it held there would sound on.
    const te::ClipPosition position { { toTime (clip.start), toTime (clip.start + clip.length) }, te::TimeDuration() };
    if (midiClip.getPosition().time != position.time)
        releaseSoundingNotes (midiClip);
    midiClip.setPosition (position);
    midiClip.setMuted (muted);

    // Emptied from the front: Tracktion looks for each note taken out from the start of its list.
    auto& sequence = midiClip.getSequence();
    while (sequence.state.getNumChildren() > 0)
        sequence.state.removeChild (0, nullptr); // everything, as MidiList::clear does
    const bool anyExpression = addNotes (sequence.state, clip, pc, notes, pattern, expressive);

    // Per-note values need a channel per note; Tracktion tracks those notes in MPE mode.
    midiClip.setMPEMode (anyExpression);
}

void EngineAdapter::writeMidiClip (te::MidiClip::Ptr& slot, te::AudioTrack& track, const Clip& clip,
                                   const PatternClip& pc, const std::vector<Note>& notes, const Pattern& pattern,
                                   bool muted, bool expressive)
{
    // A few notes are rewritten in place. Thousands (a song-long part, as the Analyzer makes) each made a change
    // to the Edit, told to all its listeners, taking out the old ones too: a new clip made with its notes in it is
    // one change.
    constexpr size_t manyNotes = 256;
    if (slot != nullptr && notes.size() < manyNotes && (size_t)slot->getSequence().getNumNotes() < manyNotes)
        return writeMidiClip (*slot, clip, pc, notes, pattern, muted, expressive);

    if (slot != nullptr)
        removeMidiClip (*slot);
    slot = nullptr;

    auto sequence = te::MidiList::createMidiList();
    const bool anyExpression = addNotes (sequence, clip, pc, notes, pattern, expressive);
    juce::ValueTree state (te::TrackItem::clipTypeToXMLType (te::TrackItem::Type::midi));
    state.addChild (sequence, -1, nullptr);

    if (! track.containsAnyMIDIClips()) // as insertMIDIClip does
        track.setVerticalScaleToDefault();
    auto* created = te::insertClipWithState (
        track, state, te::TrackItem::getSuggestedNameForNewItem (te::TrackItem::Type::midi), te::TrackItem::Type::midi,
        { { toTime (clip.start), toTime (clip.start + clip.length) }, te::TimeDuration() }, te::DeleteExistingClips::no,
        false);
    slot = dynamic_cast<te::MidiClip*> (created);
    if (slot == nullptr)
        return;

    slot->setUsesProxy (false); // see the other writeMidiClip
    slot->setMuted (muted);
    slot->setMPEMode (anyExpression);
}

//==============================================================================
void EngineAdapter::syncLauncherClips (const Project& project, bool force)
{
    const auto& launcher = project.launcher;

    // Each launcher clip keeps its slot index for its lifetime, so a playing clip is never moved.
    for (auto it = launcherSlots.begin(); it != launcherSlots.end();)
        it = launcher.findClip (it->first) == nullptr ? launcherSlots.erase (it) : std::next (it);
    std::set<int> used;
    for (auto& [id, index] : launcherSlots)
        used.insert (index);
    for (auto& clip : launcher.clips)
        if (! launcherSlots.contains (clip.id))
        {
            int index = 0;
            while (used.contains (index))
                ++index;
            launcherSlots[clip.id] = index;
            used.insert (index);
        }

    // What should be in a slot: per channel, a pattern's notes or an audio file.
    struct Wanted
    {
        const LauncherClip* clip = nullptr;
        const std::vector<Note>* notes = nullptr; // a pattern clip
        juce::File file;                          // an audio clip
    };
    std::map<ClipKey, Wanted> wanted;
    for (auto& clip : launcher.clips)
    {
        if (auto* pc = std::get_if<PatternClip> (&clip.content))
        {
            if (auto* pattern = project.findPattern (pc->pattern))
                for (auto& [channelId, notes] : pattern->notes)
                    if (! notes.empty() && channelTracks.contains (channelId) && ! frozen.channels.contains (channelId))
                        wanted[{ clip.id, channelId }] = { &clip, &notes, {} };
        }
        else if (auto* audio = std::get_if<AudioClip> (&clip.content))
        {
            const auto file = resolveAudioFile (audio->file);
            if (channelTracks.contains (audio->channel) && ! frozen.channels.contains (audio->channel) &&
                file.existsAsFile())
                wanted[{ clip.id, audio->channel }] = { &clip, nullptr, file };
        }
    }

    for (auto it = launcherClips.begin(); it != launcherClips.end();)
    {
        if (wanted.contains (it->first))
        {
            ++it;
            continue;
        }
        it->second->removeFromParent();
        it = launcherClips.erase (it);
    }

    const int slotCount = used.empty() ? 0 : *used.rbegin() + 1;
    for (auto& [channelId, track] : channelTracks)
        track->getClipSlotList().ensureNumberOfSlots (slotCount);

    for (auto& [key, want] : wanted)
    {
        const auto& clip = *want.clip;
        auto existing = launcherClips.find (key);
        bool needsWrite = existing == launcherClips.end() || force || applied == nullptr;
        if (! needsWrite)
        {
            // Rewrite only when the clip or the notes it plays changed.
            const auto* old = applied->launcher.findClip (clip.id);
            needsWrite = old == nullptr || ! (*old == clip);
            if (! needsWrite && want.notes != nullptr)
            {
                const auto& pc = std::get<PatternClip> (clip.content);
                const auto* oldPattern = applied->findPattern (pc.pattern);
                const auto* pattern = project.findPattern (pc.pattern);
                const auto oldNotes = oldPattern != nullptr ? oldPattern->notes.find (key.channel)
                                                            : std::map<ChannelId, std::vector<Note>>::const_iterator();
                needsWrite =
                    oldPattern == nullptr || oldNotes == oldPattern->notes.end() ||
                    ! (oldNotes->second == *want.notes) || oldPattern->length != pattern->length ||
                    ! samePlaying (*oldPattern, *pattern) ||
                    playsNoteExpressions (*applied, key.channel) != playsNoteExpressions (project, key.channel);
            }
        }
        if (! needsWrite)
            continue;

        auto* slot = channelTracks.at (key.channel)->getClipSlotList().getClipSlots()[launcherSlots.at (clip.id)];
        if (slot == nullptr)
            continue;
        const te::TimeRange range { toTime (0), toTime (clip.length) };

        if (want.notes != nullptr)
        {
            // A MIDI clip keeps its launch handle when rewritten, so an edit does not stop it playing.
            auto* midi =
                existing != launcherClips.end() ? dynamic_cast<te::MidiClip*> (existing->second.get()) : nullptr;
            if (midi == nullptr)
            {
                if (existing != launcherClips.end())
                    existing->second->removeFromParent();
                auto created = te::insertMIDIClip (*slot, range);
                if (created == nullptr)
                    continue;
                midi = created.get();
                launcherClips[key] = created;
            }
            const auto& pc = std::get<PatternClip> (clip.content);
            const Clip asPlaylistClip { clip.id, {}, 0, clip.length, false, pc };
            writeMidiClip (*midi, asPlaylistClip, pc, *want.notes, *project.findPattern (pc.pattern), false,
                           playsNoteExpressions (project, key.channel));
            if (clip.loop)
                midi->setLoopRangeBeats ({ te::BeatPosition(), toBeatDuration (edit, (double)clip.length) });
            else
                midi->disableLooping();
        }
        else
        {
            if (existing != launcherClips.end())
                existing->second->removeFromParent();
            const auto& audio = std::get<AudioClip> (clip.content);
            const auto offset = toTime (audio.offset) - te::TimePosition();
            auto wave = te::insertWaveClip (*slot, want.file.getFileNameWithoutExtension(), want.file,
                                            { range, offset }, te::DeleteExistingClips::no);
            if (wave == nullptr)
            {
                launcherClips.erase (key);
                continue;
            }
            applyPlayback (*wave, audio); // as on the Playlist
            wave->setGainDB (audio.gainDb);
            if (clip.loop)
                wave->setLoopRangeBeats (
                    { toBeats (edit, (double)audio.offset), toBeatDuration (edit, (double)clip.length) });
            else
                wave->disableLooping(); // a clip in a slot loops unless told not to
            launcherClips[key] = wave;
        }
    }
}

std::vector<te::Clip*> EngineAdapter::getLauncherClips (ClipId id) const
{
    std::vector<te::Clip*> clips;
    for (auto& [key, clip] : launcherClips)
        if (key.clip == id)
            clips.push_back (clip.get());
    return clips;
}

std::optional<te::MonotonicBeat> EngineAdapter::nextLaunchBeat() const
{
    // Stopped: nothing to wait for, everything starts with playback.
    auto* context = edit.getTransport().getCurrentPlaybackContext();
    if (! edit.getTransport().isPlaying() || context == nullptr || applied == nullptr)
        return std::nullopt;
    const auto syncPoint = context->getSyncPoint();
    if (! syncPoint)
        return std::nullopt;

    // Tracktion's beats are the time signature's note value (Beats.h), so a beat is 1 and a bar is the
    // numerator. At once is a moment ahead, so clips launched together still start together.
    const auto now = syncPoint->beat.inBeats();
    double next = now;
    switch (applied->launcher.quantize)
    {
    case LaunchQuantize::none:
        next = now + 0.03 * edit.tempoSequence.getBeatsPerSecondAt (syncPoint->time);
        break;
    case LaunchQuantize::beat:
        next = std::ceil (now + 1.0e-6);
        break;
    case LaunchQuantize::bar:
    {
        const double bar = std::max (1, applied->settings.timeSignature.numerator);
        next = std::ceil ((now + 1.0e-6) / bar) * bar;
        break;
    }
    }
    return te::MonotonicBeat { syncPoint->monotonicBeat.v + te::BeatDuration::fromBeats (next - now) };
}

bool EngineAdapter::launch (ClipId id)
{
    returning.stopTimer(); // a new launch: the Launcher may take over again
    const auto* clip = applied != nullptr ? applied->launcher.findClip (id) : nullptr;
    const auto mine = getLauncherClips (id);
    if (clip == nullptr || mine.empty())
        return false;

    const auto at = nextLaunchBeat();
    // One clip per launcher track: the others stop as this one starts.
    for (auto& other : applied->launcher.clips)
        if (other.track == clip->track && other.id != id)
            for (auto* c : getLauncherClips (other.id))
                if (auto handle = c->getLaunchHandle())
                    handle->stop (at);
    for (auto* c : mine)
        if (auto handle = c->getLaunchHandle())
            handle->play (at);
    return true;
}

void EngineAdapter::launchScene (SceneId scene)
{
    if (applied == nullptr)
        return;
    for (auto& track : applied->launcher.tracks)
        if (const auto* clip = applied->launcher.findClip (track.id, scene); clip == nullptr || ! launch (clip->id))
            stopLauncherTrack (track.id);
}

void EngineAdapter::stopLauncherTrack (LauncherTrackId track)
{
    if (applied == nullptr)
        return;
    const auto at = nextLaunchBeat();
    for (auto& clip : applied->launcher.clips)
        if (clip.track == track)
            for (auto* c : getLauncherClips (clip.id))
                if (auto handle = c->getLaunchHandle())
                    handle->stop (at);
}

void EngineAdapter::stopLauncher (bool now)
{
    const auto at = now ? std::nullopt : nextLaunchBeat();
    for (auto& [key, clip] : launcherClips)
        if (auto handle = clip->getLaunchHandle())
            handle->stop (at);
}

void EngineAdapter::backToPlaylist()
{
    // Tracktion stops a track's launched clips when it goes back to its arrangement.
    stopLauncher (true);
    returning.start (*this);
}

void EngineAdapter::ReturnToPlaylist::start (EngineAdapter& a)
{
    adapter = &a;
    ticks = 0;
    keepOff();
    startTimerHz (50);
}

void EngineAdapter::ReturnToPlaylist::keepOff()
{
    for (auto& [id, track] : adapter->channelTracks)
    {
        track->returnToArrangementRequested = true;
        track->playSlotClips = false;
    }
}

void EngineAdapter::ReturnToPlaylist::timerCallback()
{
    // The clips stop on the audio thread's next block; until then Tracktion's own timer sees them still
    // playing and would switch their tracks back to the Launcher (a busy computer made that last a second).
    bool stillPlaying = false;
    for (auto& [key, clip] : adapter->launcherClips)
        if (auto handle = clip->getLaunchHandle())
            stillPlaying |= handle->getPlayingStatus() == te::LaunchHandle::PlayState::playing;
    keepOff();
    if (! stillPlaying || ++ticks > 150) // at most three seconds
        stopTimer();
}

EngineAdapter::LaunchState EngineAdapter::getLaunchState (ClipId id) const
{
    for (auto* c : getLauncherClips (id))
        if (auto handle = c->getLaunchHandle())
        {
            if (auto queued = handle->getQueuedStatus())
                return *queued == te::LaunchHandle::QueueState::playQueued ? LaunchState::starting
                                                                           : LaunchState::stopping;
            return handle->getPlayingStatus() == te::LaunchHandle::PlayState::playing ? LaunchState::playing
                                                                                      : LaunchState::stopped;
        }
    return LaunchState::stopped;
}

std::optional<double> EngineAdapter::getLaunchProgress (ClipId id) const
{
    const auto* clip = applied != nullptr ? applied->launcher.findClip (id) : nullptr;
    if (clip == nullptr || getLaunchState (id) == LaunchState::stopped)
        return std::nullopt;
    for (auto* c : getLauncherClips (id))
        if (auto handle = c->getLaunchHandle())
            if (auto played = handle->getPlayedRange())
            {
                const auto length = toBeatDuration (edit, (double)clip->length).inBeats();
                if (length <= 0.0)
                    return std::nullopt;
                const auto beats = played->getLength().inBeats();
                return clip->loop ? std::fmod (beats, length) / length : std::min (1.0, beats / length);
            }
    return std::nullopt;
}

bool EngineAdapter::isLauncherActive() const
{
    for (auto& [id, track] : channelTracks)
        if (track->playSlotClips.get())
            return true;
    return false;
}

//==============================================================================
juce::String EngineAdapter::describe() const
{
    if (applied == nullptr)
        return {};

    std::map<const te::AudioTrack*, InsertId> insertOfTrack;
    for (auto& [id, track] : insertTracks)
        insertOfTrack[track.get()] = id;

    auto nameOfInsertTrack = [&] (const te::AudioTrack* t) -> juce::String
    {
        auto it = insertOfTrack.find (t);
        return it != insertOfTrack.end() ? "insert " + juce::String (it->second.value) : "unknown track";
    };

    // Where each aux bus returns, found from the Tracktion side; bus numbers themselves depend on the
    // order sends were created, so they are not part of the description.
    std::map<int, juce::String> busTarget;
    auto findReturns = [&] (const te::PluginList& list, const juce::String& name)
    {
        for (auto* r : list.getPluginsOfType<te::AuxReturnPlugin>())
            busTarget[r->busNumber.get()] = name;
    };

    findReturns (edit.getMasterPluginList(), "master");
    for (auto& [id, track] : insertTracks)
        findReturns (track->pluginList, "insert " + juce::String (id.value));

    auto describeList = [&] (const te::PluginList& list)
    {
        juce::StringArray effects, sends, returns, sidechains;

        for (auto* plugin : list.getPlugins())
        {
            if (auto* send = dynamic_cast<te::AuxSendPlugin*> (plugin))
                sends.add ((busTarget.contains (send->getBusNumber()) ? busTarget[send->getBusNumber()] : "nowhere") +
                           ":" + formatDb (send->getGainDb()));
            else if (dynamic_cast<te::AuxReturnPlugin*> (plugin) != nullptr)
                returns.add ("yes");
            else if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) == nullptr &&
                     dynamic_cast<te::LevelMeterPlugin*> (plugin) == nullptr &&
                     dynamic_cast<AudioGuardPlugin*> (plugin) == nullptr &&
                     dynamic_cast<MutePlugin*> (plugin) == nullptr)
            {
                effects.add (plugin->getName() + (plugin->isEnabled() ? "" : " (bypassed)"));

                if (auto source = plugin->getSidechainSourceID(); source.isValid())
                {
                    juce::String sourceName = "unknown";
                    for (auto& [id, track] : insertTracks)
                        if (track->itemID == source)
                            sourceName = "insert " + juce::String (id.value);
                    sidechains.add (sourceName);
                }
            }
        }

        sends.sort (false);
        return "effects=[" + effects.joinIntoString (", ") + "] sends=[" + sends.joinIntoString (", ") +
               "] return=" + (returns.isEmpty() ? "no" : returns[0]) + " sidechain=[" +
               sidechains.joinIntoString (", ") + "]";
    };

    auto output = [&] (const te::AudioTrack& t)
    {
        if (t.getOutput().usesDefaultAudioOut())
            return juce::String ("master");
        return nameOfInsertTrack (t.getOutput().getDestinationTrack());
    };

    auto fader = [] (te::AudioTrack& t)
    {
        auto* v = t.getVolumePlugin();
        return "vol=" + formatDb (v->getVolumeDb()) + " pan=" + juce::String (v->getPan(), 2) +
               " mute=" + juce::String ((int)t.isMuted (false)) + " solo=" + juce::String ((int)t.isSolo (false));
    };

    juce::StringArray lines;
    auto masterVolume = edit.getMasterVolumePlugin();
    lines.add ("master: vol=" + formatDb (masterVolume->getVolumeDb()) +
               " pan=" + juce::String (masterVolume->getPan(), 2) + " mute=" +
               juce::String ((int)(dynamic_cast<MutePlugin*> (masterMute.get()) != nullptr &&
                                   dynamic_cast<MutePlugin&> (*masterMute).isMuted())) +
               " " + describeList (edit.getMasterPluginList()));

    for (auto& insert : applied->mixer.inserts)
    {
        auto& t = *insertTracks.at (insert.id);
        lines.add ("insert " + juce::String (insert.id.value) + " '" + t.getName() + "': out=" + output (t) + " " +
                   fader (t) + " " + describeList (t.pluginList));
    }

    for (auto& channel : applied->channels)
    {
        auto& t = *channelTracks.at (channel.id);
        lines.add ("channel " + juce::String (channel.id.value) + " '" + t.getName() + "': out=" + output (t) + " " +
                   fader (t) + " " + describeList (t.pluginList));
    }

    auto& tempo = edit.tempoSequence;
    auto beats = [&] (te::TimePosition time) { return juce::String (tempo.toBeats (time).inBeats(), 3); };

    for (auto& [key, clip] : midiClips)
    {
        juce::StringArray notes;
        for (auto* n : clip->getSequence().getNotes())
            notes.add (juce::String (n->getNoteNumber()) + "@" + juce::String (n->getStartBeat().inBeats(), 3) + "+" +
                       juce::String (n->getLengthBeats().inBeats(), 3) + "v" + juce::String (n->getVelocity()));

        const auto pos = clip->getPosition();
        lines.add ("midi clip " + juce::String (key.clip.value) + " channel " + juce::String (key.channel.value) +
                   " on '" + clip->getTrack()->getName() + "': " + beats (pos.getStart()) + "-" + beats (pos.getEnd()) +
                   " muted=" + juce::String ((int)clip->isMuted()) + " notes=[" + notes.joinIntoString (" ") + "]");
    }

    for (auto& [id, clip] : audioClips)
    {
        const auto pos = clip->getPosition();
        lines.add ("audio clip " + juce::String (id.value) + " on '" + clip->getTrack()->getName() +
                   "': " + beats (pos.getStart()) + "-" + beats (pos.getEnd()) +
                   " file=" + clip->getOriginalFile().getFileName() + " gain=" + formatDb (clip->getGainDB()) +
                   " muted=" + juce::String ((int)clip->isMuted()));
    }

    for (auto& [insert, source] : noteSources)
        for (auto& clip : source.clips)
        {
            juce::StringArray notes;
            for (auto* n : clip->getSequence().getNotes())
                notes.add (juce::String (n->getNoteNumber()) + "@" + juce::String (n->getStartBeat().inBeats(), 3));
            const auto pos = clip->getPosition();
            lines.add ("notes for insert " + juce::String (insert.value) + ": " + beats (pos.getStart()) + "-" +
                       beats (pos.getEnd()) + " muted=" + juce::String ((int)clip->isMuted()) + " notes=[" +
                       notes.joinIntoString (" ") + "]");
        }

    for (auto& [target, clip] : frozenClips)
        lines.add ("frozen " + model::describe (target) + " on '" + clip->getTrack()->getName() +
                   "': file=" + clip->getOriginalFile().getFileName() + " " + beats (clip->getPosition().getStart()) +
                   "-" + beats (clip->getPosition().getEnd()));

    for (auto& [channel, clip] : previewClips)
    {
        juce::StringArray notes;
        for (auto* n : clip->getSequence().getNotes())
            notes.add (juce::String (n->getNoteNumber()) + "@" + juce::String (n->getStartBeat().inBeats(), 3));

        const auto pos = clip->getPosition();
        lines.add ("pattern preview channel " + juce::String (channel.value) + ": " + beats (pos.getStart()) + "-" +
                   beats (pos.getEnd()) + " notes=[" + notes.joinIntoString (" ") + "]");
    }

    return lines.joinIntoString ("\n");
}

} // namespace spinus::engine
