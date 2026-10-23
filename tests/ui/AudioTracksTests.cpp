#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "ui/AudioTracks.h"
#include "ui/ChannelRack.h"
#include "ui/MainView.h"
#include "ui/Playlist.h"
#include "ui/Theme.h"
#include "RealtimeRunner.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
constexpr double rate = 22050.0;

juce::File writeTone (const juce::File& file, double seconds)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.4f * (float)std::sin (2.0 * M_PI * 220.0 * i / rate));
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

struct Fixture
{
    Fixture()
    {
        root.createDirectory();
        playlist.setSize (800, 400);
        tracks.setSize (600, 300);
    }
    ~Fixture() { root.deleteRecursively(); }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-audio-tracks") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ChannelRack rack { session };
    Playlist playlist { session };
    AudioTracks tracks { session };

    void update()
    {
        rack.updateNow();
        tracks.updateNow();
    }
};
} // namespace

TEST_CASE ("Audio clips' channels are audio tracks: in their own panel, not the Channel Rack", "[ui][audiotracks]")
{
    Fixture f;
    REQUIRE (f.rack.addChannel ({}, "Kick"));
    REQUIRE (f.playlist.addAudioFile (0, 0, writeTone (f.root.getChildFile ("Vocals.wav"), 3.0)));
    f.update();

    const auto& project = f.session.getProject();
    REQUIRE (project.channels.size() == 2);
    CHECK (project.channels[1].audioTrack);
    CHECK (f.rack.getNumRows() == 1);
    CHECK (f.tracks.getNumRows() == 1);
    const auto vocals = f.tracks.getTracks().front();
    CHECK (f.tracks.describeClips (vocals) == "1 clip, 0:00 to 0:03");

    // Mixing and renaming, one undo step each.
    const auto steps = f.session.getHistory().getUndoDescriptions().size();
    REQUIRE (f.tracks.setVolume (vocals, -6.0));
    REQUIRE (f.tracks.setPan (vocals, 0.25));
    REQUIRE (f.tracks.setMuted (vocals, true));
    REQUIRE (f.tracks.rename (vocals, "Lead vocal"));
    CHECK (f.session.getHistory().getUndoDescriptions().size() == steps + 4);
    const auto* channel = project.findChannel (vocals);
    CHECK (juce::approximatelyEqual (channel->volumeDb, -6.0f));
    CHECK (channel->muted);
    CHECK (channel->name == "Lead vocal");

    // Deleting a track takes its clips along.
    REQUIRE (f.tracks.removeTrack (vocals));
    f.update();
    CHECK (f.tracks.getNumRows() == 0);
    CHECK (f.session.getProject().playlist.clips.empty());
    CHECK (f.tracks.createComponentSnapshot (f.tracks.getLocalBounds()).isValid());
}

TEST_CASE ("Older projects: channels that only play audio clips become audio tracks", "[ui][audiotracks]")
{
    Fixture f;
    REQUIRE (f.playlist.addAudioFile (0, 0, writeTone (f.root.getChildFile ("Stem.wav"), 1.0)));
    REQUIRE (f.rack.addChannel ({}, "Empty")); // no instrument, no audio clips: stays in the rack

    // As saved before audio tracks existed: no "audioTrack" flag.
    auto json = model::toJsonText (f.session.getProject()).replace ("\"audioTrack\": true,", "");
    REQUIRE (! json.contains ("audioTrack"));
    model::Project loaded;
    REQUIRE (model::fromJsonText (json, loaded).wasOk());
    CHECK (loaded.channels[0].audioTrack);
    CHECK (! loaded.channels[1].audioTrack);
    CHECK (loaded == f.session.getProject());
}

TEST_CASE ("Long audio dropped on the Channel Rack goes to audio tracks; short samples become Samplers",
           "[ui][audiotracks]")
{
    Fixture f;
    const auto shortSample = writeTone (f.root.getChildFile ("Snare.wav"), 0.5);
    const auto longVocal = writeTone (f.root.getChildFile ("Verse vocal.wav"), ChannelRack::longAudioSeconds + 5.0);
    CHECK (! ChannelRack::isLongAudio (shortSample));
    CHECK (ChannelRack::isLongAudio (longVocal));

    juce::StringArray toTracks;
    f.rack.onLongAudio = [&] (const juce::StringArray& files) { toTracks.addArray (files); };
    f.rack.filesDropped ({ shortSample.getFullPathName(), longVocal.getFullPathName() }, 0, 0);
    CHECK (toTracks == juce::StringArray { longVocal.getFullPathName() });
    REQUIRE (f.session.getProject().channels.size() == 1);
    CHECK (f.session.getProject().channels[0].name == "Snare");

    // Added as tracks: on the Playlist from the song's start, below the clips already there.
    CHECK (f.playlist.getFirstFreeRow() == 0);
    REQUIRE (f.playlist.addAudioFile (2, 0, writeTone (f.root.getChildFile ("Pad.wav"), 1.0)));
    CHECK (f.playlist.getFirstFreeRow() == 3);
    CHECK (f.playlist.addAudioFiles (toTracks, f.playlist.getFirstFreeRow(), 0) == 1);
    const auto& clip = f.session.getProject().playlist.clips.back();
    CHECK (clip.lane == f.session.getProject().playlist.lanes[3].id);
    CHECK (clip.start == 0);
}

TEST_CASE ("Shift while dropping keeps long audio in the Channel Rack", "[ui][audiotracks]")
{
    Fixture f;
    const auto longVocal = writeTone (f.root.getChildFile ("Chorus vocal.wav"), ChannelRack::longAudioSeconds + 2.0);
    juce::StringArray toTracks;
    f.rack.onLongAudio = [&] (const juce::StringArray& files) { toTracks.addArray (files); };

    const auto before = juce::ModifierKeys::currentModifiers;
    juce::ModifierKeys::currentModifiers = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    f.rack.filesDropped ({ longVocal.getFullPathName() }, 0, 0);
    juce::ModifierKeys::currentModifiers = before;

    CHECK (toTracks.isEmpty());
    REQUIRE (f.session.getProject().channels.size() == 1);
    CHECK (f.session.getProject().channels[0].name == "Chorus vocal");
    CHECK (! f.session.getProject().channels[0].audioTrack);
}

TEST_CASE ("An audio track moves to the Channel Rack as a Sampler", "[ui][audiotracks]")
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-audio-move") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    MainView view (session);
    auto& tracks = *dynamic_cast<AudioTracks*> (view.getWorkspace().getPanel ("audioTracks"));
    auto& playlist = *dynamic_cast<Playlist*> (view.getWorkspace().getPanel ("playlist"));

    const auto vocal = writeTone (root.getChildFile ("Bridge vocal.wav"), 3.0);
    REQUIRE (playlist.addAudioFile (0, 0, vocal));
    const auto track = tracks.getTracks().front();
    const auto file = tracks.getTrackFile (track);
    REQUIRE (file.has_value());
    const auto path = file.value_or (juce::File());
    CHECK (path == vocal);

    REQUIRE (view.moveAudioTrackToRack (track, path));
    const auto& project = session.getProject();
    CHECK (tracks.getTracks().empty());
    CHECK (project.playlist.clips.empty());
    REQUIRE (project.channels.size() == 1);
    CHECK (project.channels[0].name == "Bridge vocal");
    CHECK (project.channels[0].instrument.has_value());
    CHECK (! project.channels[0].audioTrack);

    // Two undo steps bring the track back.
    REQUIRE (session.getHistory().undo());
    REQUIRE (session.getHistory().undo());
    CHECK (tracks.getTracks().size() == 1);
    CHECK (project.playlist.clips.size() == 1);
    root.deleteRecursively();
}

TEST_CASE ("Tracks to record: input and arm per track; the takes land on their tracks in one undo step",
           "[ui][audiotracks]")
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-multitrack") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    MainView view (session);
    auto& tracks = *dynamic_cast<AudioTracks*> (view.getWorkspace().getPanel ("audioTracks"));

    // Two empty tracks to record into, each with its input; one armed for each.
    REQUIRE (tracks.addRecordingTrack());
    REQUIRE (tracks.addRecordingTrack());
    REQUIRE (tracks.getTracks().size() == 2);
    const auto kick = tracks.getTracks()[0], overheads = tracks.getTracks()[1];
    const auto& project = session.getProject();
    CHECK (project.findChannel (kick)->name == "Track 1");
    CHECK (project.findChannel (overheads)->name == "Track 2");
    CHECK (! project.findChannel (kick)->instrument.has_value());
    REQUIRE (tracks.setRecordInput (kick, 1, false));
    REQUIRE (tracks.setRecordInput (overheads, 3, true));
    REQUIRE (tracks.setArmed (kick, true));
    REQUIRE (tracks.setArmed (overheads, true));
    CHECK (project.findChannel (overheads)->recordInput == 3);
    CHECK (project.findChannel (overheads)->recordStereo);
    CHECK (tracks.getArmedTracks() == std::vector<model::ChannelId> { kick, overheads });
    CHECK (session.getHistory().getUndoDescriptions().contains ("Arm track for recording"));
    CHECK (tracks.getTrackLevel (kick) < 1.0e-6f); // no sound card here
    tracks.updateNow();

    // The finished takes (as the recorder hands them over): each on its own track, one undo step.
    const auto steps = session.getHistory().getUndoDescriptions().size();
    session.getAudioRecorder().onFinished (
        { { writeTone (root.getChildFile ("Track 1 1.wav"), 2.0), 0.5, 1, kick },
          { writeTone (root.getChildFile ("Track 2 1.wav"), 2.0), 0.5, 1, overheads } });
    REQUIRE (project.playlist.clips.size() == 2);
    for (const auto& clip : project.playlist.clips)
    {
        const auto* audio = std::get_if<model::AudioClip> (&clip.content);
        REQUIRE (audio != nullptr);
        CHECK (clip.start == 960); // half a second at 120 BPM
        CHECK ((audio->channel == kick) != (audio->channel == overheads));
    }
    CHECK (project.playlist.clips[0].lane != project.playlist.clips[1].lane);
    CHECK (session.getHistory().getUndoDescriptions().size() == steps + 1);
    CHECK (session.getHistory().getUndoDescriptions().strings.getLast() == "Record 2 tracks");
    REQUIRE (session.getHistory().undo());
    CHECK (project.playlist.clips.empty());
    REQUIRE (session.getHistory().redo());
    CHECK (project.playlist.clips.size() == 2);
    root.deleteRecursively();
}

TEST_CASE ("Audio tracks screenshot for the manual", "[.screenshot][audiotracks]")
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-tracks-shot") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    test::RealtimeRunner card (host.getEngine(), { .outputChannels = 2, .inputChannels = 8, .maxSeconds = 1.0 });
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    AudioTracks tracks (session);
    Playlist playlist (session);
    tracks.setLookAndFeel (&lookAndFeel);
    tracks.setSize (900, 230);
    REQUIRE (playlist.addAudioFile (0, 0, writeTone (root.getChildFile ("Lead vocal.wav"), 20.0)));
    for (int i = 0; i < 3; ++i)
        REQUIRE (tracks.addRecordingTrack());
    const auto ids = tracks.getTracks();
    tracks.rename (ids[1], "Kick mic");
    tracks.rename (ids[2], "Snare mic");
    tracks.rename (ids[3], "Overheads");
    tracks.setRecordInput (ids[1], 1, false);
    tracks.setRecordInput (ids[2], 2, false);
    tracks.setRecordInput (ids[3], 3, true);
    for (int i = 1; i < 4; ++i)
        tracks.setArmed (ids[(size_t)i], true);
    tracks.updateNow();
    const auto image = tracks.createComponentSnapshot (tracks.getLocalBounds(), true, 1.0f);
    tracks.setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/audio-tracks.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    root.deleteRecursively();
}
