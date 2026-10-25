#include <catch2/catch_test_macros.hpp>

#include "session/ProblemReport.h"

using namespace spinus::session;

namespace
{
struct Folders
{
    Folders()
    {
        root.deleteRecursively();
        logs.createDirectory();
        // Four runs; the newest has an AI service log too.
        for (auto* name :
             { "spinus-2026-10-20_10-00-00.log", "spinus-2026-10-21_10-00-00.log", "spinus-2026-10-22_10-00-00.log",
               "spinus-2026-10-23_10-00-00.log", "spinus-2026-10-23_10-00-00-ai.log" })
            logs.getChildFile (name).replaceWithText (juce::String ("log of ") + name);
        logs.getChildFile ("notes.txt").replaceWithText ("not a log");
    }

    ~Folders() { root.deleteRecursively(); }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-report-test");
    juce::File logs = root.getChildFile ("logs"), reports = root.getChildFile ("reports");
};

juce::StringArray entriesOf (const juce::File& zip)
{
    juce::ZipFile file (zip);
    juce::StringArray names;
    for (int i = 0; i < file.getNumEntries(); ++i)
        names.add (file.getEntry (i)->filename);
    names.sort (false);
    return names;
}
} // namespace

TEST_CASE ("A problem report holds the last runs' logs and the version", "[session][report]")
{
    Folders f;
    juce::File zip;
    REQUIRE (problemReport::make (f.reports, f.logs, { "Spinus DAW 0.1.0-alpha.2", "Package: Flatpak" }, zip,
                                  juce::Time (2026, 9, 25, 12, 0))
                 .wasOk());
    CHECK (zip.getFileName() == "spinus-report-2026-10-25_12-00-00.zip");
    CHECK (zip.getParentDirectory() == f.reports);

    // The newest three runs, the AI service's log with its run; not the oldest run, not other files.
    CHECK (entriesOf (zip) == juce::StringArray { "about.txt", "logs/spinus-2026-10-21_10-00-00.log",
                                                  "logs/spinus-2026-10-22_10-00-00.log",
                                                  "logs/spinus-2026-10-23_10-00-00-ai.log",
                                                  "logs/spinus-2026-10-23_10-00-00.log" });

    juce::ZipFile file (zip);
    std::unique_ptr<juce::InputStream> about (file.createStreamForEntry (*file.getEntry ("about.txt")));
    REQUIRE (about != nullptr);
    CHECK (about->readEntireStreamAsString() == "Spinus DAW 0.1.0-alpha.2\nPackage: Flatpak\n");
}

TEST_CASE ("Only the newest problem reports are kept", "[session][report]")
{
    Folders f;
    for (int minute = 0; minute < problemReport::keptReports + 2; ++minute)
    {
        juce::File zip;
        REQUIRE (
            problemReport::make (f.reports, f.logs, { "Spinus" }, zip, juce::Time (2026, 9, 25, 12, minute)).wasOk());
    }
    const auto kept = f.reports.findChildFiles (juce::File::findFiles, false, "*.zip");
    CHECK (kept.size() == problemReport::keptReports);
    CHECK (! f.reports.getChildFile ("spinus-report-2026-10-25_12-00-00.zip").exists());
    CHECK (f.reports.getChildFile ("spinus-report-2026-10-25_12-06-00.zip").exists());
}

TEST_CASE ("The issue page has the bug form with the version and system filled in", "[session][report]")
{
    const auto url = problemReport::getIssueUrl ("Spinus DAW 0.1.0-alpha.2", "Arch Linux, Flatpak").toString (true);
    CHECK (url.startsWith ("https://github.com/tuturu742/spinus-daw/issues/new?"));
    CHECK (url.contains ("template=bug.yml"));
    CHECK (url.contains ("version=Spinus%20DAW%200.1.0-alpha.2"));
    CHECK (url.contains ("system=Arch%20Linux%2C%20Flatpak"));

    // The form's fields are named so.
    const auto form =
        juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("../.github/ISSUE_TEMPLATE/bug.yml").loadFileAsString();
    CHECK (form.contains ("id: version"));
    CHECK (form.contains ("id: system"));
}
