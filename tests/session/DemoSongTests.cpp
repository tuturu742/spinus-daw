#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "engine/EngineHost.h"
#include "project/ProjectFiles.h"
#include "session/DemoSong.h"
#include "session/ProjectDocument.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace spinus;

namespace
{
/** The demo song in the source tree, as the packages ship it. */
juce::File sourceDemo() { return juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo").getChildFile ("Siskin"); }

struct TempFolder
{
    TempFolder()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-demo-test-" + juce::Uuid().toString()))
    {
        root.createDirectory();
    }
    ~TempFolder()
    {
        root.setReadOnly (false, true);
        root.deleteRecursively();
    }
    juce::File root;
};

/** A small stand-in for the shipped demo: a project folder with a state and an audio file. */
juce::File makeFakeDemo (const juce::File& at)
{
    at.createDirectory();
    at.getChildFile ("Siskin.spn").replaceWithText ("{\"schemaVersion\": 1}");
    at.getChildFile ("plugin-states").createDirectory();
    at.getChildFile ("plugin-states/3.bin").replaceWithText ("state");
    at.getChildFile ("audio").createDirectory();
    at.getChildFile ("audio/Impact.wav").replaceWithText ("RIFF");
    return at;
}

/** True if the bytes hold a path of someone's computer (a home or temporary folder). */
bool namesThisComputer (const juce::MemoryBlock& bytes)
{
    const auto* begin = static_cast<const char*> (bytes.getData());
    const auto* end = begin + bytes.getSize();
    for (const std::string needle : { "/home/", "/tmp/", "C:\\Users", "/Users/" })
        if (std::search (begin, end, needle.begin(), needle.end()) != end)
            return true;
    return false;
}

/** Every file under a folder, with its size and time: to see that nothing in it changed. */
juce::StringArray describe (const juce::File& folder)
{
    juce::StringArray lines;
    for (const auto& e : juce::RangedDirectoryIterator (
             folder, true, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
        lines.add (e.getFile().getRelativePathFrom (folder) + " " + juce::String (e.getFileSize()) + " " +
                   juce::String (e.getModificationTime().toMilliseconds()));
    lines.sort (false);
    return lines;
}
} // namespace

TEST_CASE ("The shipped demo song is found where each package puts it", "[session][demo]")
{
    TempFolder temp;

    // Arch, AppImage, Flatpak: <prefix>/lib/spinus/spinus and <prefix>/share/spinus/demo.
    const auto prefix = temp.root.getChildFile ("usr");
    const auto installed = makeFakeDemo (prefix.getChildFile ("share/spinus/demo/Siskin"));
    prefix.getChildFile ("lib/spinus").createDirectory();
    CHECK (demo::findBundledDemo (prefix.getChildFile ("lib/spinus")) == installed);
    prefix.getChildFile ("lib/x86_64-linux-gnu/spinus").createDirectory();
    CHECK (demo::findBundledDemo (prefix.getChildFile ("lib/x86_64-linux-gnu/spinus")) == installed);

    // Windows: beside spinus.exe.
    const auto windows = temp.root.getChildFile ("Spinus");
    const auto beside = makeFakeDemo (windows.getChildFile ("demo/Siskin"));
    CHECK (demo::findBundledDemo (windows) == beside);

    // A development build: the source tree's.
    CHECK (demo::findBundledDemo (temp.root.getChildFile ("nowhere")) == sourceDemo());
}

TEST_CASE ("Every demo song in the demo folder is listed, Siskin first", "[session][demo]")
{
    TempFolder temp;
    const auto prefix = temp.root.getChildFile ("usr");
    const auto demos = prefix.getChildFile ("share/spinus/demo");
    makeFakeDemo (demos.getChildFile ("Siskin"));
    for (auto name : { "Night Drive", "Acid Morning", "Song 10", "Song 9" })
    {
        demos.getChildFile (name).createDirectory();
        demos.getChildFile (name)
            .getChildFile (juce::String (name) + ".spn")
            .replaceWithText ("{\"schemaVersion\": 1}");
    }
    demos.getChildFile ("README.md").replaceWithText ("# Demo songs");
    demos.getChildFile ("Not a song").createDirectory();
    demos.getChildFile (".hidden").createDirectory();
    demos.getChildFile (".hidden/.hidden.spn").replaceWithText ("{}");

    juce::StringArray names;
    for (auto& f : demo::listDemos (demos))
        names.add (f.getFileName());
    CHECK (names == juce::StringArray { "Siskin", "Acid Morning", "Night Drive", "Song 9", "Song 10" });

    prefix.getChildFile ("lib/spinus").createDirectory();
    CHECK (demo::findDemoFolder (prefix.getChildFile ("lib/spinus")) == demos);
    CHECK (demo::findBundledDemo (prefix.getChildFile ("lib/spinus")) == demos.getChildFile ("Siskin"));
    CHECK (demo::listDemos (temp.root.getChildFile ("nowhere")).isEmpty());

    // A song's copy is named after it.
    const auto copy = demo::copyDemo (demos.getChildFile ("Night Drive"), temp.root.getChildFile ("Music"));
    REQUIRE (copy.result.wasOk());
    CHECK (copy.projectFile == temp.root.getChildFile ("Music/Night Drive/Night Drive.spn"));

    // The source tree's demo folder has the showcase, first.
    const auto source = demo::listDemos (sourceDemo().getParentDirectory());
    REQUIRE (! source.isEmpty());
    CHECK (source.getFirst() == sourceDemo());
}

TEST_CASE ("Every shipped demo song plays on Linux and Windows alike", "[session][demo]")
{
    // LSP Plugins and Odin 2 are not offered on Windows: no demo song may need them.
    const auto demos = demo::listDemos (sourceDemo().getParentDirectory());
    REQUIRE (demos.size() >= 4);
    for (auto& folder : demos)
    {
        const auto text = folder.getChildFile (folder.getFileName() + ".spn").loadFileAsString();
        INFO (folder.getFileName());
        REQUIRE (text.isNotEmpty());
        CHECK (! text.contains ("\"bundled.lsp."));
        CHECK (! text.contains ("\"bundled.odin2\""));
    }
}

TEST_CASE ("The demo song is copied to the user's folder, never written where it is installed", "[session][demo]")
{
    TempFolder temp;
    const auto bundled = makeFakeDemo (temp.root.getChildFile ("share/spinus/demo/Siskin"));
    bundled.getParentDirectory().setReadOnly (true, true); // as in /usr/share: any write there would fail
    const auto before = describe (bundled.getParentDirectory());
    const auto music = temp.root.getChildFile ("home/Music");

    // The first time: a copy, which the user can change.
    const auto first = demo::copyDemo (bundled, music);
    INFO (first.result.getErrorMessage());
    REQUIRE (first.result.wasOk());
    CHECK (! first.alreadyThere);
    CHECK (first.projectFile == music.getChildFile ("Siskin/Siskin.spn"));
    CHECK (first.projectFile.loadFileAsString() == "{\"schemaVersion\": 1}");
    CHECK (music.getChildFile ("Siskin/plugin-states/3.bin").loadFileAsString() == "state");
    CHECK (music.getChildFile ("Siskin/audio/Impact.wav").existsAsFile());
    CHECK (first.projectFile.hasWriteAccess());
    CHECK (music.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*") == 1); // nothing half-copied left

    // Again: the same copy, with the user's changes.
    first.projectFile.replaceWithText ("{\"schemaVersion\": 1, \"changed\": true}");
    const auto second = demo::copyDemo (bundled, music);
    REQUIRE (second.result.wasOk());
    CHECK (second.alreadyThere);
    CHECK (second.projectFile == first.projectFile);
    CHECK (second.projectFile.loadFileAsString().contains ("changed"));

    // A folder by that name that is not a project is left alone.
    const auto other = temp.root.getChildFile ("other");
    other.getChildFile ("Siskin").createDirectory();
    other.getChildFile ("Siskin/notes.txt").replaceWithText ("mine");
    const auto third = demo::copyDemo (bundled, other);
    REQUIRE (third.result.wasOk());
    CHECK (third.projectFile == other.getChildFile ("Siskin 2/Siskin 2.spn"));
    CHECK (other.getChildFile ("Siskin").getNumberOfChildFiles (juce::File::findFiles) == 1);

    CHECK (describe (bundled.getParentDirectory()) == before);

    // No demo installed: a problem to show, nothing made.
    const auto none = demo::copyDemo (temp.root.getChildFile ("missing"), temp.root.getChildFile ("empty"));
    CHECK (none.result.failed());
    CHECK (! temp.root.getChildFile ("empty").exists());
}

TEST_CASE ("The shipped demo song reads, and its copy opens as a project", "[session][demo]")
{
    const auto shipped = sourceDemo();
    REQUIRE (project::isProjectFolder (shipped));

    // Every device state and audio file it names is in the folder, and nothing names this computer.
    project::ProjectData data;
    const auto read = project::loadProject (shipped, data);
    INFO (read.getErrorMessage());
    REQUIRE (read.wasOk());
    CHECK (data.project.channels.size() > 20);
    CHECK (data.project.patterns.size() > 10);
    CHECK (data.deviceStates.size() > 40);
    for (const auto& clip : data.project.playlist.clips)
        if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
            CHECK (shipped.getChildFile ("audio").getChildFile (audio->file).existsAsFile());
    for (const auto& e : juce::RangedDirectoryIterator (shipped, true, "*", juce::File::findFiles))
    {
        juce::MemoryBlock bytes;
        REQUIRE (e.getFile().loadFileAsData (bytes));
        std::vector<juce::MemoryBlock> blobs { bytes };
        // A plugin's own state is inside its device state, encoded: look in it too.
        if (const auto tree = juce::ValueTree::readFromData (bytes.getData(), bytes.getSize()); tree.isValid())
            if (juce::MemoryBlock inner; inner.fromBase64Encoding (tree["state"].toString()))
                blobs.push_back (inner);
        INFO (e.getFile().getFullPathName());
        for (const auto& blob : blobs)
            CHECK (! namesThisComputer (blob));
    }
    CHECK (! shipped.getChildFile ("Siskin.wav").exists()); // the rendered song is not shipped

    // A copy opens through the normal path (the test devices stand in for the plugins).
    juce::File copied;
    const juce::File music = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("spinus-demo-open-" + juce::Uuid().toString());
    {
        const auto copy = demo::copyDemo (shipped, music);
        REQUIRE (copy.result.wasOk());
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
        ProjectDocument document (session, music.getChildFile ("settings"));
        const auto opened = document.open (copy.projectFile);
        INFO (opened.getErrorMessage());
        CHECK (opened.wasOk());
        CHECK (document.getName() == "Siskin");
        CHECK (document.getFolder() == music.getChildFile ("Siskin"));
        CHECK (session.getProject().channels.size() == data.project.channels.size());
        CHECK (! document.hasUnsavedChanges());
    }
    music.deleteRecursively();
}

TEST_CASE ("The bundled Neon Coast demo song reads, and names nothing of this computer", "[session][demo]")
{
    const auto shipped = juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo").getChildFile ("Neon Coast");
    REQUIRE (project::isProjectFolder (shipped));
    CHECK (shipped.getChildFile ("LICENSE.txt").loadFileAsString().contains ("CC BY 4.0"));

    project::ProjectData data;
    const auto read = project::loadProject (shipped, data);
    INFO (read.getErrorMessage());
    REQUIRE (read.wasOk());
    CHECK (data.project.channels.size() > 15);
    CHECK (data.project.patterns.size() > 30);
    CHECK (data.deviceStates.size() > 30);

    // Its audio clips' files are in its audio folder.
    int audioClips = 0;
    for (const auto& clip : data.project.playlist.clips)
        if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
        {
            ++audioClips;
            CHECK (shipped.getChildFile ("audio").getChildFile (audio->file).existsAsFile());
        }
    CHECK (audioClips > 0);

    // No path of the computer it was made on, in any file or inside a plugin's own state.
    for (const auto& e : juce::RangedDirectoryIterator (shipped, true, "*", juce::File::findFiles))
    {
        juce::MemoryBlock bytes;
        REQUIRE (e.getFile().loadFileAsData (bytes));
        std::vector<juce::MemoryBlock> blobs { bytes };
        if (const auto tree = juce::ValueTree::readFromData (bytes.getData(), bytes.getSize()); tree.isValid())
            if (juce::MemoryBlock inner; inner.fromBase64Encoding (tree["state"].toString()))
                blobs.push_back (inner);
        INFO (e.getFile().getFullPathName());
        for (const auto& blob : blobs)
            CHECK (! namesThisComputer (blob));
    }
    CHECK (! shipped.getChildFile ("Neon Coast.wav").exists()); // the rendered song is not shipped
    CHECK (! shipped.getChildFile (".autosave").exists());
}

TEST_CASE ("The liquid drum & bass demo song reads, names nothing of this computer and opens", "[session][demo]")
{
    const auto shipped = juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo").getChildFile ("Sodium Lights");
    REQUIRE (project::isProjectFolder (shipped));

    project::ProjectData data;
    const auto read = project::loadProject (shipped, data);
    INFO (read.getErrorMessage());
    REQUIRE (read.wasOk());
    CHECK (std::abs (data.project.settings.tempoBpm - 174.0) < 1.0e-9);
    CHECK (data.project.channels.size() >= 12);
    CHECK (data.project.patterns.size() >= 20);
    CHECK (data.project.launcher.scenes.size() >= 6);
    CHECK (data.project.playlist.markers.size() == 6);
    CHECK (std::any_of (data.project.channels.begin(), data.project.channels.end(), [] (const model::Channel& c)
                        { return c.instrument && c.instrument->stockType == "spinus.beat-slicer"; }));
    int audioClips = 0;
    for (const auto& clip : data.project.playlist.clips)
        if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
        {
            ++audioClips;
            CHECK (shipped.getChildFile ("audio").getChildFile (audio->file).existsAsFile());
        }
    CHECK (audioClips > 0);
    for (const auto& e : juce::RangedDirectoryIterator (shipped, true, "*", juce::File::findFiles))
    {
        juce::MemoryBlock bytes;
        REQUIRE (e.getFile().loadFileAsData (bytes));
        std::vector<juce::MemoryBlock> blobs { bytes };
        if (const auto tree = juce::ValueTree::readFromData (bytes.getData(), bytes.getSize()); tree.isValid())
            if (juce::MemoryBlock inner; inner.fromBase64Encoding (tree["state"].toString()))
                blobs.push_back (inner);
        INFO (e.getFile().getFullPathName());
        for (const auto& blob : blobs)
            CHECK (! namesThisComputer (blob));
    }
    CHECK (! shipped.getChildFile ("Sodium Lights.wav").exists());
    CHECK (! shipped.getChildFile ("cache").exists());

    const juce::File music = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("spinus-demo-open-" + juce::Uuid().toString());
    {
        const auto copy = demo::copyDemo (shipped, music);
        REQUIRE (copy.result.wasOk());
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
        ProjectDocument document (session, music.getChildFile ("settings"));
        const auto opened = document.open (copy.projectFile);
        INFO (opened.getErrorMessage());
        CHECK (opened.wasOk());
        CHECK (document.getName() == "Sodium Lights");
        CHECK (session.getProject().channels.size() == data.project.channels.size());
        CHECK (! document.hasUnsavedChanges());
    }
    music.deleteRecursively();
}

TEST_CASE ("The lo-fi demo song Rainy Window reads, names nothing of this computer and opens", "[session][demo]")
{
    const auto shipped = juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo").getChildFile ("Rainy Window");
    REQUIRE (project::isProjectFolder (shipped));

    project::ProjectData data;
    const auto read = project::loadProject (shipped, data);
    INFO (read.getErrorMessage());
    REQUIRE (read.wasOk());
    CHECK (data.project.channels.size() >= 10);
    CHECK (data.project.patterns.size() >= 20);
    CHECK (data.deviceStates.size() >= 30);

    // Its audio clips are in its folder: three takes of one line on one track, comped (some parts muted).
    int audioClips = 0, mutedTakes = 0;
    for (const auto& clip : data.project.playlist.clips)
        if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
        {
            ++audioClips;
            mutedTakes += clip.muted ? 1 : 0;
            CHECK (shipped.getChildFile ("audio").getChildFile (audio->file).existsAsFile());
        }
    CHECK (audioClips >= 10);
    CHECK (mutedTakes >= 6);

    for (const auto& e : juce::RangedDirectoryIterator (shipped, true, "*", juce::File::findFiles))
    {
        juce::MemoryBlock bytes;
        REQUIRE (e.getFile().loadFileAsData (bytes));
        std::vector<juce::MemoryBlock> blobs { bytes };
        if (const auto tree = juce::ValueTree::readFromData (bytes.getData(), bytes.getSize()); tree.isValid())
            if (juce::MemoryBlock inner; inner.fromBase64Encoding (tree["state"].toString()))
                blobs.push_back (inner);
        INFO (e.getFile().getFullPathName());
        for (const auto& blob : blobs)
            CHECK (! namesThisComputer (blob));
    }
    CHECK (! shipped.getChildFile ("Rainy Window.wav").exists()); // the rendered song is not shipped

    const juce::File music = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("spinus-demo-open-" + juce::Uuid().toString());
    {
        // A copy, as the user's (the Help menu's copying is tested above with the showcase).
        const auto copy = music.getChildFile ("Rainy Window");
        REQUIRE (shipped.copyDirectoryTo (copy));
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));
        ProjectDocument document (session, music.getChildFile ("settings"));
        const auto opened = document.open (copy.getChildFile ("Rainy Window.spn"));
        INFO (opened.getErrorMessage());
        CHECK (opened.wasOk());
        CHECK (document.getName() == "Rainy Window");
        CHECK (session.getProject().channels.size() == data.project.channels.size());
        CHECK (! document.hasUnsavedChanges());
    }
    music.deleteRecursively();
}
