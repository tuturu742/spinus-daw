#include <catch2/catch_test_macros.hpp>

#include "AnalysisFakes.h"
#include "FakeProvider.h"
#include "engine/EngineHost.h"
#include "session/ProjectDocument.h"
#include "ui/Analyzer.h"

#include <cstdlib>
#include <iostream>

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;
using namespace spinus::test::analysis;

// The Analyzer with the real analysis engine and models, on songs of your own: each becomes a project with
// its tempo and key, its stems separated and added as the Analyzer's window would by default (drums and bass
// replicated, the rest as audio). Run by hand:
//   SPINUS_SONGS=~/Music/songs SPINUS_SONGS_OUT=~/Music/songs/projects build/release/tests/spinus_tests "[.songs]"
// With SPINUS_SONGS_SOUNDS=presets (or tunedSurge) every replicated stem uses Surge XT presets and the closest
// drum samples, and SPINUS_DRUMS names the drum samples folder. SPINUS_SONGS_STEMS=6 or 53 (many instruments)
// separates differently.
TEST_CASE ("Analyzer on your own songs, into projects", "[.songs]")
{
    const auto* songsVar = std::getenv ("SPINUS_SONGS");
    const auto* outVar = std::getenv ("SPINUS_SONGS_OUT");
    if (songsVar == nullptr || outVar == nullptr)
        SKIP ("set SPINUS_SONGS and SPINUS_SONGS_OUT");
    const juce::File songs (songsVar), out (outVar);
    out.createDirectory();

    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    ProjectDocument document { session, t.root.getChildFile ("settings") };
    ai::AiService service (t.root);
    service.setEnabled (true);
    constexpr int tenMinutes = 10 * 60 * 1000;

    auto files =
        songs.findChildFiles (juce::File::findFiles, false, "*.wav;*.flac;*.mp3;*.ogg;*.aif;*.aiff;*.opus;*.m4a");
    files.sort();
    for (const auto& song : files)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        std::cout << "== " << song.getFileName() << "\n";
        document.newProject();
        const auto folder = out.getChildFile (song.getFileNameWithoutExtension() + "");
        if (folder.exists())
        {
            std::cout << "   already there, skipped" << "\n";
            continue;
        }
        REQUIRE (document.saveAs (folder).wasOk());

        Analyzer analyzer (session, &service);
        analyzer.setSize (660, 560);
        REQUIRE (analyzer.analyze (song));
        REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, tenMinutes));
        if (analyzer.getDownloadSize() > 0 && ! analyzer.getRhythm())
        {
            std::cout << "   downloading " << analyzer.getDownloadSize() / 1000000 << " MB of models" << "\n";
            analyzer.downloadModels();
            REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, tenMinutes));
            REQUIRE (analyzer.analyze (song));
            REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, tenMinutes));
        }
        INFO (analyzer.getStatusText() << " | " << analyzer.getNeedsText());
        REQUIRE (analyzer.getRhythm());
        std::cout << "   tempo: " << analyzer.getTempoText() << "\n   key: " << analyzer.getKeyText() << "\n";
        analyzer.useTempo();
        if (analyzer.getKey())
            analyzer.useKey();

        const auto* count = std::getenv ("SPINUS_SONGS_STEMS"); // 4 (default), 6 or 53 (many instruments)
        const int stemCount = count != nullptr ? juce::String (count).getIntValue() : 4;
        REQUIRE (analyzer.separate (stemCount));
        REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, 3 * tenMinutes));
        INFO (analyzer.getStatusText());
        if (stemCount == Analyzer::manyInstruments)
        {
            REQUIRE (! analyzer.getStems().empty());
            std::cout << "   instruments:";
            for (const auto& [name, file] : analyzer.getStems())
                std::cout << " " << name;
            std::cout << "\n";
        }
        else
            REQUIRE ((int)analyzer.getStems().size() == stemCount);

        const auto uses = analyzer.getChosenUses();
        auto sounds = analyzer.getChosenSounds();
        if (const auto* chosen = std::getenv ("SPINUS_SONGS_SOUNDS"))
        {
            const auto which =
                juce::String (chosen) == "tunedSurge" ? Analyzer::Sounds::tunedSurge : Analyzer::Sounds::presets;
            for (const auto& [stem, use] : uses)
                sounds[stem] = which;
            if (const auto* drums = std::getenv ("SPINUS_DRUMS"))
                analyzer.setDrumSamplesFolder (juce::File (drums));
        }
        REQUIRE (analyzer.addStems (uses, sounds));
        REQUIRE (waitFor ([&] { return ! analyzer.isBusy(); }, tenMinutes));
        INFO (analyzer.getStatusText());

        for (const auto& [stem, notes] : analyzer.getTranscribed())
            std::cout << "   " << stem << ": " << notes.size() << " notes" << "\n";
        const auto& p = session.getProject();
        std::cout << "   " << p.channels.size() << " channels, " << p.patterns.size() << " patterns, "
                  << p.playlist.clips.size() << " Playlist clips; status: " << analyzer.getStatusText() << "\n";
        REQUIRE (document.save().wasOk());
        std::cout << "   saved in " << (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0 << " s"
                  << "\n";
    }
    service.setEnabled (false);
}
