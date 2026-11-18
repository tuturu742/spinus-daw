#include "session/MissingAudio.h"

#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "ui/Playlist.h"

#include <catch2/catch_test_macros.hpp>

using namespace spinus;

namespace
{
/** A short WAV file of silence. */
void writeWav (const juce::File& file)
{
    REQUIRE (file.getParentDirectory().createDirectory());
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = format.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    juce::AudioBuffer<float> silence (1, 4800);
    silence.clear();
    REQUIRE (writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples()));
}

model::Clip audioClip (model::Project& p, const juce::String& file)
{
    model::AudioClip audio;
    audio.file = file;
    return { p.allocateId<model::ClipId>(), p.playlist.lanes.front().id, 0, 3840, false, audio };
}

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}
} // namespace

TEST_CASE ("Missing audio files are listed once and found in a folder's subfolders, in any case", "[missing-audio]")
{
    const juce::TemporaryFile project ("missing-project"), elsewhere ("missing-elsewhere");
    const auto& folder = project.getFile();
    const auto& search = elsewhere.getFile();
    writeWav (folder.getChildFile ("audio/here.wav"));
    writeWav (search.getChildFile ("Stems/Vocals/Lead Vox.WAV"));
    writeWav (search.getChildFile ("drums/kick.wav"));
    writeWav (search.getChildFile ("other/Kick.wav")); // the same name in another case: the exact one wins

    model::Project p;
    p.playlist.lanes.push_back ({ p.allocateId<model::LaneId>(), "Audio" });
    for (auto file : { "here.wav", "lead vox.wav", "kick.wav", "kick.wav", "C:\\Old\\pad.wav" })
        p.playlist.clips.push_back (audioClip (p, file));

    const auto missing = missingaudio::find (p, folder);
    CHECK (missing == juce::StringArray { "lead vox.wav", "kick.wav", "C:\\Old\\pad.wav" });

    const auto found = missingaudio::search (missing, search);
    REQUIRE (found.size() == 2);
    CHECK (found.at ("lead vox.wav") == search.getChildFile ("Stems/Vocals/Lead Vox.WAV"));
    CHECK (found.at ("kick.wav") == search.getChildFile ("drums/kick.wav"));
    CHECK (! found.contains ("C:\\Old\\pad.wav"));
    CHECK (missingaudio::search (missing, search.getChildFile ("nowhere")).empty());

    folder.deleteRecursively();
    search.deleteRecursively();
}

TEST_CASE ("project.relocateAudio points every clip of a file at another, in one undo step",
           "[missing-audio][commands]")
{
    model::Project p;
    p.playlist.lanes.push_back ({ p.allocateId<model::LaneId>(), "Audio" });
    for (auto file : { "a.wav", "a.wav", "b.wav" })
        p.playlist.clips.push_back (audioClip (p, file));
    commands::CommandHistory history (p);

    REQUIRE (history.perform (make ("project.relocateAudio", R"({"files": [{"from": "a.wav", "to": "found/a.wav"}]})"))
                 .wasOk());
    const auto& clips = history.getProject().playlist.clips;
    CHECK (std::get<model::AudioClip> (clips[0].content).file == "found/a.wav");
    CHECK (std::get<model::AudioClip> (clips[1].content).file == "found/a.wav");
    CHECK (std::get<model::AudioClip> (clips[2].content).file == "b.wav");
    CHECK (history.getUndoDescriptions().strings.getLast() == "Find missing audio file");

    // A file no clip plays, or no new file: refused, nothing changed.
    CHECK (
        history.perform (make ("project.relocateAudio", R"({"files": [{"from": "c.wav", "to": "d.wav"}]})")).failed());
    juce::String error;
    CHECK (commands::CommandRegistry::get().create ("project.relocateAudio", juce::JSON::parse (R"({"files": []})"),
                                                    error) == nullptr);

    REQUIRE (history.undo());
    CHECK (history.getProject() == p);
}

TEST_CASE ("Find missing audio files copies what it finds into the project and says what is still missing",
           "[missing-audio][ui]")
{
    const juce::TemporaryFile project ("missing-project"), elsewhere ("missing-elsewhere");
    const auto& folder = project.getFile();
    const auto& search = elsewhere.getFile();
    REQUIRE (folder.getChildFile ("audio").createDirectory());
    writeWav (search.getChildFile ("Bounces/vox.wav"));

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    session.setProjectFolder (folder);
    ui::Playlist playlist { session };
    juce::String said;
    playlist.onProblem = [&said] (const juce::String& text) { said = text; };

    auto p = session.getProject();
    p.playlist.lanes.push_back ({ p.allocateId<model::LaneId>(), "Audio" });
    p.playlist.clips.push_back (audioClip (p, "vox.wav"));
    p.playlist.clips.push_back (audioClip (p, "gone.wav"));
    session.loadProject ({ p, {} });
    CHECK (playlist.getMissingAudioFiles() == juce::StringArray { "vox.wav", "gone.wav" });

    CHECK (playlist.findMissingAudioIn (search) == 1);
    CHECK (folder.getChildFile ("audio/vox.wav").existsAsFile()); // copied in
    CHECK (playlist.getMissingAudioFiles() == juce::StringArray { "gone.wav" });
    CHECK (said.contains ("Found 1 of 2 missing audio files"));
    CHECK (said.contains ("Still missing: gone.wav"));

    folder.deleteRecursively();
    search.deleteRecursively();
}
