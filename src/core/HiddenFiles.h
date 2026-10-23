#pragma once

#include <juce_core/juce_core.h>

/*  Which files and folders Spinus leaves out of its lists (samples, projects, demo songs), as a file
    manager does.

    juce::File::ignoreHiddenFiles follows each system's own rule: a name starting with a dot on Linux, the
    hidden attribute on Windows. A dot name (.git, .cache, a folder copied from Linux) is left out on Windows
    too: such files belong to programs, not to the person's music.
*/
namespace spinus::core
{

/** True for a file or folder hidden by the system, or whose name starts with a dot. */
inline bool isHidden (const juce::File& file) { return file.getFileName().startsWithChar ('.') || file.isHidden(); }

/** True if `file`, or a folder between `root` and it, is hidden (for files found by searching into
    subfolders). */
inline bool isHiddenBelow (const juce::File& file, const juce::File& root)
{
    for (auto f = file; f != root && f.isAChildOf (root); f = f.getParentDirectory())
        if (isHidden (f))
            return true;
    return false;
}

} // namespace spinus::core
