#pragma once

#include <juce_core/juce_core.h>

namespace spinus::session
{

/** Help > Report a problem: a zip of Spinus's logs of its last runs, with the version and system, for the user
    to attach to a new GitHub issue whose form Spinus opens with the version and system filled in. Spinus sends
    nothing itself: the user sees the file and the page, and decides. */
namespace problemReport
{
/** How many runs' logs go in a report: this one and the two before (a crash is usually the one before). */
constexpr int runs = 3;
/** Older reports are deleted when a new one is made. */
constexpr int keptReports = 5;
constexpr const char* issueFormUrl = "https://github.com/tuturu742/spinus-daw/issues/new";

/** Where reports go: reports/ beside the log folder. */
juce::File getFolder();

/** Writes `zip` with the logs (already masked as they were written) and about.txt with `aboutLines` (the version
    and system: AboutSpinus::getVersionInfo()). */
juce::Result write (const juce::File& zip, const juce::Array<juce::File>& logs, const juce::StringArray& aboutLines);

/** Makes a report in `folder` (spinus-report-<time>.zip) from the newest runs' logs in `logFolder`, then deletes
    the reports older than the newest keptReports. The zip, or a failed result. */
juce::Result make (const juce::File& folder, const juce::File& logFolder, const juce::StringArray& aboutLines,
                   juce::File& zip, juce::Time now = juce::Time::getCurrentTime());

/** The new issue's address, with the bug form chosen and the version and system filled in. */
juce::URL getIssueUrl (const juce::String& version, const juce::String& system);
} // namespace problemReport

} // namespace spinus::session
