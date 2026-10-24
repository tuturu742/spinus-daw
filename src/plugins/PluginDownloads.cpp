#include "PluginDownloads.h"

#include "ArchiveReader.h"
#include "core/Log.h"
#include "session/ProgramFolder.h"

#include <juce_cryptography/juce_cryptography.h>

#if ! JUCE_WINDOWS
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <sys/stat.h>
#endif

namespace spinus::plugins
{

namespace
{
constexpr const char* markerName = ".spinus-pack"; // holds the installed version

/** Where an archive entry goes, or an invalid File if the pack does not use it. */
juce::File destinationFor (const juce::String& entry, const DownloadPack::Archive& archive, const juce::File& root)
{
    // Archives name their entries "./x" or "x".
    const auto path = entry.startsWith ("./") ? entry.substring (2) : entry;
    for (auto& e : archive.extract)
    {
        if (e.from.endsWith ("/"))
        {
            if (path.startsWith (e.from) && path.length() > e.from.length())
                return root.getChildFile (e.to).getChildFile (path.substring (e.from.length()));
        }
        else if (path == e.from)
        {
            return root.getChildFile (e.to.isNotEmpty() ? e.to : e.from.fromLastOccurrenceOf ("/", false, false));
        }
    }
    return {};
}

/** Only files inside the pack's folder. */
bool isInside (const juce::File& target, const juce::File& root)
{
    return target != juce::File() && (target == root || target.isAChildOf (root));
}

#if JUCE_WINDOWS
/** Downloads `url` to `target` with JUCE (WinINet, which follows GitHub's redirects). */
juce::Result download (const juce::String& url, const juce::File& target,
                       const std::function<void (juce::int64)>& onBytes, const std::atomic<bool>* cancel)
{
    target.deleteFile();
    juce::FileOutputStream out (target);
    if (! out.openedOk())
        return juce::Result::fail ("Could not write " + target.getFullPathName() + ".");

    int status = 0;
    auto in = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                     .withConnectionTimeoutMs (30000)
                                                     .withNumRedirectsToFollow (10)
                                                     .withExtraHeaders ("User-Agent: Spinus DAW")
                                                     .withStatusCode (&status));
    if (in == nullptr || status >= 400)
        return juce::Result::fail ("Could not download " + url +
                                   (status >= 400 ? " (the server answered " + juce::String (status) + ")." : "."));

    juce::HeapBlock<char> buffer (65536);
    juce::int64 written = 0;
    while (! in->isExhausted())
    {
        if (cancel != nullptr && cancel->load())
            return juce::Result::fail ("Cancelled.");
        const auto n = in->read (buffer.get(), 65536);
        if (n < 0)
            return juce::Result::fail ("Could not download " + url + ": the connection was lost.");
        if (n == 0)
            break;
        if (! out.write (buffer.get(), (size_t)n))
            return juce::Result::fail ("Could not write " + target.getFullPathName() + ".");
        written += n;
        if (onBytes)
            onBytes (written);
    }
    out.flush();
    return juce::Result::ok();
}

juce::Result unpack (const juce::File& file, const DownloadPack::Archive& archive, const juce::File& root,
                     const std::atomic<bool>* cancel)
{
    // With JUCE alone (ArchiveReader): only files inside the pack's folder.
    const auto destination = [&archive, &root] (const juce::String& entry)
    {
        const auto target = destinationFor (entry, archive, root);
        return isInside (target, root) ? target : juce::File();
    };
    if (archive.url.endsWithIgnoreCase (".zip"))
        return archives::unpackZip (file, destination, cancel);
    if (archive.url.endsWithIgnoreCase (".tar.gz"))
        return archives::unpackTarGz (file, destination, cancel);
    return juce::Result::fail ("Spinus cannot unpack " + archive.url.fromLastOccurrenceOf ("/", false, false) + ".");
}
#else
/** Downloads `url` to `target` with libcurl (follows redirects: GitHub release links do). */
juce::Result download (const juce::String& url, const juce::File& target,
                       const std::function<void (juce::int64)>& onBytes, const std::atomic<bool>* cancel)
{
    static const bool initialised = curl_global_init (CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (! initialised)
        return juce::Result::fail ("Could not start the downloader.");

    std::unique_ptr<CURL, decltype (&curl_easy_cleanup)> curl (curl_easy_init(), curl_easy_cleanup);
    if (curl == nullptr)
        return juce::Result::fail ("Could not start the downloader.");

    target.deleteFile();
    juce::FileOutputStream out (target);
    if (! out.openedOk())
        return juce::Result::fail ("Could not write " + target.getFullPathName() + ".");

    struct Context
    {
        juce::FileOutputStream& out;
        const std::function<void (juce::int64)>& onBytes;
        const std::atomic<bool>* cancel;
        juce::int64 written = 0;
    } context { out, onBytes, cancel };

    auto write = +[] (char* data, size_t size, size_t count, void* user) -> size_t
    {
        auto& c = *static_cast<Context*> (user);
        if (! c.out.write (data, size * count))
            return 0;
        c.written += (juce::int64)(size * count);
        if (c.onBytes)
            c.onBytes (c.written);
        return size * count;
    };
    auto progress = +[] (void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int
    {
        auto& c = *static_cast<Context*> (user);
        return c.cancel != nullptr && c.cancel->load() ? 1 : 0;
    };

    char error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt (curl.get(), CURLOPT_URL, url.toRawUTF8());
    curl_easy_setopt (curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt (curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt (curl.get(), CURLOPT_CONNECTTIMEOUT, 30L);
    // Give up on a stalled connection (under 1 kB/s for a minute) rather than hang.
    curl_easy_setopt (curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt (curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt (curl.get(), CURLOPT_USERAGENT, "Spinus DAW");
    curl_easy_setopt (curl.get(), CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt (curl.get(), CURLOPT_WRITEDATA, &context);
    curl_easy_setopt (curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt (curl.get(), CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt (curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt (curl.get(), CURLOPT_ERRORBUFFER, error);

    const auto code = curl_easy_perform (curl.get());
    out.flush();
    if (code == CURLE_ABORTED_BY_CALLBACK)
        return juce::Result::fail ("Cancelled.");
    if (code != CURLE_OK)
        return juce::Result::fail ("Could not download " + url + ": " +
                                   (error[0] != 0 ? juce::String (error) : juce::String (curl_easy_strerror (code))));
    return juce::Result::ok();
}

juce::Result unpack (const juce::File& file, const DownloadPack::Archive& archive, const juce::File& root,
                     const std::atomic<bool>* cancel)
{
    std::unique_ptr<struct archive, decltype (&archive_read_free)> reader (archive_read_new(), archive_read_free);
    archive_read_support_filter_all (reader.get());
    archive_read_support_format_all (reader.get());
    if (archive_read_open_filename (reader.get(), file.getFullPathName().toRawUTF8(), 65536) != ARCHIVE_OK)
        return juce::Result::fail ("Could not open " + file.getFileName() + ": " + archive_error_string (reader.get()));

    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header (reader.get(), &entry)) == ARCHIVE_OK)
    {
        if (cancel != nullptr && cancel->load())
            return juce::Result::fail ("Cancelled.");

        const juce::String name (juce::CharPointer_UTF8 (archive_entry_pathname_utf8 (entry) != nullptr
                                                             ? archive_entry_pathname_utf8 (entry)
                                                             : archive_entry_pathname (entry)));
        const auto target = destinationFor (name, archive, root);
        // Only regular files and folders, and only inside the pack's folder.
        if (! isInside (target, root))
            continue;

        const auto type = archive_entry_filetype (entry);
        if (type == AE_IFDIR)
        {
            target.createDirectory();
            continue;
        }
        if (type != AE_IFREG)
            continue;

        target.getParentDirectory().createDirectory();
        juce::FileOutputStream out (target);
        if (! out.openedOk())
            return juce::Result::fail ("Could not write " + target.getFullPathName() + ".");
        out.setPosition (0);
        out.truncate();

        const void* block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        int read = ARCHIVE_OK;
        while ((read = archive_read_data_block (reader.get(), &block, &size, &offset)) == ARCHIVE_OK)
        {
            out.setPosition (offset);
            if (! out.write (block, size))
                return juce::Result::fail ("Could not write " + target.getFullPathName() + ".");
        }
        if (read != ARCHIVE_EOF)
            return juce::Result::fail ("Could not unpack " + file.getFileName() + ": " +
                                       archive_error_string (reader.get()));
        out.flush();

        // Plugins are shared libraries: keep them executable.
        if ((archive_entry_perm (entry) & (mode_t)S_IXUSR) != 0)
            target.setExecutePermission (true);
    }

    if (status != ARCHIVE_EOF)
        return juce::Result::fail ("Could not unpack " + file.getFileName() + ": " +
                                   archive_error_string (reader.get()));
    return juce::Result::ok();
}
#endif
} // namespace

juce::int64 DownloadPack::getDownloadSize() const
{
    juce::int64 total = 0;
    for (auto& a : archives)
        total += a.size;
    return total;
}

//==============================================================================
PluginDownloads::PluginDownloads (juce::File f) : folder (std::move (f)) {}

namespace
{
// Pinned releases; the hashes are of the archives on each project's GitHub release page (checked by
// downloading them). Only projects whose source and published build are both open source, with the build
// as a plain archive (no installer), are offered. Each pack's text is the same on every system;
// only its archives differ.
DownloadPack surgeXt (DownloadPack::Archive plugins)
{
    return { "surge-xt",
             "Surge XT",
             "1.3.4",
             "Synthesizer with over 2,000 sounds and wavetables, and its effects",
             "GPL-3.0",
             "https://surge-synthesizer.github.io",
             {
                 std::move (plugins),
                 // The sounds: Surge finds SurgeXTData next to its plugin file.
                 { "https://github.com/surge-synthesizer/releases-xt/releases/download/1.3.4/"
                   "surge-xt-portable-content-1.3.4.tar.gz",
                   "7ed24822eab69f73101d8558222979b1bceb4882a780292bb7087309b7b101bd",
                   252763750,
                   { { "Surge Synth Team/SurgeXTData/", "SurgeXTData" } } },
             },
             { "Surge XT.clap", "Surge XT Effects.clap" } };
}

DownloadPack dragonflyReverb (juce::String url, juce::String sha256, juce::int64 size)
{
    return { "dragonfly-reverb",
             "Dragonfly Reverb",
             "3.2.10",
             "Hall, room and plate reverbs, and early reflections",
             "GPL-3.0",
             "https://michaelwillis.github.io/dragonfly-reverb",
             { { std::move (url),
                 std::move (sha256),
                 size,
                 { { "dragonfly-reverb-3.2.10/DragonflyHallReverb.clap", "" },
                   { "dragonfly-reverb-3.2.10/DragonflyRoomReverb.clap", "" },
                   { "dragonfly-reverb-3.2.10/DragonflyPlateReverb.clap", "" },
                   { "dragonfly-reverb-3.2.10/DragonflyEarlyReflections.clap", "" } } } },
             { "DragonflyHallReverb.clap", "DragonflyRoomReverb.clap", "DragonflyPlateReverb.clap",
               "DragonflyEarlyReflections.clap" } };
}

// The Surge Synth Team's OB-Xf (the open-source OB-Xd's successor). Its factory patches (GPL-3.0, like the
// rest of its repository) are a separate download, the same for every system. OB-Xf's own browser looks for
// them only in its shared folder, outside the pack's; Spinus finds them in the pack's Patches folder for
// closest preset.
DownloadPack obXf (juce::String url, juce::String sha256, juce::int64 size)
{
    return { "ob-xf",
             "OB-Xf",
             "1.0.3",
             "Analog-style polyphonic synthesizer in the spirit of the Oberheim OB-X, with its factory patches",
             "GPL-3.0",
             "https://github.com/surge-synthesizer/OB-Xf",
             { { std::move (url),
                 std::move (sha256),
                 size,
                 { { "obxf_products/OB-Xf.clap", "" }, { "obxf_products/LICENSE", "" } } },
               { "https://github.com/surge-synthesizer/OB-Xf/releases/download/v1.0.3/ob-xf-assets-v1.0.3.zip",
                 "6d324e520694167f22dc97912088bafea52bb6e348916056482a5f79434c6ec0",
                 23966440,
                 { { "Surge Synth Team/OB-Xf/Patches/", "Patches" } } } },
             { "OB-Xf.clap" } };
}

DownloadPack peakEater (juce::String url, juce::String sha256, juce::int64 size)
{
    return { "peakeater",
             "PeakEater",
             "0.8.2",
             "Clipper and waveshaper (hard, soft, cubic and more) with oversampling",
             "GPL-3.0",
             "https://github.com/vvvar/PeakEater",
             { { std::move (url),
                 std::move (sha256),
                 size,
                 { { "peakeater_artefacts/Release/CLAP/peakeater.clap", "" } } } },
             { "peakeater.clap" } };
}

// Six Sines (Paul Walker, of the Surge Synth Team): its own code is MIT, its builds GPL-3.0 (they include JUCE
// under its GPL-3.0 option), and its factory patches are inside the plugin.
DownloadPack sixSines (juce::String url, juce::String sha256, juce::int64 size, const juce::String& folder)
{
    std::vector<DownloadPack::Extract> extract { { folder + "/Six Sines.clap", "" } };
    if (folder == "six-sines_assets") // only the Linux zip has the licence beside the plugins
        extract.push_back ({ folder + "/LICENSE_GPL3", "" });
    return { "six-sines",
             "Six Sines",
             "1.2.1",
             "FM synthesizer: six sine operators that modulate each other, with factory patches",
             "GPL-3.0",
             "https://github.com/baconpaul/six-sines",
             { { std::move (url), std::move (sha256), size, std::move (extract) } },
             { "Six Sines.clap" } };
}

// tiagolr's TIME-12 and GATE-12 publish VST3 and LV2 builds, no CLAP: Spinus loads their VST3 (a folder).
DownloadPack time12 (juce::String url, juce::String sha256, juce::int64 size, const juce::String& folder)
{
    return { "time12",
             "TIME-12",
             "1.2.3",
             "Delay-time shaper drawn as an envelope, in time with the song: stutters, tape stops, scratches",
             "GPL-3.0",
             "https://github.com/tiagolr/time12",
             { { std::move (url), std::move (sha256), size, { { folder + "/VST3/TIME-12.vst3/", "TIME-12.vst3" } } } },
             { "TIME-12.vst3" } };
}

DownloadPack gate12 (juce::String url, juce::String sha256, juce::int64 size, const juce::String& folder)
{
    return { "gate12",
             "GATE-12",
             "1.3.3",
             "Trance gate and volume shaper drawn as an envelope, in time with the song",
             "GPL-3.0",
             "https://github.com/tiagolr/gate12",
             { { std::move (url), std::move (sha256), size, { { folder + "/VST3/GATE-12.vst3/", "GATE-12.vst3" } } } },
             { "GATE-12.vst3" } };
}

const std::vector<DownloadPack>& linuxPacks()
{
    static const std::vector<DownloadPack> packs {
        surgeXt ({ "https://github.com/surge-synthesizer/releases-xt/releases/download/1.3.4/"
                   "surge-xt-linux-1.3.4-pluginsonly.tar.gz",
                   "dd431b75f5fa197c4bffa6ca27ca46970f0a94c834119bb1db7decdeec4c28db",
                   97294804,
                   { { "Surge XT.clap", "" }, { "Surge XT Effects.clap", "" } } }),
        { "lsp-plugins",
          "LSP Plugins",
          "1.2.35",
          "Equalizer, compressor, limiter, gate, multiband compressor and spectrum analyzer",
          "LGPL-3.0",
          "https://lsp-plug.in",
          { { "https://github.com/lsp-plugins/lsp-plugins/releases/download/1.2.35/"
              "lsp-plugins-1.2.35-Linux-x86_64.7z",
              "4ef3b2f63b29a522fdaf3a18f879c1c85c82f0bdd1e0b3c849a5f3c4b08d2550",
              38793460,
              { { "lsp-plugins-1.2.35-Linux-x86_64/CLAP/", "" },
                { "lsp-plugins-1.2.35-Linux-x86_64/COPYING.LESSER.txt", "" },
                { "lsp-plugins-1.2.35-Linux-x86_64/COPYING.txt", "" } } } },
          { "lsp-plugins.clap" } },
        dragonflyReverb ("https://github.com/michaelwillis/dragonfly-reverb/releases/download/3.2.10/"
                         "dragonfly-reverb-3.2.10-linux-x86_64.tar.xz",
                         "59f1593120de82e31dd194e2ab1536af14fec2b3ec6ec45abf6d9c8ac02ef430", 2267368),
        // Odin 2 publishes its Linux build under the "NightlyDevel" release; its Windows build is an installer.
        { "odin2",
          "Odin 2",
          "2.4.1",
          "Synthesizer with analog, wavetable, FM and chiptune oscillators, and factory presets",
          "GPL-3.0",
          "https://thewavewarden.com/odin2",
          { { "https://github.com/TheWaveWarden/odin2/releases/download/NightlyDevel/Odin2.4.1Linux.zip",
              "462c4fffda99ff91a6c362d8cf41616b50807778ae7a96471753046a799aed63",
              66588004,
              { { "Odin2.clap", "" } } } },
          { "Odin2.clap" } },
        obXf ("https://github.com/surge-synthesizer/OB-Xf/releases/download/v1.0.3/ob-xf-Linux-v1.0.3.zip",
              "72b60c83cf6426337031df744c34a047104a9d95f1feaf6cd048ecfa39f74c96", 21867343),
        peakEater ("https://github.com/vvvar/PeakEater/releases/download/v0.8.2/peakeater-v0.8.2-Linux-x86_64.zip",
                   "0ddf7d839fa436e62c2e9d5b1f188d695424d5aba2f242ad0a89bb8634f299b9", 4586800),
        sixSines ("https://github.com/baconpaul/six-sines/releases/download/v1.2.1/"
                  "six-sines-Linux-2026-07-20-18ecb36.zip",
                  "9b5353eebfec3e462c80ed8c0f53c965e6262bb38537518159cce71de7eace41", 28862299, "six-sines_assets"),
        time12 ("https://github.com/tiagolr/time12/releases/download/v1.2.3/time12-linux-v1.2.3.zip",
                "d1e19e0c2e5332626af3077d4dd98dcaeb4b4c2b33ee74c267a732c29df10613", 5066023, "time12-linux"),
        gate12 ("https://github.com/tiagolr/gate12/releases/download/v1.3.3/gate12-linux-v1.3.3.zip",
                "fa19ebf1cacd064c5c2fc687894ffc7400a92e9887f6db3cc3f2f59042d07cad", 5135883, "gate12-linux"),
    };
    return packs;
}

// Windows: the projects' Windows zips (Spinus unpacks .zip and .tar.gz there). LSP Plugins publishes no
// Windows build, and Odin 2 only an installer, so they are not offered.
const std::vector<DownloadPack>& windowsPacks()
{
    static const std::vector<DownloadPack> packs {
        surgeXt ({ "https://github.com/surge-synthesizer/releases-xt/releases/download/1.3.4/"
                   "surge-xt-win64-1.3.4-pluginsonly.zip",
                   "564e162c560af07ad4ed47fe1bfcd827cf97a575de30d06c48249aad2e7c35e6",
                   49093875,
                   { { "Surge XT.clap", "" }, { "Surge XT Effects.clap", "" } } }),
        dragonflyReverb ("https://github.com/michaelwillis/dragonfly-reverb/releases/download/3.2.10/"
                         "dragonfly-reverb-3.2.10-win64.zip",
                         "ca5f35f9dcd9a33f06490d3c92f04061a1a798cb81257d5ca5851bdda0824c66", 22447365),
        obXf ("https://github.com/surge-synthesizer/OB-Xf/releases/download/v1.0.3/ob-xf-Windows-v1.0.3.zip",
              "149fda0649daf8a3aa0851801f92515f0dbf6203acf33c0edc7d3c445e80b1e7", 9222959),
        peakEater ("https://github.com/vvvar/PeakEater/releases/download/v0.8.2/peakeater-v0.8.2-Windows-x86_64.zip",
                   "6bae14feb7ea19beed6faf4a6984168521d2deb1351db9da5c208d274254e075", 3727973),
        // Six Sines' Windows zip holds the same files as its installer (.exe, not offered).
        sixSines ("https://github.com/baconpaul/six-sines/releases/download/v1.2.1/"
                  "six-sines-Windows-2026-07-20-18ecb36.zip",
                  "9f7c7fa5cb1c5e11b53c0a9306ee443149cc7bc0a109380dbbe1578f3a7b6e5d", 22398434, "installer_copy"),
        time12 ("https://github.com/tiagolr/time12/releases/download/v1.2.3/time12-win-v1.2.3.zip",
                "b5699e49850826a8279e315d51cc8cf92636fc98d5d649bcfadd7ae026982989", 5539406, "time12-win"),
        gate12 ("https://github.com/tiagolr/gate12/releases/download/v1.3.3/gate12-win-v1.3.3.zip",
                "93b1056a40a341452804260a165ce9ce0feaad65eedbcf192319910671f54cf5", 6383770, "gate12-win"),
    };
    return packs;
}
} // namespace

PluginDownloads::System PluginDownloads::getThisSystem()
{
#if JUCE_WINDOWS
    return System::windowsX64;
#else
    return System::linuxX64;
#endif
}

const std::vector<DownloadPack>& PluginDownloads::getPacksFor (System system)
{
    return system == System::windowsX64 ? windowsPacks() : linuxPacks();
}

const std::vector<DownloadPack>& PluginDownloads::getStandardPacks() { return getPacksFor (getThisSystem()); }

const DownloadPack* PluginDownloads::findStandardPack (const juce::String& id)
{
    for (auto& p : getStandardPacks())
        if (p.id == id)
            return &p;
    return nullptr;
}

bool PluginDownloads::isInstalled (const DownloadPack& pack) const
{
    const auto dir = getPackFolder (pack);
    if (dir.getChildFile (markerName).loadFileAsString().trim() != pack.version)
        return false;
    for (auto& f : pack.files)
        if (! dir.getChildFile (f).exists()) // a VST3 plugin is a folder
            return false;
    return true;
}

juce::Result PluginDownloads::install (const DownloadPack& pack,
                                       const std::function<void (const Progress&)>& onProgress,
                                       const std::atomic<bool>* cancel) const
{
    core::Logs::info ("download", "Downloading " + pack.name + " " + pack.version + " (" +
                                      juce::File::descriptionOfSizeInBytes (pack.getDownloadSize()) + ")");
    const auto started = juce::Time::getMillisecondCounter();
    auto result = installUnlogged (pack, onProgress, cancel);
    const auto took = juce::String ((juce::Time::getMillisecondCounter() - started) / 1000.0, 1) + " s";
    if (result.wasOk())
        core::Logs::info ("download", "Installed " + pack.name + " " + pack.version + " in " + took + " (" +
                                          getPackFolder (pack).getFullPathName() + ")");
    else if (cancel != nullptr && cancel->load())
        core::Logs::info ("download", pack.name + ": cancelled after " + took);
    else
        core::Logs::warning ("download", pack.name + " was not installed: " + result.getErrorMessage());
    return result;
}

juce::Result PluginDownloads::installUnlogged (const DownloadPack& pack,
                                               const std::function<void (const Progress&)>& onProgress,
                                               const std::atomic<bool>* cancel) const
{
    if (! folder.createDirectory())
        return juce::Result::fail ("Could not create " + folder.getFullPathName() + ".");

    // Everything happens in a hidden staging folder, which replaces the pack's folder at the end.
    const auto staging = folder.getChildFile ("." + pack.id + ".partial");
    staging.deleteRecursively();
    const juce::ScopeGuard cleanUp { [&staging] { staging.deleteRecursively(); } };
    if (! staging.createDirectory())
        return juce::Result::fail ("Could not create " + staging.getFullPathName() + ".");

    Progress progress { 0, pack.getDownloadSize(), "Downloading" };
    auto report = [&]
    {
        if (onProgress)
            onProgress (progress);
    };

    for (size_t i = 0; i < pack.archives.size(); ++i)
    {
        const auto& archive = pack.archives[i];
        const auto file = staging.getChildFile (".download-" + juce::String ((int)i));
        const auto before = progress.done;

        progress.stage = "Downloading";
        report();
        auto result = download (
            archive.url, file,
            [&] (juce::int64 bytes)
            {
                progress.done = before + bytes;
                report();
            },
            cancel);
        if (result.failed())
            return result;

        progress.stage = "Checking";
        report();
        if (sha256Of (file) != archive.sha256.toLowerCase())
            return juce::Result::fail ("The download of " + pack.name +
                                       " is not the expected file (its checksum differs), so it was not installed.");

        progress.stage = "Unpacking";
        report();
        result = unpack (file, archive, staging, cancel);
        file.deleteFile();
        if (result.failed())
            return result;
        progress.done = before + archive.size;
    }

    for (auto& f : pack.files)
        if (! staging.getChildFile (f).exists())
            return juce::Result::fail (pack.name + "'s download does not contain " + f + ".");

    if (! staging.getChildFile (markerName).replaceWithText (pack.version))
        return juce::Result::fail ("Could not write to " + staging.getFullPathName() + ".");

    const auto target = getPackFolder (pack);
    if (target.exists() && ! target.deleteRecursively())
        return juce::Result::fail ("Could not replace " + target.getFullPathName() + ".");
    if (! staging.moveFileTo (target))
        return juce::Result::fail ("Could not create " + target.getFullPathName() + ".");
    return juce::Result::ok();
}

juce::Result PluginDownloads::remove (const DownloadPack& pack) const
{
    auto result = removeUnlogged (pack);
    if (result.wasOk())
        core::Logs::info ("download", "Removed " + pack.name);
    else
        core::Logs::warning ("download", result.getErrorMessage());
    return result;
}

juce::Result PluginDownloads::removeUnlogged (const DownloadPack& pack) const
{
    const auto dir = getPackFolder (pack);
    if (! dir.exists())
        return juce::Result::ok();

    // The plugin files first: if one cannot go (Windows keeps a loaded plugin's file; or no permission), stop
    // there, so the pack is still complete and still shown as downloaded.
    juce::StringArray kept;
    for (auto& f : pack.files)
        if (const auto file = dir.getChildFile (f); file.exists() && ! file.deleteRecursively())
            kept.add (f);
    if (! kept.isEmpty())
        return juce::Result::fail ("Could not remove " + pack.name + ": " + kept.joinIntoString (", ") +
                                   (kept.size() == 1 ? " is" : " are") +
                                   " in use or not yours to delete. Close any program using it and try again.");

    // Then the rest (sounds, licenses, the version marker): the plugins are gone, so the pack is too, even if some
    // of these stay.
    if (dir.deleteRecursively())
        return juce::Result::ok();

    juce::StringArray left;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no))
        left.add (f.getRelativePathFrom (dir));
    left.sort (true);
    juce::String names =
        left.isEmpty() ? juce::String ("its folder") : left.joinIntoString (", ", 0, juce::jmin (3, left.size()));
    if (left.size() > 3)
        names << " and " << (left.size() - 3) << " more";
    return juce::Result::fail ("Removed " + pack.name + ", but could not delete " + names + " in " +
                               dir.getFullPathName() + ".");
}

//==============================================================================
std::vector<juce::File> PluginDownloads::getSearchFolders (const std::vector<juce::File>& extra) const
{
    std::vector<juce::File> folders;
    for (auto& pack : getStandardPacks())
        folders.push_back (getPackFolder (pack));
    folders.insert (folders.end(), extra.begin(), extra.end());
    for (auto& f : getSystemClapFolders())
        folders.push_back (f);
    for (auto& f : getSystemVst3Folders())
        folders.push_back (f);
    return folders;
}

juce::File PluginDownloads::findFile (const std::vector<juce::File>& folders, const juce::String& file)
{
    for (auto& f : folders)
        if (auto candidate = f.getChildFile (file); candidate.existsAsFile() || candidate.isDirectory())
            return candidate;
    return {};
}

std::vector<juce::File> PluginDownloads::getSystemClapFolders()
{
#if JUCE_WINDOWS
    // The CLAP standard's folders on Windows.
    return { juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Programs/Common/CLAP"),
             juce::File (
                 juce::SystemStats::getEnvironmentVariable ("COMMONPROGRAMFILES", "C:\\Program Files\\Common Files"))
                 .getChildFile ("CLAP") };
#else
    std::vector<juce::File> folders { juce::File ("~/.clap"), juce::File ("/usr/lib/clap"),
                                      juce::File ("/usr/local/lib/clap") };
    for (const auto& host : getHostPluginFolders ("CLAP")) // the computer's own, seen from the Flatpak
        folders.push_back (host);
    return folders;
#endif
}

std::vector<juce::File> PluginDownloads::getSystemVst3Folders()
{
#if JUCE_WINDOWS
    // The VST3 standard's folders on Windows.
    return { juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Programs/Common/VST3"),
             juce::File (
                 juce::SystemStats::getEnvironmentVariable ("COMMONPROGRAMFILES", "C:\\Program Files\\Common Files"))
                 .getChildFile ("VST3") };
#else
    std::vector<juce::File> folders { juce::File ("~/.vst3"), juce::File ("/usr/lib/vst3"),
                                      juce::File ("/usr/local/lib/vst3") };
    for (const auto& host : getHostPluginFolders ("VST3")) // the computer's own, seen from the Flatpak
        folders.push_back (host);
    return folders;
#endif
}

juce::String PluginDownloads::sha256Of (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return {};
    return juce::SHA256 (in).toHexString();
}

} // namespace spinus::plugins
