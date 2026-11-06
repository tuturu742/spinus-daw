#pragma once

#include "model/Model.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace spinus
{

/** One of an instrument's presets, for closest preset: how it is loaded, and how the AI
    service's index of measured presets knows it. */
struct InstrumentPreset
{
    enum class Kind
    {
        file,       // a file the plugin loads itself through CLAP preset-load (Surge XT's .fxp patches)
        state,      // a saved state Spinus gives the plugin (OB-Xf's patches and Odin 2's built-in presets: these
                    // plugins do not load presets for a host)
        program,    // one of the plugin's programs, as the host sees them (VST2, VST3, LV2 presets)
        discovered, // a preset the plugin lists through CLAP preset discovery
        stateFile   // a file in a preset folder the user chose (PresetFolders), given to the plugin as its state
    };

    juce::String name, category;
    Kind kind = Kind::file;
    juce::File file;         // file and stateFile; for state, where it came from (if from a file)
    juce::MemoryBlock state; // state
    int program = -1;        // program
    plugins::ClapPluginFormat::DiscoveredPreset discovered;
    /** The index's name for it (unique within its instrument) and its version: a preset whose version
        changes is measured again. */
    juce::String id, version;
};

/** Finds and loads instruments' presets, whatever the plugin: Surge XT's patch files, OB-Xf's patches and
    Odin 2's built-in presets (given as the plugin's state, since neither loads presets for a host), presets
    a CLAP plugin lists through preset discovery, and other plugins' programs (more than one with a real
    name). */
class InstrumentPresets
{
public:
    /** At most this many presets are measured per instrument (the first ones), except Surge XT's own (about
        3,000). */
    static constexpr int most = 1000;

    /** The name of the AI service's index for this instrument's presets: "surge-xt", "ob-xf" and "odin2"
        (downloaded or installed as CLAP), else "plugin-" and a hash of the plugin's format, file and name. */
    static juce::String libraryOf (const model::Device&);

    /** Every preset of the instrument in `instance` (made for `device`), in a stable order; empty when it
        has none Spinus can choose from. Then the files in the preset folders the user chose for it (PresetFolders,
        beside the sound index's `indexFolder`; the default index's when it is empty). Message thread (it may read
        many small files). */
    static std::vector<InstrumentPreset> find (juce::AudioPluginInstance&, const model::Device&,
                                               const juce::File& indexFolder = {});

    /** Puts the preset in place (message thread). A file preset may finish loading on the plugin's own
        thread (Surge XT): HiddenInstrument::loadPreset waits for it. */
    static juce::Result load (juce::AudioPluginInstance&, const InstrumentPreset&);

    //==============================================================================
    // The pieces (tests).

    /** The plugin's programs with real names: none when it has fewer than two (an empty name, "Default",
        "Init" or "Program 3" is no preset; a name given twice counts once). */
    static std::vector<InstrumentPreset> findPrograms (juce::AudioPluginInstance&, const juce::String& version = {});
    static bool isPlaceholderName (const juce::String&);

    /** OB-Xf's patches: its factory patches (the download's Patches folder beside the plugin, or OB-Xf's own
        shared folder) and the user's (Documents/OB-Xf/Patches). */
    static std::vector<juce::File> getObxfFolders (const juce::File& pluginFile);
    static std::vector<InstrumentPreset> findObxfPatches (const std::vector<juce::File>& folders);
    /** The state OB-Xf saves for a patch, from its .fxp file (a VST "FPCh" chunk holding the patch's
        settings); empty if it is not an OB-Xf patch. */
    static juce::MemoryBlock obxfStateFromPatch (const juce::MemoryBlock& fxp);

    /** Odin 2's factory presets, read from its plugin file (they are built into it); empty if there are
        none. */
    static std::vector<InstrumentPreset> findOdinPresets (const juce::File& pluginFile);
    /** The preset files in folders the user chose: every file of the kind most of them are (a folder's notes and
        pictures are left out), in the folders inside too, by name; the folders inside are their categories.
        Placeholder names ("Default") are no presets. */
    static std::vector<InstrumentPreset> findStateFiles (const juce::Array<juce::File>& folders);
    /** A preset file as the plugin's state, the way the plugin keeps it (empty when the plugin can't take one):
        - inside the host's wrapper for VST3 plugins (the plugin's own part of it replaced, the rest as it is now);
        - an XML file as JUCE's binary XML when the plugin's own state is that (plugins made with JUCE);
        - otherwise the file as it is.
        VST2 plugins keep theirs in a bank format Spinus does not write: none. */
    static juce::MemoryBlock stateFromFile (juce::AudioPluginInstance&, const juce::MemoryBlock& file);
};

} // namespace spinus
