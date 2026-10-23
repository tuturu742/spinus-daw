#include "ManualDemo.h"

#include "ChannelRack.h"
#include "MainView.h"
#include "Mixer.h"
#include "PianoRoll.h"
#include "Playlist.h"
#include "commands/CommandRegistry.h"
#include "session/Session.h"

namespace spinus::ui
{

namespace
{
void perform (Session& session, const juce::String& name, const juce::String& json)
{
    juce::String error;
    if (auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error))
        session.getHistory().perform (std::move (command));
}
} // namespace

void makeManualDemo (MainView& view, Session& session)
{
    auto& workspace = view.getWorkspace();
    auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
    if (rack == nullptr)
        return;

    perform (session, "project.setKey", R"({"tonic": 0, "scale": "minor"})");
    rack->addChannel ("bundled.surge-xt", "Surge XT");
    rack->addChannel ({}, "Kick");
    rack->addChannel ({}, "Hat");
    rack->addChannel ("spinus.3x-osc", "Bass");
    const auto& channels = session.getProject().channels;
    const auto lead = channels[0].id, kick = channels[1].id, hat = channels[2].id, bass = channels[3].id;
    rack->setChannelColour (lead, 0xff4a90e0);
    rack->setChannelColour (kick, 0xffe0894a);
    rack->setChannelColour (hat, 0xffd9c34a);
    rack->setChannelColour (bass, 0xff9a6ae0);
    for (int step : { 0, 4, 8, 12 })
        rack->toggleStep (kick, step);
    for (int step = 2; step < 16; step += 4)
        rack->toggleStep (hat, step);
    for (int step : { 0, 7, 10 })
        rack->toggleStep (bass, step);
    rack->setSwing (0.2);
    rack->updateNow();

    if (! session.getProject().patterns.empty())
        perform (session, "pattern.set",
                 "{\"pattern\": " + juce::String ((int)session.getProject().patterns.front().id.value) +
                     R"(, "name": "Verse", "colour": "#ff4a90e0"})");

    if (auto* roll = dynamic_cast<PianoRoll*> (workspace.getPanel ("pianoRoll")))
    {
        session.setSelectedChannel (lead);
        roll->handleUpdateNowIfNeeded();
        const std::pair<int, int> melody[] = { { 60, 0 }, { 63, 1 }, { 67, 2 }, { 70, 3 },
                                               { 72, 4 }, { 70, 6 }, { 67, 7 } };
        for (auto [pitch, half] : melody)
            roll->addNote (pitch, (model::Ticks)half * 480, 480, 0.6f + 0.05f * (float)(half % 3));
        // A chord under the melody, so the chord name shows.
        for (int pitch : { 48, 51, 55 })
            roll->addNote (pitch, 0, 1920, 0.7f);
        roll->highlightSongKey();
        roll->scrollToPitch (60);
    }

    if (auto* playlist = dynamic_cast<Playlist*> (workspace.getPanel ("playlist")))
    {
        for (auto [row, bar] : { std::pair { 0, 0 }, std::pair { 0, 1 }, std::pair { 0, 2 }, std::pair { 0, 3 },
                                 std::pair { 1, 2 }, std::pair { 1, 3 }, std::pair { 2, 4 } })
            playlist->placePattern (row, (model::Ticks)bar * 3840);
        playlist->addMarker (0, "Intro");
        playlist->addMarker ((model::Ticks)2 * 3840, "Drop");
        playlist->renameLane (0, "Verse");
        playlist->setLaneColour (0, 0xff4a90e0);
        playlist->renameLane (1, "Drop");
        playlist->setLaneColour (1, 0xffe0894a);
    }

    if (auto* mixer = dynamic_cast<Mixer*> (workspace.getPanel ("mixer")))
    {
        const auto inserts = session.getProject().mixer.inserts; // Surge XT, Kick, Hat, Bass
        mixer->addEffect (inserts[0].id, "spinus.eq");
        mixer->addEffect (inserts[0].id, "spinus.compressor");
        mixer->addEffect (inserts[1].id, "spinus.compressor");
        mixer->setInsertVolume (inserts[1].id, -3.0);
        mixer->setInsertPan (inserts[2].id, 0.3);
        mixer->addEffect (inserts[3].id, "spinus.ducker");
        mixer->setSidechain (inserts[3].id, inserts[1].id);
        mixer->addInsert();
        const auto reverbBus = session.getProject().mixer.inserts.back().id;
        mixer->renameInsert (reverbBus, "Reverb");
        mixer->addEffect (reverbBus, "bundled.dragonfly.hall");
        mixer->setSend (inserts[0].id, reverbBus, -9.0);
        mixer->setSend (inserts[2].id, reverbBus, -12.0);
        mixer->addEffect (model::masterInsertId, "spinus.limiter");
        // The lead's volume rising and falling: an automation clip on the Playlist.
        model::ParameterTarget leadVolume;
        leadVolume.kind = model::ParameterTarget::Kind::insertVolume;
        leadVolume.insert = inserts[0].id;
        if (auto clip = session.createAutomationClip (leadVolume); clip.isValid())
            perform (session, "clip.setPoints",
                     "{\"clip\": " + juce::String ((int)clip.value) +
                         R"(, "points": [{"time": 0, "value": 0.2}, {"time": 7680, "value": 0.9, "curve": 0.4},
                                         {"time": 11520, "value": 0.6}, {"time": 15360, "value": 0.85}]})");
        mixer->selectInsert (inserts[0].id);
        mixer->scrollToStrip (0);
        mixer->updateNow();
    }
}

} // namespace spinus::ui
