#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "plugins/ArchiveReader.h"

#include <archive.h>
#include <archive_entry.h>

namespace archives = spinus::plugins::archives;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-archive-tests-" + juce::Uuid().toString());
};

// Deep enough for tar's long-name cases: over 100 characters (ustar's prefix field) and over 255 (GNU and pax).
juce::String longFolder()
{
    return "Surge Synth Team/SurgeXTData/wavetables_3rdparty/A.Liv/Clean Sounding Wavetables With A Long Folder Name";
}

juce::String veryLong()
{
    return longFolder() + "/" + juce::String::repeatedString ("x", 120) + "/" + juce::String::repeatedString ("y", 90) +
           ".wav";
}

/** A .tar.gz as libarchive (what the projects' releases are made with) writes it, in `format`. */
void writeTarGz (const juce::File& file, int format)
{
    std::unique_ptr<struct archive, decltype (&archive_write_free)> writer (archive_write_new(), archive_write_free);
    archive_write_add_filter_gzip (writer.get());
    archive_write_set_format (writer.get(), format);
    REQUIRE (archive_write_open_filename (writer.get(), file.getFullPathName().toRawUTF8()) == ARCHIVE_OK);

    const auto add = [&writer] (const juce::String& path, int type, const juce::String& content)
    {
        std::unique_ptr<archive_entry, decltype (&archive_entry_free)> entry (archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname_utf8 (entry.get(), path.toRawUTF8());
        archive_entry_set_filetype (entry.get(), (unsigned)type);
        archive_entry_set_perm (entry.get(), 0644);
        if (type == AE_IFLNK)
            archive_entry_set_symlink (entry.get(), "/etc/passwd");
        archive_entry_set_size (entry.get(), type == AE_IFREG ? (la_int64_t)content.getNumBytesAsUTF8() : 0);
        REQUIRE (archive_write_header (writer.get(), entry.get()) == ARCHIVE_OK);
        if (type == AE_IFREG)
            archive_write_data (writer.get(), content.toRawUTF8(), content.getNumBytesAsUTF8());
    };
    add ("plugin/", AE_IFDIR, {});
    add ("plugin/Surge XT.clap", AE_IFREG, "the plugin");
    add ("plugin/empty.txt", AE_IFREG, {});
    add ("plugin/link", AE_IFLNK, {});
    add (longFolder() + "/Short.wav", AE_IFREG, "a wavetable");
    add (veryLong(), AE_IFREG, juce::String::repeatedString ("0123456789", 300)); // several blocks
    add ("left out.txt", AE_IFREG, "not wanted");
    REQUIRE (archive_write_close (writer.get()) == ARCHIVE_OK);
}

/** Puts every entry under `root`, except "left out.txt". */
archives::Destination into (const juce::File& root)
{
    return [root] (const juce::String& entry)
    { return entry == "left out.txt" ? juce::File() : root.getChildFile (entry); };
}
} // namespace

TEST_CASE ("Archives unpack with JUCE alone: tar.gz in ustar GNU and pax form", "[plugins]")
{
    // Every form the projects' archives come in. ustar keeps names up to 255 characters (prefix + name).
    const auto [format, longestName] = GENERATE (
        std::pair { ARCHIVE_FORMAT_TAR_USTAR, false }, std::pair { ARCHIVE_FORMAT_TAR_GNUTAR, true },
        std::pair { ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, true }, std::pair { ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE, true });
    INFO ("format " << format);
    TempFolder t;
    const auto file = t.root.getChildFile ("pack.tar.gz");
    if (longestName)
        writeTarGz (file, format);
    else
    {
        // ustar cannot hold the longest name: libarchive refuses it, so that entry is left out of this form.
        std::unique_ptr<struct archive, decltype (&archive_write_free)> writer (archive_write_new(),
                                                                                archive_write_free);
        archive_write_add_filter_gzip (writer.get());
        archive_write_set_format (writer.get(), format);
        REQUIRE (archive_write_open_filename (writer.get(), file.getFullPathName().toRawUTF8()) == ARCHIVE_OK);
        for (auto& [path, content] :
             std::vector<std::pair<juce::String, juce::String>> { { "plugin/Surge XT.clap", "the plugin" },
                                                                  { "plugin/empty.txt", {} },
                                                                  { longFolder() + "/Short.wav", "a wavetable" },
                                                                  { "left out.txt", "not wanted" } })
        {
            std::unique_ptr<archive_entry, decltype (&archive_entry_free)> entry (archive_entry_new(),
                                                                                  archive_entry_free);
            archive_entry_set_pathname_utf8 (entry.get(), path.toRawUTF8());
            archive_entry_set_filetype (entry.get(), AE_IFREG);
            archive_entry_set_perm (entry.get(), 0644);
            archive_entry_set_size (entry.get(), (la_int64_t)content.getNumBytesAsUTF8());
            REQUIRE (archive_write_header (writer.get(), entry.get()) == ARCHIVE_OK);
            archive_write_data (writer.get(), content.toRawUTF8(), content.getNumBytesAsUTF8());
        }
        REQUIRE (archive_write_close (writer.get()) == ARCHIVE_OK);
    }

    const auto out = t.root.getChildFile ("out");
    const auto result = archives::unpackTarGz (file, into (out));
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    CHECK (out.getChildFile ("plugin/Surge XT.clap").loadFileAsString() == "the plugin");
    CHECK (out.getChildFile ("plugin/empty.txt").existsAsFile());
    CHECK (out.getChildFile (longFolder() + "/Short.wav").loadFileAsString() == "a wavetable");
    CHECK_FALSE (out.getChildFile ("left out.txt").exists());
    CHECK_FALSE (out.getChildFile ("plugin/link").exists()); // links are not followed or made
    if (longestName)
        CHECK (out.getChildFile (veryLong()).loadFileAsString() == juce::String::repeatedString ("0123456789", 300));
}

TEST_CASE ("Archives unpack with JUCE alone: zip files", "[plugins]")
{
    TempFolder t;
    const auto source = t.root.getChildFile ("source");
    REQUIRE (source.createDirectory());
    source.getChildFile ("Surge XT.clap").replaceWithText ("the plugin");
    source.getChildFile ("left out.txt").replaceWithText ("not wanted");
    source.getChildFile ("deep").createDirectory();
    source.getChildFile ("deep/Effects.clap").replaceWithText ("effects");

    juce::ZipFile::Builder builder;
    for (auto* path : { "Surge XT.clap", "left out.txt", "deep/Effects.clap" })
        builder.addFile (source.getChildFile (path), 9, path);
    const auto zip = t.root.getChildFile ("pack.zip");
    {
        juce::FileOutputStream stream (zip);
        REQUIRE (builder.writeToStream (stream, nullptr));
    }

    const auto out = t.root.getChildFile ("out");
    REQUIRE (archives::unpackZip (zip, into (out)).wasOk());
    CHECK (out.getChildFile ("Surge XT.clap").loadFileAsString() == "the plugin");
    CHECK (out.getChildFile ("deep/Effects.clap").loadFileAsString() == "effects");
    CHECK_FALSE (out.getChildFile ("left out.txt").exists());
}

TEST_CASE ("A broken or cut-off archive is reported, not half unpacked silently", "[plugins]")
{
    TempFolder t;
    const auto file = t.root.getChildFile ("pack.tar.gz");
    writeTarGz (file, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED);

    // Cut off inside the last file's data.
    juce::MemoryBlock whole;
    REQUIRE (file.loadFileAsData (whole));
    juce::MemoryOutputStream ungzipped;
    {
        juce::MemoryInputStream raw (whole, false);
        juce::GZIPDecompressorInputStream in (&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
        ungzipped.writeFromInputStream (in, -1);
    }
    // Inside the long file's data (cutting from the end would only cut tar's padding).
    const auto* bytes = static_cast<const char*> (ungzipped.getData());
    const std::string_view tar (bytes, ungzipped.getDataSize());
    const auto found = tar.find ("0123456789");
    REQUIRE (found != std::string_view::npos);
    const auto inside = found + 100;
    const auto cut = t.root.getChildFile ("cut.tar.gz");
    {
        juce::FileOutputStream out (cut);
        juce::GZIPCompressorOutputStream gzip (out, 6, juce::GZIPCompressorOutputStream::windowBitsGZIP);
        gzip.write (ungzipped.getData(), (size_t)inside);
    }
    CHECK (archives::unpackTarGz (cut, into (t.root.getChildFile ("out"))).failed());

    t.root.getChildFile ("not an archive.zip").replaceWithText ("hello");
    CHECK (archives::unpackZip (t.root.getChildFile ("not an archive.zip"), into (t.root)).failed());
}
