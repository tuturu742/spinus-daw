#include <catch2/catch_test_macros.hpp>

#include "session/AnalysisLibrary.h"
#include "ui/AnalysisLibraryView.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace spinus;

/* The analysis library: what Spinus learnt about songs, kept in the settings folder for the Analyzer and
   the Track creator, with stems as FLAC, its size, and deleting. */

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-library-tests-" + juce::Uuid().toString());
};

/** A 3-second stereo 32-bit float WAV (as the AI service writes stems): a tone. */
juce::File makeStem (const juce::File& file, double frequency)
{
    const double rate = 44100.0;
    juce::AudioBuffer<float> audio (2, (int)(3.0 * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c)
            audio.setSample (c, i, 0.3f * (float)std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate));
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions()
                    .withSampleRate (rate)
                    .withNumChannels (2)
                    .withBitsPerSample (32)
                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

const juce::String digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const juce::String other = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
const auto never = [] { return false; };
} // namespace

TEST_CASE ("Analysis library: entries by the song's contents, of this version only", "[session][library]")
{
    TempFolder t;
    auto library = AnalysisLibrary::inAiFolder (t.root.getChildFile ("ai"));
    CHECK (library.getFolder() == t.root.getChildFile ("ai/library"));
    CHECK (! AnalysisLibrary::inAiFolder ({}).isAvailable());
    CHECK (! library.read (digest).isObject());

    REQUIRE (library.set (digest, "Song.flac", "rhythm", juce::JSON::parse (R"({"tempo": 120.0})")));
    REQUIRE (library.setIn (digest, "Song.flac", "notes", "htdemucs/bass", juce::JSON::parse ("[1, 2]")));
    REQUIRE (library.setIn (digest, "Renamed.flac", "notes", "htdemucs/other", juce::JSON::parse ("[3]")));
    const auto entry = library.read (digest);
    CHECK (juce::approximatelyEqual ((double)entry["rhythm"]["tempo"], 120.0));
    CHECK (entry["notes"]["htdemucs/bass"].size() == 2);
    CHECK (entry["notes"]["htdemucs/other"].size() == 1);
    CHECK (entry["name"].toString() == "Renamed.flac"); // the last name it had
    CHECK ((int)entry["version"] == AnalysisLibrary::version);
    CHECK (library.getFolder().getChildFile (AnalysisLibrary::keyOf (digest) + "/song.json").existsAsFile());

    // An entry of another version is not used, but listed (to delete).
    const auto old = library.getFolder().getChildFile (AnalysisLibrary::keyOf (other));
    REQUIRE (old.createDirectory().wasOk());
    REQUIRE (old.getChildFile ("song.json").replaceWithText (R"({"version": 0, "name": "Old.wav"})"));
    CHECK (! library.read (other).isObject());
    const auto entries = library.list();
    REQUIRE (entries.size() == 2);
    const auto& stale = entries[0].key == AnalysisLibrary::keyOf (other) ? entries[0] : entries[1];
    CHECK (! stale.current);
    CHECK (ui::AnalysisLibraryView::describe (stale).contains ("another version"));
}

TEST_CASE ("Analysis library: stems kept as FLAC, given back as WAV, sizes, deleting", "[session][library]")
{
    TempFolder t;
    auto library = AnalysisLibrary::inAiFolder (t.root.getChildFile ("ai"));
    const auto song = t.root.getChildFile ("My Song.wav");
    REQUIRE (song.replaceWithText ("the user's own audio file"));
    const std::map<juce::String, juce::File> stems { { "bass", makeStem (t.root.getChildFile ("out/bass.wav"), 55.0) },
                                                     { "other",
                                                       makeStem (t.root.getChildFile ("out/other.wav"), 220.0) } };
    CHECK (library.findStems (digest, "htdemucs").empty());
    REQUIRE (library.keepStems (digest, "htdemucs", stems, never).wasOk());
    REQUIRE (library.set (digest, song.getFileName(), "key", juce::JSON::parse (R"({"name": "A minor"})")));
    const auto kept = library.findStems (digest, "htdemucs");
    REQUIRE (kept.size() == 2);
    CHECK (kept.at ("bass").hasFileExtension ("flac"));
    // 24-bit FLAC: much smaller than the 32-bit float WAV.
    CHECK (kept.at ("bass").getSize() < stems.at ("bass").getSize() * 3 / 4);
    CHECK (library.stemModels (digest) == juce::StringArray { "htdemucs" });
    CHECK (library.findStems (digest, "htdemucs_6s").empty()); // another separation: not these

    // Given back as WAV files the AI service reads, with the same length.
    std::map<juce::String, juce::File> wavs;
    REQUIRE (library.restoreStems (digest, "htdemucs", t.root.getChildFile ("cache"), wavs, never).wasOk());
    REQUIRE (wavs.size() == 2);
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (wavs.at ("bass")), true));
        REQUIRE (reader != nullptr);
        CHECK (reader->lengthInSamples == (juce::int64)(3.0 * 44100.0));
    }

    // Sizes: the song's entry, its stems, the total.
    auto entries = library.list();
    REQUIRE (entries.size() == 1);
    CHECK (entries[0].name == "My Song.wav");
    CHECK (entries[0].stemBytes > 0);
    CHECK (entries[0].bytes > entries[0].stemBytes);
    CHECK (library.totalBytes() == entries[0].bytes);

    // The window shows it, and deletes the stems: what was measured stays.
    ui::AnalysisLibraryView view (library);
    REQUIRE (view.getEntries().size() == 1);
    CHECK (view.getTotalText().contains ("1 song"));
    CHECK (ui::AnalysisLibraryView::describe (view.getEntries()[0]).contains ("stems:"));
    REQUIRE (view.deleteStems (0));
    CHECK (library.findStems (digest, "htdemucs").empty());
    CHECK (library.read (digest)["key"]["name"].toString() == "A minor");
    CHECK (view.getEntries()[0].stemBytes == 0);

    // All stems, one song, everything; the user's own files are never touched.
    REQUIRE (library.keepStems (other, "htdemucs", stems, never).wasOk());
    REQUIRE (view.deleteAllStems());
    view.refresh();
    CHECK (library.stemModels (other).isEmpty());
    REQUIRE (view.deleteSong (0));
    CHECK (view.getEntries().size() == 1);
    REQUIRE (view.deleteEverything());
    CHECK (view.getEntries().empty());
    CHECK (view.getTotalText() == "No songs are kept.");
    CHECK (! library.getFolder().exists());
    CHECK (song.existsAsFile());
    for (const auto& [name, file] : stems)
        CHECK (file.existsAsFile());
    CHECK (! view.deleteSong (0));
}

TEST_CASE ("Analysis library: sizes in words", "[session][library]")
{
    using View = ui::AnalysisLibraryView;
    CHECK (View::sizeText (0) == "0 KB");
    CHECK (View::sizeText (1) == "1 KB");
    CHECK (View::sizeText (4500) == "5 KB");
    CHECK (View::sizeText (12'300'000) == "12.3 MB");
    CHECK (View::sizeText (2'500'000'000) == "2.5 GB");
}
