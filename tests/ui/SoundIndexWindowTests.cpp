#include "FakeProvider.h"
#include "engine/EngineHost.h"
#include "session/Session.h"
#include "session/InstrumentProfiles.h"
#include "session/LeftOutSounds.h"
#include "ui/SoundIndexWindow.h"
#include "ui/Theme.h"

#include <catch2/catch_test_macros.hpp>

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;

#if defined(SPINUS_PLUGIN_SCANNER) && defined(SPINUS_BROKEN_PLUGINS_DIR)
namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-sound-index-window-" + juce::Uuid().toString());
};

SoundSource threeOsc()
{
    SoundSource s;
    s.key = "stock-spinus.3x-osc";
    s.name = "3x Osc";
    s.format = "Spinus";
    s.file = "spinus:3x-osc";
    s.stockType = "spinus.3x-osc";
    s.badge = "Spinus";
    s.fingerprint = "Spinus test";
    return s;
}

SoundSource testSynth()
{
    SoundSource s;
    s.key = "plugin-test-synth.clap";
    s.name = s.pluginName = "Spinus Test Synth";
    s.format = "CLAP";
    s.badge = "CLAP";
    s.file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName();
    s.fingerprint = "1";
    return s;
}

void writeTone (const juce::File& file, double hz)
{
    file.getParentDirectory().createDirectory();
    juce::AudioBuffer<float> b (1, 22050);
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i,
                     (float)(0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / 44100.0) *
                             std::exp (-i / 6000.0)));
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

/** A session whose sound index is kept in a temporary folder, measures 3x Osc and the test synth, and the
    samples in one folder. */
struct Fixture
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    SoundIndexService& index = session.getSoundIndex();
    juce::File samples = t.root.getChildFile ("Samples");

    Fixture()
    {
        index.setFolder (t.root.getChildFile ("ai/sounds"));
        SoundIndexer::Options options;
        options.helper = juce::File (SPINUS_PLUGIN_SCANNER);
        index.setOptions (options);
        index.getSources = [] { return std::vector<SoundSource> { threeOsc(), testSynth() }; };
        index.getSampleFolders = [this] { return juce::Array<juce::File> { samples }; };
        writeTone (samples.getChildFile ("low.wav"), 55.0);
        writeTone (samples.getChildFile ("Leads/high.wav"), 880.0);
    }

    ~Fixture() { SoundIndexWindow::close(); }

    bool waitForIt()
    {
        return waitFor ([&] { return ! index.isUpdating() && index.getWaiting() == 0; }, 180000);
    }
};

void settle() { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); }

const SoundIndexPage::InstrumentRow* rowOf (const SoundIndexPage& page, const juce::String& key)
{
    for (const auto& r : page.getInstruments())
        if (r.source.key == key)
            return &r;
    return nullptr;
}

juce::SparseSet<int> rowSet (int row)
{
    juce::SparseSet<int> set;
    set.addRange ({ row, row + 1 });
    return set;
}

int rowIndex (const SoundIndexPage& page, const juce::String& key)
{
    for (size_t i = 0; i < page.getInstruments().size(); ++i)
        if (page.getInstruments()[i].source.key == key)
            return (int)i;
    return -1;
}
} // namespace

TEST_CASE ("Options > Sound index lists, ticks, measures, forgets and deletes", "[ui][sounds][index][helper]")
{
    Fixture f;
    SoundIndexWindow::show (f.index, nullptr);
    auto* page = SoundIndexWindow::getOpenPage();
    REQUIRE (page != nullptr);
    CHECK (page->getStatusText().contains ("No sound index yet"));

    // Spinus's own is always measured; the user's plugin only once ticked.
    REQUIRE (rowOf (*page, threeOsc().key) != nullptr);
    CHECK (rowOf (*page, threeOsc().key)->always);
    CHECK (rowOf (*page, threeOsc().key)->measured);
    REQUIRE (rowOf (*page, testSynth().key) != nullptr);
    CHECK_FALSE (rowOf (*page, testSynth().key)->measured);
    CHECK (rowOf (*page, testSynth().key)->state.contains ("tick it"));
    page->toggle (rowIndex (*page, threeOsc().key)); // stays ticked
    settle();
    CHECK (rowOf (*page, threeOsc().key)->measured);
    page->toggle (rowIndex (*page, testSynth().key));
    REQUIRE (waitFor ([&] { return rowOf (*page, testSynth().key)->measured; }, 10000));
    CHECK (f.index.getIncludedPlugins() == juce::StringArray { testSynth().key });
    CHECK (rowOf (*page, testSynth().key)->measured);
    CHECK (rowOf (*page, testSynth().key)->state.contains ("Not measured yet"));

    CHECK (f.index.getIndexedSources().size() == 2); // the ticked ones are what an update measures

    f.index.update();
    CHECK (f.index.isUpdating());
    CHECK (waitFor ([&] { return page->getStatusText().contains ("Updating"); }, 10000));
    REQUIRE (f.waitForIt());
    REQUIRE (waitFor ([&] { return page->getStatusText().contains ("2 instruments"); }, 10000));
    CHECK (page->getStatusText().contains ("2 samples"));
    CHECK (rowOf (*page, testSynth().key)->state.contains ("6 presets measured"));
    // The user's plugin is not learned: it says so (Spinus's own needn't be).
    INFO (rowOf (*page, testSynth().key)->state);
    CHECK (rowOf (*page, testSynth().key)->state.contains ("not learned"));
    CHECK_FALSE (rowOf (*page, threeOsc().key)->state.contains ("learned"));
    REQUIRE (page->getFolders().size() == 1);
    CHECK (page->getFolders()[0].samples == 2);
    CHECK (page->getSounds().getShown().size() > 6);

    // Show its sounds: the Sounds tab, searched for it.
    page->showSoundsOf (rowIndex (*page, testSynth().key));
    CHECK (page->getTabs().getCurrentTabIndex() == 1);
    CHECK (page->getSounds().getShown().size() == 6);
    CHECK (page->getSounds().getFrom() == testSynth().name);

    // The filters: only samples, only presets, only problems; a folder's samples from the Samples tab.
    page->getSounds().setFilter (SoundIndexView::Show::samples);
    CHECK (page->getSounds().getShown().size() == 2);
    page->getSounds().setFilter (SoundIndexView::Show::presets);
    CHECK (std::none_of (page->getSounds().getShown().begin(), page->getSounds().getShown().end(),
                         [] (const SoundIndex::Row& r) { return r.sample; }));
    page->getSounds().setFilter (SoundIndexView::Show::problems);
    CHECK (page->getSounds().getShown().empty());
    page->getTabs().setCurrentTabIndex (2);
    page->showSamplesOf (0, false);
    CHECK (page->getTabs().getCurrentTabIndex() == 1);
    CHECK (page->getSounds().getShow() == SoundIndexView::Show::samples);
    CHECK (page->getSounds().getShown().size() == 2);
    page->getSounds().setFilter (SoundIndexView::Show::samples, f.samples.getChildFile ("Leads").getFullPathName());
    CHECK (page->getSounds().getShown().size() == 1); // a folder holds the folders inside it

    // Listen: a sample plays as it is.
    juce::File played;
    f.index.play = [&] (const juce::File& file) { played = file; };
    juce::String heard = "not yet";
    f.index.listen (page->getSounds().getShown()[0], [&] (const juce::String& problem) { heard = problem; });
    CHECK (heard.isEmpty());
    CHECK (played == f.samples.getChildFile ("Leads/high.wav"));

    // Leave out: one sample, then a preset; they leave the index and an update doesn't bring them back.
    auto& sounds = page->getSounds();
    sounds.selectRows (rowSet (0));
    sounds.actOnSelected();
    CHECK (sounds.getMessage().startsWith ("1 sound left out"));
    page->getSounds().setFilter (SoundIndexView::Show::presets, testSynth().name);
    REQUIRE (waitFor ([&] { return page->getSounds().getShown().size() == 6; }, 10000));
    const auto preset = page->getSounds().getShown()[2];
    page->getSounds().selectRows (rowSet (2));
    page->getSounds().actOnSelected();
    REQUIRE (waitFor ([&] { return page->getSounds().getShown().size() == 5; }, 10000));
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK (saved.samples.size() == 1);
        const auto& kept = saved.instruments[testSynth().key].sounds;
        CHECK (std::none_of (kept.begin(), kept.end(), [&] (const juce::var& s) { return s["id"] == preset.id; }));
    }
    page->getSounds().setFilter (SoundIndexView::Show::leftOut);
    CHECK (page->getSounds().getShown().size() == 2);
    f.index.update();
    REQUIRE (f.waitForIt());
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK (saved.samples.size() == 1);
        CHECK (saved.instruments[testSynth().key].sounds.size() == 5);
    }

    // The Role menu: only one role's sounds.
    page->getSounds().setFilter (SoundIndexView::Show::everything);
    const auto aRole = page->getSounds().getShown()[0].role;
    page->getSounds().setRole (aRole);
    CHECK (std::all_of (page->getSounds().getShown().begin(), page->getSounds().getShown().end(),
                        [&] (const SoundIndex::Row& r) { return r.role == aRole; }));
    page->getSounds().setRole ({});

    // Use as, on several selected presets at once.
    page->getSounds().setFilter (SoundIndexView::Show::presets, testSynth().name);
    {
        juce::SparseSet<int> three;
        three.addRange ({ 0, 3 });
        page->getSounds().selectRows (three);
    }
    CHECK (page->getSounds().markSelected ("bass") == 3);
    for (int row = 0; row < 3; ++row)
        CHECK (page->getSounds().markOf (row) == "bass");
    CHECK (page->getSounds().markOf (3).isEmpty());
    CHECK (page->getSounds().markSelected ({}) == 3);
    CHECK (page->getSounds().markOf (0).isEmpty());
    page->getSounds().setFilter (SoundIndexView::Show::everything);

    // Put back: the update measures them again.
    REQUIRE (waitFor ([&] { return ! f.index.isUpdating(); }, 10000));
    page->getSounds().setFilter (SoundIndexView::Show::leftOut);
    page->getSounds().actOnAllShown(); // two: asks first
    settle();
    if (auto* box = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent()))
        box->exitModalState (0);
    settle();
    {
        LeftOutSounds left (f.index.getFolder());
        left.load();
        CHECK (left.getEntries().size() == 2); // not before Yes
    }
    {
        std::vector<SoundIndex::Row> rows (page->getSounds().getShown());
        page->getSounds().onPutBack (rows);
    }
    REQUIRE (f.waitForIt());
    {
        LeftOutSounds left (f.index.getFolder());
        left.load();
        CHECK (left.getEntries().empty());
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK (saved.samples.size() == 2);
        CHECK (saved.instruments[testSynth().key].sounds.size() == 6);
    }
    page->getSounds().setFilter (SoundIndexView::Show::everything);

    // Forget: its sounds leave the index, and it is unticked.
    page->forget (rowIndex (*page, testSynth().key));
    CHECK (waitFor ([&] { return ! rowOf (*page, testSynth().key)->measured; }, 10000));
    CHECK (f.index.getIncludedPlugins().isEmpty());
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK_FALSE (saved.instruments.contains (testSynth().key));
        CHECK (saved.instruments.contains (threeOsc().key));
    }

    // Measure again: one instrument measured anew, ticked again.
    page->measureAgain (rowIndex (*page, testSynth().key));
    REQUIRE (f.waitForIt());
    CHECK (f.index.getIncludedPlugins() == juce::StringArray { testSynth().key });
    CHECK (f.index.getLastResult().value().soundsMeasured == 6);
    CHECK (f.index.getStatusText().startsWith ("Spinus Test Synth: 6 sounds measured."));
    CHECK (waitFor ([&] { return rowOf (*page, testSynth().key)->state.contains ("6 presets measured"); }, 10000));

    // Learned (it plays C4 to C6 only): the next update measures it again, its test notes on those keys.
    {
        InstrumentProfiles profiles (f.index.getFolder());
        profiles.load();
        InstrumentProfiles::Profile profile;
        profile.modes.emplace_back();
        profile.modes[0].setKeys (60, 84, { InstrumentProfiles::Job::plays, {}, InstrumentProfiles::Source::user });
        profiles.set (testSynth().key, profile);
        REQUIRE (profiles.save());
    }
    page->reload();
    CHECK (rowOf (*page, testSynth().key)->learned);
    CHECK (rowOf (*page, testSynth().key)->state.contains ("learned since it was measured"));
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK (saved.learnedKeys (testSynth().key) == "60-84");
        CHECK (saved.needsVisit (testSynth()));
    }
    f.index.update();
    REQUIRE (f.waitForIt());
    CHECK (waitFor ([&] { return rowOf (*page, testSynth().key)->state.contains ("measured on the keys it plays"); },
                    10000));
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK (saved.instruments[testSynth().key].playedKeys == "60-84");
        CHECK_FALSE (saved.needsVisit (testSynth()));
        CHECK (saved.instruments[testSynth().key].sounds.size() == 6);
        for (const auto& sound : saved.instruments[testSynth().key].sounds)
            CHECK ((int)sound["low"] >= 60);
    }

    // Delete, after the user said yes.
    CHECK (page->deleteIndex());
    settle();
    CHECK_FALSE (f.index.getFolder().getChildFile ("index.json").exists());
    CHECK (page->getStatusText().contains ("No sound index yet"));
}

TEST_CASE ("Sound index: Refresh sound index measures one sample or folder again, and waits for an update",
           "[ui][sounds][index][helper]")
{
    Fixture f;
    f.index.update();
    // Asked while the update runs: it waits, then runs.
    f.index.refreshSamples ({ f.samples.getChildFile ("Leads") });
    CHECK (f.index.getWaiting() == 1);
    CHECK (f.index.getStatusText().contains ("then 1 more"));
    REQUIRE (f.waitForIt());
    CHECK (f.index.getLastResult().value().samplesMeasured == 1); // the refresh, after the update's 2
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        CHECK (saved.samples.size() == 2);
        CHECK (saved.instruments.size() == 1); // 3x Osc: the test synth isn't ticked
    }

    // A file that is gone leaves the index when its folder is refreshed; the others stay.
    f.samples.getChildFile ("Leads/high.wav").deleteFile();
    f.index.refreshSamples ({ f.samples.getChildFile ("Leads") });
    REQUIRE (f.waitForIt());
    CHECK (f.index.getLastResult().value().samplesRemoved == 1);
    {
        SoundIndex saved (f.index.getFolder());
        saved.load();
        REQUIRE (saved.samples.size() == 1);
        CHECK (saved.samples[0].path.endsWith ("low.wav"));
    }

    // One file.
    f.index.refreshSamples ({ f.samples.getChildFile ("low.wav") });
    REQUIRE (f.waitForIt());
    CHECK (f.index.getLastResult().value().samplesMeasured == 1);

    // An instrument Spinus can't measure is said so; Stop drops what waits.
    CHECK_FALSE (f.index.refreshInstrument ("plugin-not-installed"));
    f.index.update();
    f.index.refreshSamples ({ f.samples });
    f.index.cancel();
    CHECK (f.index.getWaiting() == 0);
    REQUIRE (f.waitForIt());
}
TEST_CASE ("Options > Sound index screenshot for the manual", "[.screenshot][manual]")
{
    SpinusLookAndFeel look (Theme::dark());
    juce::LookAndFeel::setDefaultLookAndFeel (&look);
    {
        Fixture f;
        f.index.setIncluded (testSynth().key, true);
        f.index.update();
        REQUIRE (f.waitForIt());
        SoundIndexPage page (f.index);
        page.setSize (860, 560);
        const auto file =
            juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/sound-index.png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        const auto image = page.createComponentSnapshot (page.getLocalBounds());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    }
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}
#endif
