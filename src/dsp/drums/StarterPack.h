#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace spinus::dsp::starter
{

/*  The Spinus Starter Pack: free sample files from Signature Sounds (free for any use; CC0 1.0 where marked) that come
   with every package, read-only, in one folder:

        Arch, AppImage, Flatpak   <prefix>/share/spinus/samples/Spinus Starter Pack/
        Windows                   <install folder>\samples\Spinus Starter Pack\       (beside spinus.exe)
        development build         <source tree>/samples/Spinus Starter Pack/

    The Browser lists it first under Samples; the Drum Machine's factory kits are made of its one-shots
    (kits.json in the folder). Nothing is ever written there: a project that uses one of its sounds keeps the
    sound itself (the Drum Machine and the Sampler keep their samples in the project; an audio clip's file is
    copied into the project's audio/ folder), so the project opens on any computer. tools/starter-pack makes
    the folder from the original sample packs. */

inline constexpr const char* packName = "Spinus Starter Pack";
inline constexpr const char* kitsFileName = "kits.json";

/** The pack's folder, found from the folder a program is in (Spinus's, or a plugin's): <folder>/samples/<pack>,
    else share/spinus/samples/<pack> up to six folders above it (lib/spinus, or a plugin's bundle in
    lib/vst3), else, in a development build of Spinus, the source tree's. Only a folder with kits.json counts.
    A nonexistent file if none is there. */
juce::File findPack (const juce::File& programFolder);

/** The same from the running program (Spinus, or the plugin's library in another program), and the folder
    its link points to (/usr/bin/spinus is a link to /usr/lib/spinus/spinus). */
juce::File findPack();

/** One pad of a factory kit. `file` is relative to the pack's folder ("Drums/Kicks/Kick 808 Deep.flac"). */
struct KitPad
{
    juce::String name, file;
    int chokeGroup = 0;
    float tuneSemitones = 0.0f, volumeDb = 0.0f; // the pad's Tune and Volume
};

struct Kit
{
    juce::String name, description;
    std::vector<KitPad> pads; // pad 1 first; up to 16
};

/** The factory kits in the pack's kits.json, in its order; none if it is missing or unreadable. */
std::vector<Kit> readKits (const juce::File& pack);

/** A kit pad's file in the pack ("/" in `file` works on Windows too). */
juce::File padFile (const juce::File& pack, const KitPad&);

/** What `spinus --self-test` finds of an installed pack: its sounds (FLAC files) and kits, and what is wrong
    (a sound that does not read, a kit without 16 pads or with a pad whose sound is not there). */
struct PackCheck
{
    int sounds = 0, kits = 0;
    juce::StringArray problems;
};
PackCheck checkPack (const juce::File& pack);

} // namespace spinus::dsp::starter
