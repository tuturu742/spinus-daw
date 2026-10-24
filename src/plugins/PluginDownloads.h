#pragma once

#include <juce_core/juce_core.h>

namespace spinus::plugins
{

/** A set of open-source plugins Spinus offers to download: one project's release, e.g.
    Surge XT. Spinus does not ship these plugins; it downloads the project's own release archives
    from the project's own site, checks them against pinned SHA-256 hashes, and unpacks the files
    Spinus uses into the pack's folder. */
struct DownloadPack
{
    struct Extract
    {
        juce::String from; // path inside the archive; ending in "/" takes the folder's contents
        juce::String to;   // path inside the pack's folder ("" for its top)
    };

    struct Archive
    {
        juce::String url;
        juce::String sha256;
        juce::int64 size = 0; // bytes, for progress and for telling the user
        std::vector<Extract> extract;
    };

    juce::String id;          // the folder name, e.g. "surge-xt"
    juce::String name;        // e.g. "Surge XT"
    juce::String version;     // the pinned release
    juce::String description; // one line for the download window
    juce::String license;
    juce::String homepage;
    std::vector<Archive> archives;
    juce::StringArray files; // what Spinus loads from the pack, e.g. "Surge XT.clap" or "TIME-12.vst3" (a folder)

    juce::int64 getDownloadSize() const;
};

/** Downloads and installs packs into a folder (normally ~/.local/share/Spinus DAW/plugins), one
    sub-folder per pack, and finds the bundled plugins' files.

    install() blocks (run it on a background thread); the rest is cheap and thread-safe. */
class PluginDownloads
{
public:
    explicit PluginDownloads (juce::File folder);

    /** The systems Spinus is built for (no macOS build yet). */
    enum class System
    {
        linuxX64,
        windowsX64
    };

    static System getThisSystem();

    /** The packs offered on `system`, the ones behind model::getBundledDevices(): only projects that publish
        an open-source build of their own for that system, as an archive Spinus can unpack. A
        pack missing here (LSP Plugins and Odin 2 on Windows) is not offered there, and its devices are
        hidden. */
    static const std::vector<DownloadPack>& getPacksFor (System);

    /** The packs for this system. */
    static const std::vector<DownloadPack>& getStandardPacks();
    static const DownloadPack* findStandardPack (const juce::String& id);

    juce::File getFolder() const { return folder; }
    juce::File getPackFolder (const DownloadPack& pack) const { return folder.getChildFile (pack.id); }

    /** Installed at the pinned version, with all its files. */
    bool isInstalled (const DownloadPack&) const;

    struct Progress
    {
        juce::int64 done = 0, total = 0; // bytes downloaded, of all the pack's archives
        juce::String stage;              // "Downloading", "Checking", "Unpacking"
    };

    /** Downloads, checks and unpacks the pack, replacing an older version. Nothing is left behind if it
        fails or is cancelled. */
    juce::Result install (const DownloadPack&, const std::function<void (const Progress&)>& onProgress = {},
                          const std::atomic<bool>* cancel = nullptr) const;

    /** Deletes the pack's folder (Options > Get free plugins > Remove). Its plugin files go first: if one of
        them cannot be deleted (in use, or no permission), nothing else is, the pack stays installed, and the
        result says which. If only other files are left behind, the pack counts as removed and the result names
        them. Removing a pack that is not there succeeds. */
    juce::Result remove (const DownloadPack&) const;

    /** The folders the bundled plugins are looked for in, best first: each pack's folder, then the
        given extra folders (e.g. a development build's plugins/), then the system's CLAP and VST3 folders,
        so plugins installed with the system's package manager also work. */
    std::vector<juce::File> getSearchFolders (const std::vector<juce::File>& extra = {}) const;

    /** The first folder holding `file`, or an invalid File. A VST3 plugin is a folder (a bundle), so a
        folder of that name counts too. */
    static juce::File findFile (const std::vector<juce::File>& folders, const juce::String& file);

    /** The system's CLAP folders (~/.clap, /usr/lib/clap, /usr/local/lib/clap). */
    static std::vector<juce::File> getSystemClapFolders();

    /** The system's VST3 folders (~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3; on Windows Common Files\VST3). */
    static std::vector<juce::File> getSystemVst3Folders();

    /** Hashes a file (lowercase hex). */
    static juce::String sha256Of (const juce::File&);

private:
    juce::Result installUnlogged (const DownloadPack&, const std::function<void (const Progress&)>& onProgress,
                                  const std::atomic<bool>* cancel) const;
    juce::Result removeUnlogged (const DownloadPack&) const;

    juce::File folder;
};

} // namespace spinus::plugins
