#include "Session.h"
#include "engine/Beats.h"

#include "commands/CommandRegistry.h"
#include "dsp/ParameterText.h"
#include "engine/ParameterIds.h"
#include "model/ModelJson.h"

#include "engine/BundledDevices.h"

namespace spinus
{

namespace te = tracktion;

Session::Session (te::Engine& engine, std::unique_ptr<engine::DeviceFactory> d)
    : devices (d != nullptr ? std::move (d) : std::make_unique<engine::DefaultDeviceFactory>()),
      edit (te::Edit::createSingleTrackEdit (engine)),
      adapter (std::make_unique<engine::EngineAdapter> (*edit, *devices))
{
    edit->clickTrackEnabled = false;
    edit->clickTrackEmphasiseBars = true; // a different click on the first beat of each bar
    suggestions.onPlaybackChanged = [this] { syncAdapter(); };
    freezer = std::make_unique<Freezer> (*this);
    tuner = std::make_unique<Tuner> (*this);
    preview = std::make_unique<engine::SamplePreview> (&engine.getDeviceManager());
    syncAdapter();
    recorder = std::make_unique<MidiRecorder> (*this);
    ideaRecorder = std::make_unique<IdeaRecorder> (*this);
    audioRecorder = std::make_unique<AudioRecorder> (*this);
    launcherRecorder = std::make_unique<LauncherRecorder> (*this);
    historyListener = history.addListener ([this] (const commands::ChangeSet& c) { projectChanged (c); });
    controlWatcher = std::make_unique<ControlWatcher> (*this);
    edit->getParameterChangeHandler().addListener (*controlWatcher);
}

void Session::ControlWatcher::pluginParameterChanged (te::AutomatableParameter& parameter, bool isFollowingAutomation)
{
    if (isFollowingAutomation)
        return;
    changed = &parameter; // only the pointer here: this may be the audio thread
    triggerAsyncUpdate();
}

void Session::ControlWatcher::handleAsyncUpdate()
{
    auto* parameter = changed.exchange (nullptr);
    if (parameter == nullptr)
        return;
    // Still one of the project's plugins? (It may have been removed meanwhile.)
    for (auto* plugin : te::getAllPlugins (session.getEdit(), false))
        for (int i = 0; i < plugin->getNumAutomatableParameters(); ++i)
            if (plugin->getAutomatableParameter (i).get() == parameter)
                if (auto device = session.adapter->findDevice (plugin))
                {
                    model::ParameterTarget target;
                    target.kind = model::ParameterTarget::Kind::deviceParameter;
                    target.device = *device;
                    target.parameterId = parameter->paramID;
                    session.lastMoved = target;
                    return;
                }
}

juce::String Session::describeLastMovedControl() const
{
    if (! lastMoved)
        return {};
    if (auto* parameter = adapter->findParameter (*lastMoved))
        return parameter->getParameterName() + " (" + getDeviceTitle (lastMoved->device) + ")";
    return {};
}

Session::~Session()
{
    edit->getParameterChangeHandler().removeListener (*controlWatcher);
    controlWatcher->cancelPendingUpdate();
    history.removeListener (historyListener);
    edit->getTransport().stop (false, true);
    preview = nullptr;
    audioRecorder = nullptr;
    ideaRecorder = nullptr;
    launcherRecorder = nullptr;
    recorder = nullptr;
    tuner = nullptr;
    freezer = nullptr;
    adapter = nullptr;
    edit = nullptr;
}

void Session::setRunSeparately (model::DeviceId id, bool separately)
{
    const auto& project = getProject();
    const auto* device = project.findDevice (id);
    if (sandboxList == nullptr || device == nullptr || device->kind != model::Device::Kind::plugin)
        return;
    const auto key = plugins::SandboxList::keyOf (device->plugin.format, device->plugin.fileOrIdentifier);
    sandboxList->set (key, separately, device->plugin.name);

    std::vector<model::DeviceId> same;
    auto collect = [&] (const model::Device& d)
    {
        if (d.kind == model::Device::Kind::plugin &&
            plugins::SandboxList::keyOf (d.plugin.format, d.plugin.fileOrIdentifier) == key)
            same.push_back (d.id);
    };
    for (auto& channel : project.channels)
        if (channel.instrument)
            collect (*channel.instrument);
    for (auto& slot : project.mixer.master.effects)
        collect (slot.device);
    for (auto& insert : project.mixer.inserts)
        for (auto& slot : insert.effects)
            collect (slot.device);
    adapter->reloadDevices (same, project);
    sendChangeMessage();
}

void Session::reloadDevice (model::DeviceId id)
{
    adapter->reloadDevices ({ id }, getProject());
    sendChangeMessage();
}

void Session::retryMissingDevices()
{
    if (adapter->getMissingDevices().empty())
        return;
    adapter->sync (getProject()); // creates every device it can that is not there yet
    sendChangeMessage();
}

void Session::projectChanged (const commands::ChangeSet&)
{
    // The current pattern may have been removed (or the first one created).
    const auto& project = history.getProject();
    if (project.findPattern (currentPattern) == nullptr)
    {
        currentPattern = project.patterns.empty() ? model::PatternId() : project.patterns.front().id;
        sendChangeMessage();
    }

    if (project.findChannel (selectedChannel) == nullptr)
    {
        selectedChannel = project.channels.empty() ? model::ChannelId() : project.channels.back().id;
        sendChangeMessage();
    }

    if (loading == Loading::clearing)
    {
        freezer->clear();
        tuner->clear();
    }
    else if (loading == Loading::loading)
    {
        tuner->update();
        freezer->projectLoaded();
    }
    syncAdapter();

    // A pattern's length may have changed: keep looping over the whole pattern (or the region).
    applyLoop();

    // The selected channel's track may be new (or gone): the keyboard follows it.
    if (recorder != nullptr)
        recorder->updateRouting();
}

void Session::syncAdapter()
{
    applyMode();
    if (loading == Loading::no)
    {
        tuner->update(); // first: frozen renders use the tuned audio
        freezer->update();
    }
    adapter->sync (suggestions.isListening() ? suggestions.withSuggestion (history.getProject())
                                             : history.getProject());
}

void Session::refreshRenderedAudio()
{
    syncAdapter();
    sendChangeMessage();
}

bool Session::isDeviceFrozen (model::DeviceId id) const
{
    const auto& p = getProject();
    return model::getFrozenParts (p, model::getEffectiveFreezeTargets (p)).devices.contains (id);
}

void Session::applyMode()
{
    adapter->setPlayMode (mode == Mode::pattern ? engine::EngineAdapter::PlayMode::pattern
                                                : engine::EngineAdapter::PlayMode::song,
                          currentPattern);
}

project::ProjectData Session::captureProjectData() const { return { getProject(), adapter->captureDeviceStates() }; }

void Session::loadProject (project::ProjectData data)
{
    auto& transport = edit->getTransport();
    transport.stop (false, true);
    transport.setPosition (te::TimePosition());
    playStart = te::TimePosition();

    // Remove the old project's devices first: device IDs repeat between projects, and a device must
    // be created fresh to receive its saved settings.
    suggestions.discard(); // they were for the old project
    loading = Loading::clearing;
    history.reset ({});
    adapter->setSavedDeviceStates (std::move (data.deviceStates));
    loading = Loading::loading;
    history.reset (std::move (data.project));
    loading = Loading::no;

    currentPattern = getProject().patterns.empty() ? model::PatternId() : getProject().patterns.front().id;
    selectedChannel = getProject().channels.empty() ? model::ChannelId() : getProject().channels.front().id;
    syncAdapter();
    recorder->updateRouting();
    sendChangeMessage();
}

void Session::setProjectFolder (const juce::File& folder)
{
    projectFolder = folder;
    adapter->setAudioFolder (folder == juce::File() ? juce::File() : folder.getChildFile ("audio"));
    freezer->setProjectFolder (folder);
    tuner->setProjectFolder (folder);
    syncAdapter();
}

//==============================================================================
bool Session::isPlaying() const { return edit->getTransport().isPlaying(); }

void Session::play()
{
    auto& transport = edit->getTransport();
    if (transport.isPlaying())
        return;

    applyLoop();
    if (mode == Mode::pattern)
    {
        // Pattern mode always plays the pattern (or its loop region) from its start, looping.
        transport.setPosition (patternLoop ? toTimeRange (*patternLoop).getStart() : te::TimePosition());
    }
    else if (songLoop && ! toTimeRange (*songLoop).contains (transport.getPosition()))
    {
        transport.setPosition (toTimeRange (*songLoop).getStart());
    }

    playStart = transport.getPosition();
    transport.play (false);
    if (recordArmed && mode == Mode::pattern)
        recorder->start();
    sendChangeMessage();
}

void Session::stop()
{
    auto& transport = edit->getTransport();
    if (! transport.isPlaying())
        return;

    if (recorder->isRecording())
        lastRecorded = recorder->stop();

    if (launcherRecorder->isArmed())
        lastLauncherRecorded = launcherRecorder->finish(); // before the launched clips stop
    adapter->stopLauncher (true);
    transport.stop (false, false);
    transport.setPosition (playStart);
    sendChangeMessage();
}

bool Session::launchClip (model::ClipId clip)
{
    if (! adapter->launch (clip))
        return false;
    if (! isPlaying())
        play();
    sendChangeMessage();
    return true;
}

void Session::launchScene (model::SceneId scene)
{
    adapter->launchScene (scene);
    if (! isPlaying())
        for (auto& clip : getProject().launcher.clips)
            if (clip.scene == scene && adapter->getLaunchState (clip.id) != engine::EngineAdapter::LaunchState::stopped)
            {
                play();
                break;
            }
    sendChangeMessage();
}

void Session::stopLauncherTrack (model::LauncherTrackId track)
{
    adapter->stopLauncherTrack (track);
    sendChangeMessage();
}

void Session::stopLauncher()
{
    adapter->stopLauncher (false);
    sendChangeMessage();
}

void Session::backToPlaylist()
{
    adapter->backToPlaylist();
    sendChangeMessage();
}

void Session::setLauncherRecording (bool record)
{
    if (record == launcherRecorder->isArmed())
        return;
    if (record)
        setMode (Mode::song); // what is launched is recorded at its song position
    else
        lastLauncherRecorded = launcherRecorder->finish();
    launcherRecorder->setArmed (record);
    sendChangeMessage();
}

void Session::togglePlay()
{
    if (isPlaying())
        stop();
    else
        play();
}

void Session::setMode (Mode newMode)
{
    if (mode == newMode)
        return;

    const bool wasPlaying = isPlaying();
    stop();
    mode = newMode;
    syncAdapter();
    applyLoop();

    if (wasPlaying)
        play();

    sendChangeMessage();
}

void Session::applyLoop()
{
    auto& transport = edit->getTransport();
    if (mode == Mode::pattern)
    {
        const auto whole = adapter->getPatternLoopRange();
        auto range = patternLoop ? toTimeRange (*patternLoop).getIntersectionWith (whole) : whole;
        transport.setLoopRange (range.isEmpty() ? whole : range);
        transport.looping = true;
    }
    else
    {
        if (songLoop)
            transport.setLoopRange (toTimeRange (*songLoop));
        transport.looping = songLoop.has_value();
    }
}

te::TimeRange Session::toTimeRange (LoopRegion region) const
{
    auto at = [this] (model::Ticks t) { return engine::ticksToTime (*edit, (double)t); };
    return { at (region.start), at (region.end) };
}

void Session::setLoopRegion (Mode forMode, std::optional<LoopRegion> region)
{
    if (region && region->end <= region->start)
        region.reset();
    (forMode == Mode::pattern ? patternLoop : songLoop) = region;
    applyLoop();
    sendChangeMessage();
}

void Session::toggleMode() { setMode (mode == Mode::pattern ? Mode::song : Mode::pattern); }

void Session::setCurrentPattern (model::PatternId id)
{
    if (id == currentPattern || (id.isValid() && history.getProject().findPattern (id) == nullptr))
        return;

    currentPattern = id;
    patternLoop.reset(); // it was for the other pattern
    syncAdapter();
    applyLoop();

    sendChangeMessage();
}

void Session::setSelectedChannel (model::ChannelId id)
{
    if (id == selectedChannel || history.getProject().findChannel (id) == nullptr)
        return;

    selectedChannel = id;
    recorder->updateRouting();
    sendChangeMessage();
}

void Session::setRecordArmed (bool armed)
{
    if (armed == recordArmed)
        return;

    recordArmed = armed;
    if (armed)
    {
        setMode (Mode::pattern); // notes are recorded into the current pattern
        if (isPlaying())
            recorder->start();
    }
    else if (recorder->isRecording())
    {
        lastRecorded = recorder->stop();
    }
    sendChangeMessage();
}

std::unique_ptr<engine::ExportJob>
Session::createExportJob (const juce::File& destination, engine::ExportSettings settings,
                          std::function<void (const engine::ExportJob::Result&)> onFinished)
{
    // Audio clips of a saved project name files in its audio/ folder.
    settings.audioFolder = projectFolder == juce::File() ? juce::File() : projectFolder.getChildFile ("audio");
    settings.tunedAudio = tuner->getRenderedFiles(); // Spinus Tune's corrections are part of the song
    return std::make_unique<engine::ExportJob> (edit->engine, *devices, getProject(), adapter->captureDeviceStates(),
                                                destination, settings, std::move (onFinished));
}

bool Session::showDeviceWindow (model::DeviceId id)
{
    if (isDeviceFrozen (id))
        return false;

    auto* plugin = adapter->getDevicePlugin (id);
    if (plugin == nullptr)
        return false;
    plugin->showWindowExplicitly();
    return plugin->windowState->isWindowShowing();
}

bool Session::isDeviceWindowShowing (model::DeviceId id) const
{
    auto* plugin = adapter->getDevicePlugin (id);
    return plugin != nullptr && plugin->windowState->isWindowShowing();
}

void Session::closeDeviceWindow (model::DeviceId id)
{
    if (auto* plugin = adapter->getDevicePlugin (id))
        plugin->windowState->closeWindowExplicitly();
}

model::ClipId Session::createAutomationClip (const model::ParameterTarget& target)
{
    auto* parameter = adapter->findParameter (target);
    if (parameter == nullptr)
        return {};

    const auto value = juce::jlimit (0.0f, 1.0f, parameter->getCurrentNormalisedValue());
    const auto bar = getProject().settings.timeSignature.barTicks();

    auto json = model::targetToJson (target);

    juce::Array<juce::var> points;
    for (auto time : { (model::Ticks)0, bar * 4 })
        points.add (model::pointToJson ({ time, value, 0.0f }));

    auto* params = new juce::DynamicObject();
    params->setProperty ("laneName", describeTarget (target));
    params->setProperty ("start", 0);
    params->setProperty ("length", (juce::int64)(bar * 4));
    params->setProperty ("target", json);
    params->setProperty ("points", points);

    juce::String error;
    auto command = commands::CommandRegistry::get().create ("clip.addAutomation", juce::var (params), error);
    auto* raw = command.get();
    if (command == nullptr || history.perform (std::move (command)).failed())
        return {};
    return model::ClipId { (uint32_t)(int)raw->getResult()["clip"] };
}

juce::String Session::describeTarget (const model::ParameterTarget& target) const
{
    using K = model::ParameterTarget::Kind;
    const auto& p = getProject();
    switch (target.kind)
    {
    case K::channelVolume:
    case K::channelPan:
        if (auto* c = p.findChannel (target.channel))
            return c->name + (target.kind == K::channelVolume ? " volume" : " pan");
        break;
    case K::insertVolume:
    case K::insertPan:
        if (auto* i = p.findInsert (target.insert))
            return i->name + (target.kind == K::insertVolume ? " volume" : " pan");
        break;
    case K::deviceParameter:
    {
        auto name = target.parameterId;
        if (auto* parameter = adapter->findParameter (target))
            name = parameter->getParameterName();
        auto title = getDeviceTitle (target.device);
        return (title.isNotEmpty() ? title.upToFirstOccurrenceOf (" - ", false, false) : juce::String ("Device")) +
               " " + name;
    }
    }
    return "Automation";
}

juce::String Session::describeTargetValue (const model::ParameterTarget& target, float normalized) const
{
    normalized = juce::jlimit (0.0f, 1.0f, normalized);
    if (auto* parameter = adapter->findParameter (target))
    {
        const auto value = parameter->valueRange.convertFrom0to1 (normalized);
        return (parameter->valueToString (value) + " " + parameter->getLabel()).trim();
    }
    return juce::String (juce::roundToInt (normalized * 100.0f)) + "%";
}

std::optional<float> Session::parseTargetValue (const model::ParameterTarget& target, const juce::String& text) const
{
    const auto trimmed = text.trim();

    // A plugin's setting: as the plugin's own parameter reads it (dsp/ParameterText.h's rule).
    if (target.kind == model::ParameterTarget::Kind::deviceParameter)
        if (auto* external = dynamic_cast<te::ExternalPlugin*> (adapter->getDevicePlugin (target.device)))
            if (auto* instance = external->getAudioPluginInstance())
                if (auto* parameter = engine::ParameterFinder (*instance).find (target.parameterId))
                    return dsp::param::normalisedForText (*parameter, trimmed);

    // Volume and pan: Tracktion's own reading, in the units they show.
    if (auto* parameter = adapter->findParameter (target))
    {
        const auto shown = (parameter->valueToString (parameter->getCurrentValue()) + " " + parameter->getLabel());
        if (! trimmed.containsAnyOf ("0123456789") || ! dsp::param::unitsAgree (trimmed, shown))
            return std::nullopt;
        return juce::jlimit (0.0f, 1.0f,
                             parameter->valueRange.convertTo0to1 (
                                 parameter->valueRange.getRange().clipValue (parameter->stringToValue (trimmed))));
    }

    // Not loaded: a percentage of the whole range, as describeTargetValue shows it.
    if (! trimmed.containsAnyOf ("0123456789") || ! dsp::param::unitsAgree (trimmed, "0%"))
        return std::nullopt;
    return juce::jlimit (0.0f, 1.0f, trimmed.upToFirstOccurrenceOf ("%", false, false).trim().getFloatValue() / 100.0f);
}

std::vector<std::pair<juce::String, juce::String>> Session::getDeviceParameters (model::DeviceId id) const
{
    std::vector<std::pair<juce::String, juce::String>> result;
    if (auto* plugin = adapter->getDevicePlugin (id))
        for (int i = 0; i < plugin->getNumAutomatableParameters(); ++i)
            if (auto parameter = plugin->getAutomatableParameter (i))
                result.emplace_back (parameter->paramID, parameter->getParameterName());
    return result;
}

juce::String Session::getDeviceParameterValueText (model::DeviceId id, const juce::String& parameterId) const
{
    if (auto* plugin = adapter->getDevicePlugin (id))
        for (int i = 0; i < plugin->getNumAutomatableParameters(); ++i)
            if (auto parameter = plugin->getAutomatableParameter (i);
                parameter != nullptr && parameter->paramID == parameterId)
                return parameter->getCurrentValueAsStringWithLabel().trim();
    return {};
}

juce::String Session::getDeviceTitle (model::DeviceId id) const
{
    auto nameOf = [] (const model::Device& d) { return model::getDeviceDisplayName (d); };

    const auto& p = getProject();
    for (auto& channel : p.channels)
        if (channel.instrument && channel.instrument->id == id)
            return nameOf (*channel.instrument) + " - " + channel.name;

    auto search = [&] (const model::Insert& insert, const juce::String& where) -> juce::String
    {
        for (auto& slot : insert.effects)
            if (slot.device.id == id)
                return nameOf (slot.device) + " - " + where;
        return {};
    };

    if (auto t = search (p.mixer.master, "Master"); t.isNotEmpty())
        return t;
    for (size_t i = 0; i < p.mixer.inserts.size(); ++i)
        if (auto t = search (p.mixer.inserts[i], juce::String ((int)i + 1) + " " + p.mixer.inserts[i].name);
            t.isNotEmpty())
            return t;
    return {};
}

bool Session::isMetronomeOn() const { return edit->clickTrackEnabled.get(); }

void Session::setMetronome (bool on)
{
    edit->clickTrackEnabled = on;
    sendChangeMessage();
}

juce::String Session::getPositionText() const
{
    // In the pattern's own bars in pattern mode (it may have its own time signature), the song's otherwise.
    const auto& project = history.getProject();
    const auto sig = mode == Mode::pattern ? project.getTimeSignature (currentPattern) : project.settings.timeSignature;
    const auto ticks = juce::jmax<model::Ticks> (
        0, (model::Ticks)std::floor (engine::timeToTicks (*edit, edit->getTransport().getPosition())));
    const auto bar = ticks / sig.barTicks() + 1;
    const auto inBar = ticks % sig.barTicks();
    const auto beat = inBar / sig.beatTicks() + 1;
    const auto step = (inBar % sig.beatTicks()) / (model::ticksPerQuarterNote / 4) + 1;
    return juce::String (bar) + ":" + juce::String (beat).paddedLeft ('0', 2) + ":" +
           juce::String (step).paddedLeft ('0', 2);
}

float Session::getCpuUsage() const { return (float)edit->engine.getDeviceManager().getCpuUsage(); }

} // namespace spinus
