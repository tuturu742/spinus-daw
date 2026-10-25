#include "ProblemReport.h"

#include "ProgramFolder.h"
#include "core/Log.h"

namespace spinus::session::problemReport
{

juce::File getFolder() { return getLogFolder().getSiblingFile ("reports"); }

juce::Result write (const juce::File& zip, const juce::Array<juce::File>& logs, const juce::StringArray& aboutLines)
{
    juce::ZipFile::Builder builder;
    for (const auto& log : logs)
        if (log.existsAsFile())
            builder.addFile (log, 9, "logs/" + log.getFileName());

    const auto about = aboutLines.joinIntoString ("\n") + "\n";
    builder.addEntry (std::make_unique<juce::MemoryInputStream> (about.toRawUTF8(), about.getNumBytesAsUTF8(), true), 9,
                      "about.txt", juce::Time::getCurrentTime());

    if (! zip.getParentDirectory().createDirectory())
        return juce::Result::fail ("Could not make the folder " + zip.getParentDirectory().getFullPathName());
    zip.deleteFile();
    juce::FileOutputStream out (zip);
    if (! out.openedOk() || ! builder.writeToStream (out, nullptr))
        return juce::Result::fail ("Could not write " + zip.getFullPathName());
    return juce::Result::ok();
}

juce::Result make (const juce::File& folder, const juce::File& logFolder, const juce::StringArray& aboutLines,
                   juce::File& zip, juce::Time now)
{
    zip = folder.getChildFile ("spinus-report-" + now.formatted ("%Y-%m-%d_%H-%M-%S") + ".zip");
    if (auto result = write (zip, core::Logs::recentFiles (runs, logFolder), aboutLines); result.failed())
        return result;

    auto reports = folder.findChildFiles (juce::File::findFiles, false, "spinus-report-*.zip");
    // The names hold the time: newest last when sorted by name.
    std::sort (reports.begin(), reports.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
    for (int i = 0; i < reports.size() - keptReports; ++i)
        reports.getReference (i).deleteFile();
    return juce::Result::ok();
}

juce::URL getIssueUrl (const juce::String& version, const juce::String& system)
{
    // GitHub fills an issue form's fields from parameters named after their ids (.github/ISSUE_TEMPLATE/bug.yml).
    return juce::URL (issueFormUrl)
        .withParameter ("template", "bug.yml")
        .withParameter ("version", version)
        .withParameter ("system", system);
}

} // namespace spinus::session::problemReport
