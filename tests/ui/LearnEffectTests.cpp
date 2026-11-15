#include <catch2/catch_test_macros.hpp>

#include "FakeProvider.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/InstrumentPresets.h"
#include "ui/LearnEffectWindow.h"
#include "ui/Mixer.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-learn-effect-" + juce::Uuid().toString());
};

std::vector<Session::ParameterText> compressorSettings()
{
    return { { "p1", "Thresh", "-12.0 dB", "-60.0 dB", "0.0 dB" },
             { "p2", "Param 7", "40 %", "0 %", "100 %" },
             { "p3", "Attack", "10 ms", "0.1 ms", "300 ms" } };
}
} // namespace

TEST_CASE ("Effect profiles: kept, read back, shared and told to agents", "[effects][profiles]")
{
    TempFolder t;
    const auto index = t.root.getChildFile ("ai/sounds");
    EffectProfiles::Profile profile;
    profile.goodFor = "glue on drum buses";
    profile.settings.push_back ({ "p1", "Thresh", "level where it starts to squash", "-30 dB", "-6 dB", true });
    profile.settings.push_back ({ "p2", "Param 7", {}, {}, {}, false }); // nothing learned: left out
    profile.manuals.push_back ({ "https://example.org/manual.pdf", {} });
    profile.manuals.push_back ({ "/home/someone/manual.pdf", {} });
    {
        EffectProfiles profiles (index);
        profiles.set ("plugin-abc", profile);
        REQUIRE (profiles.save());
    }
    CHECK (EffectProfiles::fileIn (index) == t.root.getChildFile ("ai/effect-profiles.json"));
    EffectProfiles again (index);
    again.load();
    const auto read = again.get ("plugin-abc");
    REQUIRE (read.has_value());
    CHECK (read.value().goodFor == "glue on drum buses");
    REQUIRE (read.value().settings.size() == 1);
    CHECK (read.value().settings[0].what == "level where it starts to squash");
    CHECK (read.value().settings[0].matters);
    CHECK_FALSE (again.get ("plugin-other").has_value());

    // For agents: what it suits and the learned settings with their safe ranges.
    const auto told = EffectProfiles::describeForAgents (read.value());
    CHECK (told["goodFor"].toString() == "glue on drum buses");
    REQUIRE (told["settings"].size() == 1);
    CHECK (told["settings"][0]["safeRange"].toString() == "-30 dB to -6 dB");
    CHECK ((bool)told["settings"][0]["matters"]);
    CHECK (EffectProfiles::describeForAgents ({}).isVoid());

    // Shared: only web manuals; read back as imported.
    const auto shared = EffectProfiles::toShared (read.value(), "Bus Squasher");
    CHECK (shared["profile"]["manuals"].size() == 1);
    juce::String name, problem;
    const auto imported = EffectProfiles::fromShared (juce::JSON::parse (juce::JSON::toString (shared)), name, problem);
    REQUIRE (imported.has_value());
    CHECK (name == "Bus Squasher");
    CHECK (imported.value().settings[0].source == EffectProfiles::Source::imported);
    CHECK_FALSE (EffectProfiles::fromShared (juce::var ("nope"), name, problem).has_value());
    CHECK (problem.contains ("not an effect profile"));

    // Forgotten.
    again.set ("plugin-abc", std::nullopt);
    REQUIRE (again.save());
    EffectProfiles none (index);
    none.load();
    CHECK_FALSE (none.get ("plugin-abc").has_value());
}

TEST_CASE ("Learn this effect: settings as the plugin reports them, edited, searched and imported",
           "[ui][effects][profiles]")
{
    EffectProfiles::Profile before;
    before.settings.push_back (
        { "p3", "Attack", "how fast it reacts", {}, {}, false, EffectProfiles::Source::manual }); // not the user's
    before.settings.push_back ({ "gone", "Old knob", "an older version's", {}, {}, false });
    LearnEffectPanel panel ("Bus Squasher", compressorSettings(), before);
    REQUIRE (panel.getNumShown() == 4); // the plugin's three, and one an older version had
    CHECK (panel.getRowText (0).startsWith ("Thresh | -60.0 dB to 0.0 dB"));
    CHECK (panel.getRowText (2).contains ("how fast it reacts"));

    panel.setWhat (1, "dry to wet");
    panel.setRange (0, "-30 dB", "-6 dB");
    panel.setMatters (0, true);
    panel.setGoodFor ("glue on drum buses");
    auto profile = panel.getProfile();
    CHECK (profile.goodFor == "glue on drum buses");
    REQUIRE (profile.find ("p1") != nullptr);
    CHECK (profile.find ("p1")->low == "-30 dB");
    CHECK (profile.find ("p1")->matters);
    CHECK (profile.find ("p2")->what == "dry to wet");
    CHECK (profile.find ("gone") != nullptr);

    panel.search ("param");
    CHECK (panel.getNumShown() == 1);
    panel.search ("wet"); // what it does is searched too
    CHECK (panel.getNumShown() == 1);
    panel.search ({});

    // An imported profile fills what the user has not set; the user's word wins.
    EffectProfiles::Profile theirs;
    theirs.goodFor = "their words";
    theirs.settings.push_back ({ "p2", "Param 7", "their mix", {}, {}, false });
    theirs.settings.push_back ({ "p3", "Attack", "their attack", "1 ms", "30 ms", true });
    theirs.settings.push_back ({ "p9", "Not here", "a setting this version lacks", {}, {}, false });
    const auto said = panel.importShared (EffectProfiles::toShared (theirs, "Bus Squasher"), "squasher.json");
    CHECK (said.contains ("1 setting filled"));
    profile = panel.getProfile();
    CHECK (profile.goodFor == "glue on drum buses");
    CHECK (profile.find ("p2")->what == "dry to wet");
    CHECK (profile.find ("p3")->what == "their attack"); // set by the earlier profile, not by the user now
    CHECK (profile.find ("p3")->source == EffectProfiles::Source::imported);
    CHECK (profile.find ("p9") == nullptr);
    CHECK (panel.importShared (juce::var ("nope"), "bad.json").contains ("could not be read"));
}

TEST_CASE ("Learn this effect: the manual's reading proposed, applied around what the user set",
           "[ui][effects][profiles]")
{
    // The service's answer.
    const auto answer = juce::JSON::parse (R"({
        "goodFor": "glue on drum buses",
        "settings": [{"id": "p1", "what": "where it starts to squash", "low": "-30 dB", "high": "-6 dB", "matters": true},
                     {"id": "p2", "what": "dry to wet", "low": "", "high": "", "matters": false},
                     {"id": "p3", "what": "", "low": "", "high": "", "matters": false}],
        "notes": "Slow attacks keep the punch.", "by": "claude-test",
        "read": [{"source": "a.pdf", "pages": 3}, {"source": "https://gone.example.org", "problem": "the page answered 404."}]})");
    const auto reading = EffectProfiles::readingFrom (answer);
    REQUIRE (reading.has_value());
    CHECK (reading.value().goodFor == "glue on drum buses");
    REQUIRE (reading.value().settings.size() == 2); // p3 says nothing
    CHECK (reading.value().settings[0].source == EffectProfiles::Source::manual);
    CHECK (reading.value().problems.size() == 1);
    CHECK_FALSE (EffectProfiles::readingFrom (juce::JSON::parse (R"({"settings": []})")).has_value());

    LearnEffectPanel panel ("Bus Squasher", compressorSettings(), std::nullopt);
    panel.addManual ("www.example.org/squasher");
    panel.addManual ("not a file");
    panel.addManual ("https://example.org/other");
    REQUIRE (panel.getManuals().size() == 2);
    CHECK (panel.getManuals()[0].source == "https://www.example.org/squasher");
    CHECK (panel.getMessage().contains ("Choose file"));
    panel.removeManual (1);
    CHECK (panel.getManuals().size() == 1);

    panel.setWhat (1, "my own words"); // the user's: stays
    panel.setReading (reading);
    CHECK (panel.hasReading());
    CHECK (panel.getMessage().contains ("Apply fills 1 setting"));
    CHECK (panel.getMessage().contains ("Not read"));
    CHECK (panel.getProfile().find ("p1") == nullptr); // nothing taken before Apply
    CHECK (panel.applyReading() == 1);
    CHECK_FALSE (panel.hasReading());
    const auto profile = panel.getProfile();
    CHECK (profile.goodFor == "glue on drum buses");
    REQUIRE (profile.find ("p1") != nullptr);
    CHECK (profile.find ("p1")->low == "-30 dB");
    CHECK (profile.find ("p1")->name == "Thresh");
    CHECK (profile.find ("p2")->what == "my own words");
    REQUIRE (profile.manuals.size() == 1); // kept with the profile

    panel.setReading (reading);
    panel.discardReading();
    CHECK_FALSE (panel.hasReading());
    panel.setReading (std::nullopt, "the AI could not be reached.");
    CHECK (panel.getMessage() == "The manual was not read: the AI could not be reached.");
}

TEST_CASE ("Learn this effect: a real effect's settings and their ranges, saved beside the sound index",
           "[ui][effects][profiles]")
{
    TempFolder t;
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session { host.getEngine(), std::move (devices) };
    Mixer mixer (session);
    REQUIRE (mixer.addEffect (model::masterInsertId, "spinus.filter"));
    const auto& filter = session.getProject().mixer.master.effects.front().device;

    const auto settings = session.getDeviceParameterTexts (filter.id);
    REQUIRE_FALSE (settings.empty());
    const auto cutoff = std::find_if (settings.begin(), settings.end(),
                                      [] (const Session::ParameterText& p) { return p.id == "cutoff"; });
    REQUIRE (cutoff != settings.end());
    CHECK (cutoff->low.isNotEmpty());
    CHECK (cutoff->high.isNotEmpty());
    CHECK (cutoff->low != cutoff->high);

    const auto index = t.root.getChildFile ("ai/sounds");
    const auto key = InstrumentPresets::libraryOf (filter);
    LearnEffectWindow::open (index, key, "Filter", settings, nullptr);
    REQUIRE (LearnEffectWindow::getNumOpen() == 1);
    LearnEffectWindow::open (index, key, "Filter", settings, nullptr); // brought to the front, not twice
    CHECK (LearnEffectWindow::getNumOpen() == 1);
    auto* panel = LearnEffectWindow::lastOpened();
    REQUIRE (panel != nullptr);
    panel->setGoodFor ("taming harsh highs");
    panel->onSave (panel->getProfile());
    CHECK (test::waitFor ([] { return LearnEffectWindow::getNumOpen() == 0; }, 5000));
    EffectProfiles saved (index);
    saved.load();
    REQUIRE (saved.get (key).has_value());
    CHECK (saved.get (key).value().goodFor == "taming harsh highs");
    LearnEffectWindow::closeAll();
}

TEST_CASE ("Learn this effect screenshot for the manual", "[.screenshot][manual]")
{
    SpinusLookAndFeel look (Theme::dark());
    juce::LookAndFeel::setDefaultLookAndFeel (&look);
    {
        std::vector<Session::ParameterText> settings {
            { "p1", "Thresh", "-12.0 dB", "-60.0 dB", "0.0 dB" }, { "p2", "Ratio", "4.0:1", "1.0:1", "20.0:1" },
            { "p3", "Attack", "10 ms", "0.1 ms", "300 ms" },      { "p4", "Release", "120 ms", "5 ms", "2000 ms" },
            { "p5", "Param 5", "0.0 dB", "0.0 dB", "24.0 dB" },   { "p6", "Param 6", "100 %", "0 %", "100 %" },
            { "p7", "SC HPF", "Off", "Off", "500 Hz" },           { "p8", "Knee", "6.0 dB", "0.0 dB", "24.0 dB" }
        };
        EffectProfiles::Profile profile;
        profile.goodFor = "glue on drum buses and mixes; gentle levelling of vocals";
        profile.settings.push_back ({ "p1", "Thresh", "level where it starts to turn down", "-30 dB", "-6 dB", true });
        profile.settings.push_back (
            { "p2", "Ratio", "how much it turns down above the threshold", "1.5:1", "6:1", true });
        profile.settings.push_back ({ "p3", "Attack", "how fast it reacts; slower keeps the punch", "1 ms", "50 ms" });
        profile.settings.push_back ({ "p5", "Param 5", "make-up gain", "0 dB", "12 dB" });
        profile.settings.push_back ({ "p6", "Param 6", "dry to wet (parallel compression)", {}, {} });
        profile.manuals.push_back ({ "/home/me/Manuals/Bus Compressor.pdf", {} });
        LearnEffectPanel panel ("a bus compressor", settings, profile);
        panel.setSize (900, 460);
        const auto file =
            juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/learn-effect.png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        const auto image = panel.createComponentSnapshot (panel.getLocalBounds());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    }
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}
