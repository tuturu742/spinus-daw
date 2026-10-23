#pragma once

#include <juce_core/juce_core.h>

namespace spinus::demo
{

/*  The demo songs (the Browser's Projects, "<Name> (demo)"; docs/user/getting-started.md): songs made in
    Spinus, shipped with every package as read-only project folders in one demo folder (demo/ in the source
    tree, CC BY 4.0; tools/showcase-song makes Siskin). Opening one never touches that folder: the
    song is copied to the user's Music folder first, and the copy is what opens.

        Arch, AppImage, Flatpak   <prefix>/share/spinus/demo/<Name>/   (Spinus is <prefix>/lib/spinus/spinus)
        Windows                   <install folder>\demo\<Name>\        (beside spinus.exe)
        development build         <source tree>/demo/<Name>/
*/

/** The first demo song, listed before the others. */
inline constexpr const char* demoSongName = "Siskin";

/** The project folders in a demo folder, by name with Siskin first. Other files (README.md) and folders
    that are not projects are left out. */
juce::Array<juce::File> listDemos (const juce::File& demoFolder);

/** The shipped demo folder, found from the folder Spinus's program is in: <program>/demo, else share/spinus/demo
    up to three folders above it (lib/spinus, lib64/spinus, lib/<triplet>/spinus), else, in a development build,
    the source tree's demo/. Only a folder holding at least one demo song counts. A nonexistent file if none
    is there. */
juce::File findDemoFolder (const juce::File& programFolder);

/** The same, from the running Spinus's own folder (getProgramFolder()). */
juce::File findDemoFolder();

/** Every shipped demo song (listDemos of findDemoFolder). */
juce::Array<juce::File> findBundledDemos();

/** The first shipped demo song (Siskin when it is there); a nonexistent file if there is none. */
juce::File findBundledDemo (const juce::File& programFolder);
juce::File findBundledDemo();

/** Where the user's copy goes: the Music folder (~/Music; on Windows the user's Music folder). */
juce::File getDemoCopyParent();

struct DemoCopy
{
    juce::Result result = juce::Result::ok();
    juce::File projectFile;    // the copy's .spn file, to open
    bool alreadyThere = false; // an earlier copy was found and kept (with the user's own changes)
};

/** Makes the user's copy of a demo song in `parent`: `parent`/<its name>, e.g. Music/Siskin. A copy
    made earlier (a project folder by that name) is kept and returned as it is, so changes made to it are not
    lost; a folder by that name that is not a project is left alone and the copy is "Siskin 2" (3, ...).
    The copy is made in a hidden folder beside it and renamed into place, so a failed copy never looks like
    a project. Only `bundled` is read: nothing is written there. */
DemoCopy copyDemo (const juce::File& bundled, const juce::File& parent);

} // namespace spinus::demo
