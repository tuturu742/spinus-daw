#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "../model/ExampleProject.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "session/ProjectDocument.h"

using namespace spinus;
namespace te = tracktion;

namespace
{

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

void requireOk (const juce::Result& r)
{
    INFO (r.getErrorMessage());
    REQUIRE (r.wasOk());
}

struct TempFolder
{
    TempFolder()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::String ("spinus-doc-test") + "-" + juce::Uuid().toString()))
    {
        root.createDirectory();
    }

    ~TempFolder() { root.deleteRecursively(); }

    juce::File root;
};

/** A project with the example's content; the settings folder (recent list, Unsaved/) is temporary. */
struct Fixture
{
    Fixture()
        : host ({ .openAudioDevice = false }),
          session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()))
    {
        session.getHistory().reset (test::makeExample());
        document = std::make_unique<ProjectDocument> (session, temp.root.getChildFile ("settings"));
    }

    te::CompressorPlugin* compressor() // the example's drums insert compressor (device 3)
    {
        return dynamic_cast<te::CompressorPlugin*> (session.getAdapter().getDevicePlugin (model::DeviceId { 3 }));
    }

    void addPattern() { requireOk (session.getHistory().perform (make ("pattern.add", R"({"length": 3840})"))); }

    /** What a crash leaves behind: the lock of a process that is gone. */
    static void simulateCrash (const juce::File& folder)
    {
        folder.getChildFile (".session.lock").replaceWithText ("pid 999999999\nopened earlier\n");
    }

    TempFolder temp;
    EngineHost host;
    Session session;
    std::unique_ptr<ProjectDocument> document;
};

} // namespace

TEST_CASE ("Save, edit, undo back to the saved state, and open again", "[session][document]")
{
    Fixture f;
    CHECK (f.document->isUntitled());
    CHECK (f.document->getName() == "Untitled");
    CHECK (! f.document->hasUnsavedChanges());
    CHECK (f.document->save().failed()); // no folder yet

    REQUIRE (f.compressor() != nullptr);
    f.compressor()->setThreshold (0.25f);
    const auto threshold = f.compressor()->getThreshold();
    CHECK (! f.document->hasUnsavedEdits());
    CHECK (f.document->hasUnsavedChanges()); // a device setting changed

    const auto folder = f.temp.root.getChildFile ("Song"); // "" is added
    requireOk (f.document->saveAs (folder));
    CHECK (f.document->getFolder() == f.temp.root.getChildFile ("Song"));
    CHECK (f.document->getName() == "Song");
    CHECK (! f.document->hasUnsavedChanges());
    CHECK (project::SessionLock::getState (f.document->getFolder()) == project::SessionLock::State::openInSpinus);
    CHECK (f.document->getRecentProjects() == juce::Array<juce::File> { f.document->getFolder() });

    // An edit is unsaved; undoing it returns to the saved state.
    f.addPattern();
    CHECK (f.document->hasUnsavedEdits());
    REQUIRE (f.session.getHistory().undo());
    CHECK (! f.document->hasUnsavedEdits());
    REQUIRE (f.session.getHistory().redo());
    CHECK (f.document->hasUnsavedEdits());
    const auto edited = f.session.getProject();
    requireOk (f.document->save());
    CHECK (! f.document->hasUnsavedChanges());

    // A new project replaces everything; opening brings the saved one back, devices included.
    f.document->newProject();
    CHECK (f.document->isUntitled());
    CHECK (f.session.getProject() == model::Project());
    CHECK (! f.session.getHistory().canUndo());
    CHECK (project::SessionLock::getState (f.temp.root.getChildFile ("Song")) == project::SessionLock::State::none);

    requireOk (f.document->open (f.temp.root.getChildFile ("Song/Song.spn")));
    CHECK (f.session.getProject() == edited);
    CHECK (! f.session.getHistory().canUndo());
    CHECK (! f.document->hasUnsavedChanges());
    REQUIRE (f.compressor() != nullptr);
    CHECK (juce::approximatelyEqual (f.compressor()->getThreshold(), threshold));
    CHECK (f.session.getCurrentPattern() == edited.patterns.front().id);

    // Closing cleanly removes the lock.
    f.document = nullptr;
    CHECK (project::SessionLock::getState (f.temp.root.getChildFile ("Song")) == project::SessionLock::State::none);
}

TEST_CASE ("Opening something that is not a project changes nothing", "[session][document]")
{
    Fixture f;
    f.addPattern();
    const auto before = f.session.getProject();

    CHECK (f.document->open (f.temp.root).failed());
    f.temp.root.getChildFile ("Broken/Broken.spn").create();
    CHECK (f.document->open (f.temp.root.getChildFile ("Broken")).failed());

    CHECK (f.session.getProject() == before);
    CHECK (f.document->isUntitled());
    CHECK (f.document->hasUnsavedEdits());
}

TEST_CASE ("A project open in another window is not opened or overwritten twice", "[session][document]")
{
    Fixture f;
    const auto folder = f.temp.root.getChildFile ("Shared");
    requireOk (f.document->saveAs (folder));

    Session other (f.host.getEngine(), std::make_unique<test::TestDevices> (f.host.getEngine()));
    ProjectDocument otherDocument (other, f.temp.root.getChildFile ("settings2"));
    CHECK (otherDocument.open (folder).failed());
    CHECK (otherDocument.saveAs (folder).failed());
    CHECK (otherDocument.isUntitled());

    // Reopening one's own project is fine.
    requireOk (f.document->open (folder));
}

TEST_CASE ("Save As takes the project's audio files along", "[session][document]")
{
    Fixture f;
    requireOk (f.document->saveAs (f.temp.root.getChildFile ("A")));
    f.temp.root.getChildFile ("A/audio/drums/loop.wav").create();
    f.temp.root.getChildFile ("A/audio/drums/loop.wav").replaceWithText ("not really audio");

    requireOk (f.document->saveAs (f.temp.root.getChildFile ("B")));
    CHECK (f.temp.root.getChildFile ("B/audio/drums/loop.wav").loadFileAsString() == "not really audio");
    CHECK (f.document->getName() == "B");
    CHECK (f.document->getRecentProjects().getFirst() == f.temp.root.getChildFile ("B"));
    CHECK (f.document->getRecentProjects().size() == 2);
}

TEST_CASE ("Save As makes a folder with a .spn file, never mixed into another folder", "[session][document]")
{
    Fixture f;
    requireOk (f.document->saveAs (f.temp.root.getChildFile ("Song.spn")));
    CHECK (f.document->getFolder() == f.temp.root.getChildFile ("Song"));
    CHECK (f.temp.root.getChildFile ("Song/Song.spn").existsAsFile());

    const auto music = f.temp.root.getChildFile ("Music");
    REQUIRE (music.getChildFile ("track.mp3").create());
    CHECK (f.document->saveAs (music).getErrorMessage().contains ("other files"));
    CHECK (f.document->getFolder() == f.temp.root.getChildFile ("Song"));
    CHECK (! music.getChildFile ("Music.spn").exists());

    // An older "Name.spinus" project shows its name without the ending.
    REQUIRE (f.temp.root.getChildFile ("Old.spinus").createDirectory());
    REQUIRE (f.temp.root.getChildFile ("Old.spinus/project.json")
                 .replaceWithText (f.temp.root.getChildFile ("Song/Song.spn").loadFileAsString()));
    requireOk (f.document->open (f.temp.root.getChildFile ("Old.spinus/project.json")));
    CHECK (f.document->getName() == "Old");
}

TEST_CASE ("Autosave writes only when something changed", "[session][document]")
{
    Fixture f;
    requireOk (f.document->saveAs (f.temp.root.getChildFile ("Song")));
    const project::Autosaves autosaves (f.document->getFolder());

    requireOk (f.document->autosave());
    CHECK (autosaves.list().isEmpty()); // nothing new since saving

    f.addPattern();
    requireOk (f.document->autosave());
    CHECK (autosaves.list().size() == 1);
    requireOk (f.document->autosave());
    CHECK (autosaves.list().size() == 1);

    f.compressor()->setThreshold (0.5f); // device settings count too
    requireOk (f.document->autosave());
    CHECK (autosaves.list().size() == 2);
}

TEST_CASE ("After a crash, a saved project's autosave can be recovered", "[session][document]")
{
    Fixture f;
    const auto folder = f.temp.root.getChildFile ("Song");
    requireOk (f.document->saveAs (folder));
    f.addPattern();
    const auto unsaved = f.session.getProject();
    requireOk (f.document->autosave());

    // The session "crashes": the lock stays behind, naming a process that is gone.
    f.document = nullptr;
    Fixture::simulateCrash (folder);

    f.session.getHistory().reset ({});
    f.document = std::make_unique<ProjectDocument> (f.session, f.temp.root.getChildFile ("settings"));
    const auto recoveries = f.document->findRecoveries();
    REQUIRE (recoveries.size() == 1);
    CHECK (recoveries[0].projectFolder == folder);
    CHECK (recoveries[0].name == "Song");
    CHECK (! recoveries[0].untitled);

    requireOk (f.document->recover (recoveries[0]));
    CHECK (f.session.getProject() == unsaved);
    CHECK (f.document->getFolder() == folder);
    CHECK (f.document->hasUnsavedEdits()); // not in the saved project yet
    CHECK (f.document->findRecoveries().empty());

    requireOk (f.document->save());
    CHECK (! f.document->hasUnsavedChanges());
}

TEST_CASE ("After a crash, an untitled project's autosave can be recovered or discarded", "[session][document]")
{
    Fixture f;
    const auto unsavedRoot = f.temp.root.getChildFile ("settings/Unsaved");
    f.addPattern();
    requireOk (f.document->autosave());
    auto folders = unsavedRoot.findChildFiles (juce::File::findDirectories, false);
    REQUIRE (folders.size() == 1);
    const auto project = f.session.getProject();

    // A clean close deletes an untitled project's autosaves.
    f.document = nullptr;
    CHECK (! folders[0].exists());

    // A crash keeps them, and the next start offers them.
    f.document = std::make_unique<ProjectDocument> (f.session, f.temp.root.getChildFile ("settings"));
    f.addPattern();
    requireOk (f.document->autosave());
    const auto crashedProject = f.session.getProject();
    folders = unsavedRoot.findChildFiles (juce::File::findDirectories, false);
    REQUIRE (folders.size() == 1);
    Fixture::simulateCrash (folders[0]);

    Session next (f.host.getEngine(), std::make_unique<test::TestDevices> (f.host.getEngine()));
    {
        ProjectDocument document (next, f.temp.root.getChildFile ("settings"));
        auto recoveries = document.findRecoveries();
        REQUIRE (recoveries.size() == 1);
        CHECK (recoveries[0].untitled);

        requireOk (document.recover (recoveries[0]));
        CHECK (next.getProject() == crashedProject);
        CHECK (document.isUntitled());
        CHECK (document.hasUnsavedEdits());

        // Saving it somewhere drops the untitled autosaves.
        requireOk (document.saveAs (f.temp.root.getChildFile ("Kept")));
        CHECK (! folders[0].exists());
    }

    // Discarding deletes an untitled project's autosaves.
    f.document = std::make_unique<ProjectDocument> (f.session, f.temp.root.getChildFile ("settings"));
    f.addPattern();
    requireOk (f.document->autosave());
    folders = unsavedRoot.findChildFiles (juce::File::findDirectories, false);
    REQUIRE (folders.size() == 1);
    Fixture::simulateCrash (folders[0]);
    const auto recovery = ProjectDocument::findRecovery (folders[0], true);
    REQUIRE (recovery.has_value());
    ProjectDocument::discard (recovery.value_or (ProjectDocument::Recovery {}));
    CHECK (! folders[0].exists());
}
