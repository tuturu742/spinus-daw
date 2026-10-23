#include <catch2/catch_test_macros.hpp>

#include "AnalysisFakes.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "session/ProjectDocument.h"
#include "ui/MainView.h"
#include "ui/PianoRoll.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>

using namespace spinus;
using namespace spinus::ui;
using namespace spinus::test::analysis;

// A song the Analyzer turned into many instruments (53 parts, each on an instrument of its own): every part on a channel of its own with hundreds to thousands of notes
// in a song-long pattern, the song's sections as patterns on the Playlist, and automation. Opening patterns,
// selecting channels and saving must stay quick.

namespace
{
using Clock = std::chrono::steady_clock;

double msSince (Clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (Clock::now() - start).count();
}

/** Commands collected into one undo step (as the Analyzer adds a song), with their results known at once. */
class Steps
{
public:
    explicit Steps (const model::Project& project) : trial (project) {}

    juce::var add (const juce::String& name, const juce::var& params)
    {
        auto& registry = commands::CommandRegistry::get();
        juce::String error;
        auto onCopy = registry.create (name, params, error);
        auto real = registry.create (name, params, error);
        INFO (name << ": " << error);
        REQUIRE (onCopy != nullptr);
        REQUIRE (real != nullptr);
        auto* command = onCopy.get();
        const auto tried = trial.perform (std::move (onCopy));
        INFO (tried.getErrorMessage());
        REQUIRE (tried.wasOk());
        transaction->add (std::move (real));
        return command->getResult();
    }

    juce::Result performOn (commands::CommandHistory& history) { return history.perform (std::move (transaction)); }

private:
    commands::CommandHistory trial;
    std::unique_ptr<commands::Transaction> transaction = std::make_unique<commands::Transaction> ("Notes from a song");
};

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [name, value] : properties)
        o->setProperty (name, value);
    return juce::var (o);
}

/** The instruments to spread the parts over: Spinus's own, Surge XT when built, and the CLAP plugins named in
    SPINUS_PERF_PLUGINS (colon-separated paths, e.g. Odin 2 and OB-Xf), as the Analyzer would choose them. */
std::vector<juce::var> instrumentsFor (tracktion::Engine& engine)
{
    std::vector<juce::var> result {
        object ({ { "kind", "stock" }, { "stockType", "spinus.3x-osc" } }),
        object ({ { "kind", "stock" }, { "stockType", "spinus.sampler" } }),
    };
    if (bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        result.push_back (object ({ { "kind", "stock" }, { "stockType", "bundled.surge-xt" } }));

    if (const auto* paths = std::getenv ("SPINUS_PERF_PLUGINS"))
    {
        auto& manager = engine.getPluginManager();
        for (auto& path : juce::StringArray::fromTokens (paths, ":", ""))
            for (auto* format : manager.pluginFormatManager.getFormats())
            {
                if (! format->fileMightContainThisPluginType (path))
                    continue;
                juce::OwnedArray<juce::PluginDescription> found;
                format->findAllTypesForFile (found, path);
                for (auto* d : found)
                    if (d->isInstrument)
                    {
                        manager.knownPluginList.addType (*d);
                        result.push_back (object ({ { "kind", "plugin" },
                                                    { "plugin", object ({ { "format", d->pluginFormatName },
                                                                          { "name", d->name },
                                                                          { "manufacturer", d->manufacturerName },
                                                                          { "fileOrIdentifier", d->fileOrIdentifier },
                                                                          { "uniqueId", d->uniqueId } }) } }));
                    }
            }
    }
    return result;
}

struct Song
{
    std::vector<model::PatternId> patterns; // the song-long one first, then the sections
    std::vector<model::ChannelId> channels;
    int notes = 0;
};

/** A four-minute song (120 bars at 120 BPM) in `parts` parts, as the Analyzer adds it, plus eight sections. */
Song addSong (Session& session, int parts, int notesPerPart)
{
    constexpr model::Ticks bar = 3840, beat = 960;
    constexpr int bars = 120;
    const auto instruments = instrumentsFor (session.getEdit().engine);

    Song song;
    Steps steps (session.getProject());
    std::mt19937 random (42);
    const auto whole = (int)steps.add (
        "pattern.add", object ({ { "name", "Analysis: Song" }, { "length", (juce::int64)(bar * bars) } }))["pattern"];
    song.patterns.push_back (model::PatternId { (uint32_t)whole });
    std::vector<int> sections;
    for (int s = 0; s < 8; ++s)
    {
        const auto id = (int)steps.add ("pattern.add", object ({ { "name", "Section " + juce::String (s + 1) },
                                                                 { "length", (juce::int64)(bar * 4) } }))["pattern"];
        sections.push_back (id);
        song.patterns.push_back (model::PatternId { (uint32_t)id });
    }

    for (int part = 0; part < parts; ++part)
    {
        const auto& device = instruments[(size_t)part % instruments.size()];
        const auto channel =
            (int)steps.add ("channel.add", object ({ { "name", "Part " + juce::String (part + 1) + " (Song)" },
                                                     { "instrument", device } }))["channel"];
        song.channels.push_back (model::ChannelId { (uint32_t)channel });

        // Transcribed notes: spread over the song, some chords, uneven lengths.
        const auto spacing = (bar * bars) / notesPerPart;
        juce::Array<juce::var> list;
        for (int i = 0; i < notesPerPart; ++i)
        {
            const auto start = (juce::int64)i * spacing + (juce::int64)(random() % 40);
            list.add (object ({ { "start", start },
                                { "length", (juce::int64)(60 + random() % (juce::uint32)beat) },
                                { "pitch", 36 + (int)(random() % 48) },
                                { "velocity", 0.3 + 0.7 * (double)(random() % 100) / 100.0 } }));
        }
        steps.add ("pattern.setNotes", object ({ { "pattern", whole }, { "channel", channel }, { "notes", list } }));
        song.notes += notesPerPart;

        // The sections: a bar-long phrase of the part, four times.
        for (auto section : sections)
        {
            juce::Array<juce::var> phrase;
            for (int i = 0; i < 32; ++i)
                phrase.add (object ({ { "start", (juce::int64)(i * bar / 8) },
                                      { "length", (juce::int64)(bar / 8) },
                                      { "pitch", 48 + (int)(random() % 24) },
                                      { "velocity", 0.8 } }));
            steps.add ("pattern.setNotes",
                       object ({ { "pattern", section }, { "channel", channel }, { "notes", phrase } }));
        }
    }

    // On the Playlist: the analysis at bar 1, then the sections one after another; and a volume curve for some
    // of the parts.
    const auto lane = steps.add ("lane.add", object ({ { "name", "Notes (Song)" } }))["lane"];
    steps.add ("clip.addPattern", object ({ { "lane", lane }, { "pattern", whole }, { "start", 0 } }));
    const auto sectionLane = steps.add ("lane.add", object ({ { "name", "Sections" } }))["lane"];
    for (int i = 0; i < 24; ++i)
        steps.add ("clip.addPattern", object ({ { "lane", sectionLane },
                                                { "pattern", sections[(size_t)i % sections.size()] },
                                                { "start", (juce::int64)(i * 4 * bar) } }));
    for (int part = 0; part < parts; part += 4)
    {
        juce::Array<juce::var> points;
        for (int i = 0; i <= 64; ++i)
            points.add (object ({ { "time", (juce::int64)(i * bar * bars / 64) }, { "value", (i % 2) * 0.5 + 0.25 } }));
        steps.add ("clip.addAutomation",
                   object ({ { "laneName", "Volume " + juce::String (part + 1) },
                             { "start", 0 },
                             { "length", (juce::int64)(bar * bars) },
                             { "target", object ({ { "kind", "channelVolume" },
                                                   { "channel", (int)song.channels[(size_t)part].value } }) },
                             { "points", points } }));
    }

    const auto result = steps.performOn (session.getHistory());
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    return song;
}

/** Runs what the message thread has queued (change messages, async updates), returning how long it took. */
double settle()
{
    const auto start = Clock::now();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    return msSince (start);
}

/** Opens a pattern as the Playlist's double-click and the Channel Rack's piano-roll button do: the pattern
    becomes current, a channel is selected, the Piano Roll shows it, and the window is drawn. */
double openPattern (MainView& view, Session& session, model::PatternId pattern, model::ChannelId channel)
{
    const auto start = Clock::now();
    session.setCurrentPattern (pattern);
    session.setSelectedChannel (channel);
    const auto engine = msSince (start);
    view.getWorkspace().showPanel ("pianoRoll");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    const auto updates = msSince (start);
    if (auto* roll = dynamic_cast<PianoRoll*> (view.getWorkspace().getPanel ("pianoRoll")))
    {
        roll->handleUpdateNowIfNeeded();
        roll->createComponentSnapshot (roll->getLocalBounds());
    }
    const auto roll = msSince (start);
    view.createComponentSnapshot (view.getLocalBounds());
    if (std::getenv ("SPINUS_PERF_VERBOSE") != nullptr)
        std::cout << "  engine " << engine << ", updates " << updates - engine << ", piano roll drawn "
                  << roll - updates << ", window drawn " << msSince (start) - roll << std::endl;
    return msSince (start);
}

struct Timings
{
    double add = 0, firstOpen = 0, worstOpen = 0, totalOpen = 0, worstEdit = 0, autosave = 0, save = 0;
    int opens = 0;
};

Timings measure (int parts, int notesPerPart, bool save)
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    ProjectDocument document { session, t.root.getChildFile ("settings") };
    MainView view { session, &document };
    view.setSize (1400, 900);
    settle();

    Timings timings;
    auto start = Clock::now();
    const auto song = addSong (session, parts, notesPerPart);
    settle();
    timings.add = msSince (start);
    if (std::getenv ("SPINUS_PERF_VERBOSE") != nullptr)
        std::cout << "added: " << timings.add << " ms" << std::endl;
    REQUIRE ((int)session.getProject().channels.size() == parts);

    // Every pattern, each with a few of its channels, and back to the song-long one.
    for (size_t i = 0; i < song.patterns.size() + 1; ++i)
    {
        const auto pattern = song.patterns[i % song.patterns.size()];
        for (size_t c = 0; c < 3; ++c)
        {
            const auto ms =
                openPattern (view, session, pattern, song.channels[(i * 7 + c * 13) % song.channels.size()]);
            if (timings.opens == 0)
                timings.firstOpen = ms;
            timings.worstOpen = std::max (timings.worstOpen, ms);
            timings.totalOpen += ms;
            ++timings.opens;
            if (std::getenv ("SPINUS_PERF_VERBOSE") != nullptr)
                std::cout << "open " << timings.opens << ": " << ms << " ms" << std::endl;
        }
    }
    // Editing the song-long pattern: notes added and moved in the Piano Roll, one undo step each.
    if (auto* roll = dynamic_cast<PianoRoll*> (view.getWorkspace().getPanel ("pianoRoll")))
    {
        CHECK ((int)roll->getNotes().size() == notesPerPart); // back on the song-long pattern
        for (int i = 0; i < 4; ++i)
        {
            const auto begin = Clock::now();
            REQUIRE (roll->addNote (60 + i, 3840 * (i + 1), 240, 0.8f));
            REQUIRE (roll->moveNote (0, 240, 1));
            settle();
            timings.worstEdit = std::max (timings.worstEdit, msSince (begin) / 2.0);
            if (std::getenv ("SPINUS_PERF_VERBOSE") != nullptr)
                std::cout << "edit " << i << ": " << msSince (begin) / 2.0 << " ms" << std::endl;
        }
        CHECK ((int)roll->getNotes().size() == notesPerPart + 4);
    }

    // Profiling: drawing the window again and again, then back and forth between the song-long pattern and a
    // section.
    if (const auto* paints = std::getenv ("SPINUS_PERF_PAINTS"))
        for (int i = 0; i < juce::String (paints).getIntValue(); ++i)
        {
            const auto begin = Clock::now();
            view.createComponentSnapshot (view.getLocalBounds());
            std::cout << "paint " << i << ": " << msSince (begin) << " ms" << std::endl;
        }
    if (const auto* switches = std::getenv ("SPINUS_PERF_SWITCHES"))
        for (int i = 0; i < juce::String (switches).getIntValue(); ++i)
        {
            const auto begin = Clock::now();
            session.setCurrentPattern (song.patterns[(size_t)(i % 2 == 0 ? 1 : 0)]);
            std::cout << "switch " << i << ": " << msSince (begin) << " ms" << std::endl;
        }

    if (save)
    {
        // The autosave (every two minutes, while the project is still untitled too), then Save As.
        start = Clock::now();
        const auto autosaved = document.autosave();
        timings.autosave = msSince (start);
        INFO (autosaved.getErrorMessage());
        CHECK (autosaved.wasOk());

        start = Clock::now();
        const auto saved = document.saveAs (t.root.getChildFile ("Song"));
        timings.save = msSince (start);
        INFO (saved.getErrorMessage());
        CHECK (saved.wasOk());
        CHECK (project::isProjectFolder (t.root.getChildFile ("Song")));
    }

    std::cout << parts << " instruments, " << song.notes << " notes: added in " << timings.add
              << " ms; opening a pattern " << timings.totalOpen / timings.opens << " ms on average, "
              << timings.worstOpen << " ms at worst (first " << timings.firstOpen << " ms, " << timings.opens
              << " opens); an edit " << timings.worstEdit << " ms at worst";
    if (save)
        std::cout << "; autosaved in " << timings.autosave << " ms, saved in " << timings.save << " ms";
    std::cout << std::endl;
    return timings;
}
} // namespace

TEST_CASE ("Many instruments: opening patterns and saving stay quick", "[ui][perf]")
{
    // 53 parts of 1500 notes (about 80 000 notes), as the 53-stem analysis of a four-minute song makes. The
    // bounds are generous (a debug or sanitizer build is several times slower); a slow open takes
    // seconds.
    const auto timings = measure (53, 1500, true);
#if JUCE_DEBUG
    constexpr double scale = 8.0;
#else
    constexpr double scale = 1.0;
#endif
    CHECK (timings.worstOpen < 400.0 * scale);
    CHECK (timings.totalOpen / timings.opens < 150.0 * scale);
    CHECK (timings.worstEdit < 300.0 * scale);
    CHECK (timings.autosave < 5000.0 * scale);
    CHECK (timings.save < 5000.0 * scale);
}

// For profiling by hand, with more parts, more notes and real plugins (SPINUS_PERF_VERBOSE=1 times each step,
// SPINUS_PERF_SWITCHES=n and SPINUS_PERF_PAINTS=n repeat a pattern switch or a redraw):
//   SPINUS_PERF_PARTS=120 SPINUS_PERF_NOTES=3000 SPINUS_PERF_PLUGINS=/path/Odin2.clap:/path/OB-Xf.clap
//   build/release/tests/spinus_tests "[.perf-many]"
TEST_CASE ("Many instruments, sized by hand", "[.perf-many]")
{
    const auto* parts = std::getenv ("SPINUS_PERF_PARTS");
    const auto* notes = std::getenv ("SPINUS_PERF_NOTES");
    measure (parts != nullptr ? juce::String (parts).getIntValue() : 53,
             notes != nullptr ? juce::String (notes).getIntValue() : 1500, true);
}
