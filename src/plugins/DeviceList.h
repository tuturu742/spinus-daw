#pragma once

#include "PluginCatalog.h"
#include "model/Model.h"

namespace spinus::plugins
{

/** Everything Spinus can put in a channel or an effect slot (for the Browser), by where it comes
    from: Spinus's own devices (Sampler, Drum Machine, 3x Osc), the open-source plugins Spinus offers to
    download (Surge XT, LSP, Dragonfly), and the installed plugins the catalog found, minus
    the ones a bundled device already uses. The Browser and the "add" menus of the Channel Rack and the Mixer use it.

    Message thread only. Sends a change message after refresh().
*/
class DeviceList : public juce::ChangeBroadcaster
{
public:
    enum class Origin
    {
        spinus,   // made by Spinus
        bundled,  // open-source plugins Spinus offers to download, made by others
        installed // plugins installed on this computer
    };

    struct Entry
    {
        juce::String name;
        juce::String maker;  // "Spinus", the bundled project (e.g. "Dragonfly Reverb"), or the plugin's maker
        juce::String badge;  // "VST3", "CLAP", "LV2", "VST2", "WIN" (a Windows plugin through yabridge)
        juce::String detail; // shown in the hint bar: maker, format, file
        bool instrument = false;
        Origin origin = Origin::installed;
        bool available = true; // bundled: false until its plugin is downloaded
        juce::String pack;     // bundled: the download that provides it (PluginDownloads), e.g. "surge-xt"
        model::Device device;  // without an ID: ready for channel.add / insert.addEffect

        /** Installed plugins: every format it is installed in, the listed one among them ("VST3",
            "VST2"...), and the key its preferred format is remembered by. */
        juce::StringArray formats;
        juce::String format, key;
    };

    DeviceList(); // bundled devices only

    /** Adds the catalog's plugins (call refresh() after a scan). nullptr: bundled devices only. */
    void setCatalog (PluginCatalog*);
    PluginCatalog* getCatalog() const noexcept { return catalog; }

    /** Where the bundled devices' plugin files are looked for, as given to registerBundledDevices. Not
        set: every bundled device counts as available. */
    void setBundledFolders (std::vector<juce::File>);
    const std::optional<std::vector<juce::File>>& getBundledFolders() const noexcept { return bundledFolders; }

    void refresh();

    /** Where the formats the user prefers are kept (a plugin often installs VST3 and VST2, and one of
        them may work better). Not set: not remembered. */
    void setPreferencesFile (const juce::File&);
    /** Lists this plugin in another of its formats from now on (remembered); false if it has no such format.
        Channels and effects already added keep theirs. */
    bool preferFormat (const juce::String& key, const juce::String& format);

    /** Spinus's and bundled devices first (in their own order), then installed plugins sorted by name. */
    const std::vector<Entry>& getEntries() const noexcept { return entries; }

    static juce::String getBadge (const juce::PluginDescription&);
    static model::Device makeDevice (const juce::PluginDescription&);

    /** For drag and drop: an entry as a var, and back (false if the var is not an entry). */
    static juce::var toVar (const Entry&);
    static bool fromVar (const juce::var&, Entry& result);

private:
    PluginCatalog* catalog = nullptr;
    std::optional<std::vector<juce::File>> bundledFolders;
    std::vector<Entry> entries;
    juce::File preferencesFile;
    std::map<juce::String, juce::String> preferredFormats; // by Entry::key
};

} // namespace spinus::plugins
