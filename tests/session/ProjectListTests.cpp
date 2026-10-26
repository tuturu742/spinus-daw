#include <catch2/catch_test_macros.hpp>

#include "session/ProjectList.h"

using namespace spinus;

namespace
{
struct Temp
{
    Temp() { root.createDirectory(); }
    ~Temp() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-project-list-" + juce::Uuid().toString());
};

/** A project folder as Spinus saves it: <name>/<name>.spn, with audio/ inside. */
juce::File makeProject (const juce::File& folder)
{
    folder.getChildFile ("audio").createDirectory();
    folder.getChildFile (folder.getFileName() + ".spn").replaceWithText ("{\"schemaVersion\": 1}");
    return folder;
}

juce::StringArray names (const juce::Array<juce::File>& files, const juce::File& root)
{
    juce::StringArray result;
    for (auto& f : files)
        result.add (f.getRelativePathFrom (root).replaceCharacter ('\\', '/'));
    return result;
}
} // namespace

TEST_CASE ("Folders are searched for projects: into subfolders, not into projects or hidden folders",
           "[session][projects]")
{
    Temp t;
    const auto music = t.root.getChildFile ("Music");
    makeProject (music.getChildFile ("Beat"));
    makeProject (music.getChildFile ("2024/Summer/Song"));
    makeProject (music.getChildFile ("Beat/audio/Inside")); // inside a project: its own files, not listed
    makeProject (music.getChildFile (".hidden/Secret"));
    makeProject (music.getChildFile ("node_modules/Thing"));
    makeProject (music.getChildFile ("a/b/c/d/e/f/g/Deep")); // deeper than the limit
    music.getChildFile ("Old.spinus").createDirectory();     // the older format
    music.getChildFile ("Old.spinus/project.json").replaceWithText ("{}");
    music.getChildFile ("Code").createDirectory(); // a project.json that is not Spinus's
    music.getChildFile ("Code/project.json").replaceWithText ("{}");
    music.getChildFile ("Samples/Kick.wav").create();

    bool truncated = true;
    CHECK (names (ProjectList::findProjects (music, ProjectList::maxDepth, ProjectList::maxFoldersPerScan, nullptr,
                                             &truncated),
                  music) == juce::StringArray { "2024/Summer/Song", "Beat", "Old.spinus" });
    CHECK (! truncated);

    // The limits: how deep, and how many folders.
    CHECK (names (ProjectList::findProjects (music, 1), music) == juce::StringArray { "Beat", "Old.spinus" });
    CHECK (names (ProjectList::findProjects (music, 8), music).contains ("a/b/c/d/e/f/g/Deep"));
    ProjectList::findProjects (music, ProjectList::maxDepth, 3, nullptr, &truncated);
    CHECK (truncated);

#if ! JUCE_WINDOWS
    // A link to a folder is not followed (it could lead back up, or to the whole disk).
    REQUIRE (music.createSymbolicLink (music.getChildFile ("Link"), true));
    CHECK (names (ProjectList::findProjects (music), music).size() == 3);
#endif

    // A cancelled search stops.
    std::atomic<bool> cancel { true };
    CHECK (ProjectList::findProjects (music, 6, 1000, &cancel).isEmpty());
    // The folder itself can be the project.
    CHECK (ProjectList::findProjects (music.getChildFile ("Beat")) ==
           juce::Array<juce::File> { music.getChildFile ("Beat") });
    CHECK (ProjectList::findProjects (t.root.getChildFile ("nowhere")).isEmpty());
}

TEST_CASE ("The project list: folders, projects opened in them, kept in the settings", "[session][projects]")
{
    Temp t;
    const auto settings = t.root.getChildFile ("settings/projects.json");
    const auto a = makeProject (t.root.getChildFile ("Elsewhere/Alpha"));
    const auto library = t.root.getChildFile ("Library");
    const auto c = makeProject (library.getChildFile ("Charlie"));
    const auto d = makeProject (library.getChildFile ("Sets/Delta"));

    ProjectList list;
    list.setFile (settings);
    list.waitForScan();
    CHECK (list.getAllProjects().isEmpty());

    // A project opened outside the folders is not listed (File › Open recent has it).
    list.noteUsed (a.getChildFile ("Alpha.spn"));
    CHECK (list.getAllProjects().isEmpty());

    // A folder: searched in the background; its projects are listed once.
    list.noteUsed (c);
    REQUIRE (list.addFolder (library));
    CHECK (! list.addFolder (library));                         // already there
    CHECK (! list.addFolder (library.getChildFile ("Sets")));   // inside it
    CHECK (! list.addFolder (t.root.getChildFile ("nowhere"))); // not a folder
    list.waitForScan();
    REQUIRE (list.getFolders().size() == 1);
    REQUIRE (list.getFolders()[0].projects.size() == 2);
    CHECK (list.getFolders()[0].projects[0].folder == c);
    CHECK (list.getFolders()[0].projects[0].name == "Charlie");
    CHECK (list.getFolders()[0].projects[1].folder == d);
    CHECK (list.getAllProjects().size() == 2);

    // A project saved into the folder is there at once (a .spn file means its folder).
    const auto e = makeProject (library.getChildFile ("Echo"));
    list.noteUsed (e.getChildFile ("Echo.spn"));
    CHECK (list.getFolders()[0].projects.size() == 3);

    // Gone from the disk: it drops out at the next search.
    d.deleteRecursively();
    list.rescan();
    list.waitForScan();
    CHECK (list.getFolders()[0].projects.size() == 2);

    // Removed from the list: hidden at the next search, until opened again.
    list.remove (e);
    list.rescan();
    list.waitForScan();
    CHECK (list.getFolders()[0].projects.size() == 1);
    CHECK (e.isDirectory()); // the files stay

    // Kept: the next start lists the same at once, before searching.
    {
        ProjectList again;
        again.setFile (settings);
        REQUIRE (again.getFolders().size() == 1);
        CHECK (again.getFolders()[0].projects.size() == 1);
        again.waitForScan();
        again.noteUsed (e); // opened again: back in its folder
        again.waitForScan();
        CHECK (again.getFolders()[0].projects.size() == 2);
    }

    // A folder added above one in the list takes its place; a folder removed takes its projects with it.
    ProjectList third;
    third.setFile (settings);
    third.waitForScan();
    CHECK (third.getFolders()[0].projects.size() == 2);
    REQUIRE (third.addFolder (t.root));
    third.waitForScan();
    REQUIRE (third.getFolders().size() == 1);
    CHECK (third.getFolders()[0].folder == t.root);
    CHECK (third.getFolders()[0].projects.size() == 3); // Alpha, Charlie, Echo
    third.removeFolder (t.root);
    CHECK (third.getFolders().empty());
    CHECK (third.getAllProjects().isEmpty());

    // A folder that went away is shown as missing.
    const auto gone = t.root.getChildFile ("Gone");
    gone.createDirectory();
    REQUIRE (third.addFolder (gone));
    gone.deleteRecursively();
    third.rescan();
    third.waitForScan();
    CHECK (third.getFolders()[0].missing);
}

TEST_CASE ("A list saved with opened projects (before 0.1.0-alpha.3) still loads", "[session][projects]")
{
    Temp t;
    const auto settings = t.root.getChildFile ("projects.json");
    const auto library = t.root.getChildFile ("Library");
    const auto a = makeProject (library.getChildFile ("Alpha"));
    settings.replaceWithText (R"({"opened": [")" + a.getFullPathName().replace ("\\", "\\\\") +
                              R"("], "folders": [{"path": ")" + library.getFullPathName().replace ("\\", "\\\\") +
                              R"(", "projects": []}], "hidden": []})");
    ProjectList list;
    list.setFile (settings);
    list.waitForScan();
    REQUIRE (list.getFolders().size() == 1);
    CHECK (list.getAllProjects() == juce::Array<juce::File> { a });
    CHECK (! settings.loadFileAsString().contains ("opened")); // dropped when the list is kept again
}

TEST_CASE ("Searching many projects stays quick", "[session][projects]")
{
    Temp t;
    for (int i = 0; i < 400; ++i)
        makeProject (t.root.getChildFile ("Year " + juce::String (i % 8) + "/Song " + juce::String (i)));
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto found = ProjectList::findProjects (t.root);
    CHECK (found.size() == 400);
    CHECK (juce::Time::getMillisecondCounterHiRes() - start < 5000.0);

    ProjectList list;
    REQUIRE (list.addFolder (t.root));
    for (int i = 0; i < 400; ++i)
        list.noteUsed (t.root.getChildFile ("Year " + juce::String (i % 8) + "/Song " + juce::String (i)));
    list.waitForScan();
    CHECK (list.getFolders()[0].projects.size() == 400);
    CHECK (list.getAllProjects().size() == 400); // each is listed once
}
