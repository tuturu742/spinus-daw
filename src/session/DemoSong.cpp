#include "DemoSong.h"

#include "ProgramFolder.h"
#include "core/HiddenFiles.h"
#include "project/ProjectFiles.h"

namespace spinus::demo
{

juce::Array<juce::File> listDemos (const juce::File& demoFolder)
{
    juce::Array<juce::File> demos;
    if (! demoFolder.isDirectory())
        return demos;
    for (const auto& entry : juce::RangedDirectoryIterator (
             demoFolder, false, "*", juce::File::findDirectories | juce::File::ignoreHiddenFiles))
        if (! core::isHidden (entry.getFile()) && project::isProjectFolder (entry.getFile()))
            demos.add (entry.getFile());
    struct SiskinFirst
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            const bool aFirst = a.getFileName() == demoSongName, bFirst = b.getFileName() == demoSongName;
            if (aFirst != bFirst)
                return aFirst ? -1 : 1;
            return a.getFileName().compareNatural (b.getFileName());
        }
    } order;
    demos.sort (order);
    return demos;
}

juce::File findDemoFolder (const juce::File& programFolder)
{
    const auto hasDemos = [] (const juce::File& f) { return ! listDemos (f).isEmpty(); };

    if (const auto beside = programFolder.getChildFile ("demo"); hasDemos (beside))
        return beside;

    auto folder = programFolder;
    for (int up = 0; up < 3 && folder.getParentDirectory() != folder; ++up)
    {
        folder = folder.getParentDirectory();
        if (const auto shared = folder.getChildFile ("share/spinus/demo"); hasDemos (shared))
            return shared;
    }

#if defined(SPINUS_SOURCE_DIR)
    if (const auto source = juce::File (SPINUS_SOURCE_DIR).getChildFile ("demo"); hasDemos (source))
        return source;
#endif
    return {};
}

juce::File findDemoFolder() { return findDemoFolder (getProgramFolder()); }

juce::Array<juce::File> findBundledDemos() { return listDemos (findDemoFolder()); }

juce::File findBundledDemo (const juce::File& programFolder)
{
    const auto demos = listDemos (findDemoFolder (programFolder));
    return demos.isEmpty() ? juce::File() : demos.getFirst();
}

juce::File findBundledDemo() { return findBundledDemo (getProgramFolder()); }

juce::File getDemoCopyParent() { return juce::File::getSpecialLocation (juce::File::userMusicDirectory); }

namespace
{
/** Copies every file and folder under `from` into `to` (made first), each writable by the user: an
    installed copy's files can be read-only. */
juce::Result copyTree (const juce::File& from, const juce::File& to)
{
    if (const auto r = to.createDirectory(); r.failed())
        return r;
    for (const auto& entry : juce::RangedDirectoryIterator (from, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto& file = entry.getFile();
        const auto target = to.getChildFile (file.getFileName());
        if (entry.isDirectory())
        {
            if (const auto r = copyTree (file, target); r.failed())
                return r;
        }
        else if (! file.copyFileTo (target))
            return juce::Result::fail ("Could not copy " + file.getFileName() + " to " + to.getFullPathName());
        else
            target.setReadOnly (false);
    }
    return juce::Result::ok();
}
} // namespace

DemoCopy copyDemo (const juce::File& bundled, const juce::File& parent)
{
    DemoCopy copy;
    if (! project::isProjectFolder (bundled))
    {
        copy.result = juce::Result::fail ("The demo song is not installed with this Spinus.");
        return copy;
    }

    // The user's copy from before, kept as it is; or the first free name.
    const auto name = bundled.getFileName();
    auto target = parent.getChildFile (name);
    for (int n = 2; target.exists(); ++n)
    {
        if (project::isProjectFolder (target))
        {
            copy.projectFile = project::findProjectFile (target);
            copy.alreadyThere = true;
            return copy;
        }
        target = parent.getChildFile (name + " " + juce::String (n));
    }

    if (const auto r = parent.createDirectory(); r.failed())
    {
        copy.result =
            juce::Result::fail ("Could not make the folder " + parent.getFullPathName() + ": " + r.getErrorMessage());
        return copy;
    }
    const auto partial = parent.getChildFile ("." + target.getFileName() + ".copying");
    partial.deleteRecursively();
    if (const auto r = copyTree (bundled, partial); r.failed())
    {
        partial.deleteRecursively();
        copy.result = r;
        return copy;
    }
    if (! partial.moveFileTo (target))
    {
        partial.deleteRecursively();
        copy.result = juce::Result::fail ("Could not make " + target.getFullPathName());
        return copy;
    }

    // The project file is named after its folder ("Siskin 2.spn" in "Siskin 2").
    auto file = project::findProjectFile (target);
    if (const auto named = target.getChildFile (target.getFileName() + project::projectFileExtension);
        file != named && file.moveFileTo (named))
        file = named;
    copy.projectFile = std::move (file);
    return copy;
}

} // namespace spinus::demo
