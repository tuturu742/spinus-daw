#include <catch2/catch_test_macros.hpp>

#include "session/PartModes.h"

using namespace spinus;
using Job = InstrumentProfiles::Job;
using Profile = InstrumentProfiles::Profile;

namespace
{
juce::var parse (const char* json) { return juce::JSON::parse (juce::String (json)); }

/** A sampled guitar: Normal (E2 to E6, palm mute and slide switches low), Riff (entered by C#7, its own keys),
    Strummer (entered by C#8, no playing keys) and Tapping (a plugin setting, no key). */
Profile guitar()
{
    Profile p;
    InstrumentProfiles::Mode normal;
    normal.goodFor = "picked lines, metal rhythm with palm mutes";
    normal.voices = InstrumentProfiles::Voices::one;
    normal.setKeys (40, 88, { Job::plays, {}, InstrumentProfiles::Source::probing });
    normal.setKeys (24, 24, { Job::switches, "palm mute", InstrumentProfiles::Source::manual });
    normal.setKeys (25, 25, { Job::switches, "slide", InstrumentProfiles::Source::manual });
    normal.setKeys (26, 26, { Job::switches, "open", InstrumentProfiles::Source::manual });
    normal.setKeys (97, 97, { Job::switches, "strummer", InstrumentProfiles::Source::manual });
    InstrumentProfiles::Mode riff;
    riff.name = "Riff";
    riff.enterKey = 85;
    riff.goodFor = "funk riffs";
    riff.setKeys (48, 72, { Job::plays, {}, InstrumentProfiles::Source::probing });
    InstrumentProfiles::Mode strummer;
    strummer.name = "Strummer";
    strummer.enterKey = 97;
    strummer.goodFor = "strummed rhythm, country and folk";
    strummer.setKeys (36, 59, { Job::special, "chord keys", InstrumentProfiles::Source::manual });
    InstrumentProfiles::Mode tapping;
    tapping.name = "Tapping";
    tapping.goodFor = "country tapping";
    tapping.setKeys (40, 88, { Job::plays, {}, InstrumentProfiles::Source::user });
    p.modes = { normal, riff, strummer, tapping };
    return p;
}

/** A sketch song with a guitar layer playing in a verse and a chorus, resting in the bridge. */
juce::var song (const char* modes = "{}")
{
    auto s = parse (R"({
        "sections": [{"id": "verse"}, {"id": "chorus"}, {"id": "bridge"}],
        "layerParts": {"gtr": {
            "verse": [{"start": 0, "length": 240, "pitch": 30, "velocity": 0.8},
                      {"start": 480, "length": 240, "pitch": 64, "velocity": 0.8}],
            "chorus": [{"start": 4, "length": 240, "pitch": 100, "velocity": 0.8}],
            "bridge": []
        }},
        "parts": {}
    })");
    s.getDynamicObject()->setProperty ("modes", parse (modes));
    return s;
}

juce::Array<juce::var> notes (const juce::var& song, const char* section)
{
    return *song["layerParts"]["gtr"][section].getArray();
}
} // namespace

TEST_CASE ("Part modes: the modes songwriting can use, and what the AI is told of them", "[instruments][songwriting]")
{
    const auto g = guitar();
    // Strummer has no playing keys (a later step), Tapping has no key to enter it.
    CHECK (partmodes::usable (g) == std::vector<int> { 0, 1 });

    const auto told = partmodes::brief (g);
    REQUIRE (told.size() == 2);
    CHECK (told[0]["name"].toString() == "Normal");
    CHECK ((bool)told[0]["first"]);
    CHECK ((int)told[0]["low"] == 40);
    CHECK ((int)told[0]["high"] == 88);
    CHECK (told[0]["voices"].toString() == "one note at a time");
    // The switch that enters a mode is not an articulation.
    REQUIRE (told[0]["switches"].size() == 3);
    CHECK (told[0]["switches"][0]["what"].toString() == "palm mute");
    CHECK ((int)told[0]["switches"][0]["key"] == 24);
    CHECK (told[1]["name"].toString() == "Riff");
    CHECK_FALSE (told[1].hasProperty ("first"));
    CHECK_FALSE (told[1].hasProperty ("switches"));

    Profile none;
    CHECK (partmodes::brief (none).size() == 0);
    CHECK (partmodes::pick (none, "rhythm guitar") == -1);
}

TEST_CASE ("Part modes: a mode picked by the words of what it is good for", "[instruments][songwriting]")
{
    const auto g = guitar();
    CHECK (partmodes::pick (g, "Guitar, funk riffs") == 1);
    CHECK (partmodes::pick (g, "Rhythm guitar, funk") == 0); // as good a match: the first
    CHECK (partmodes::pick (g, "Lead guitar, metal") == 0);
    CHECK (partmodes::pick (g, "Guitar, country") == 0); // Strummer and Tapping are not usable yet
    CHECK (partmodes::pick (g, "") == 0);
}

TEST_CASE ("Part modes: the song's own choice, checked against the profile", "[instruments][songwriting]")
{
    const auto g = guitar();
    auto use = partmodes::useFor (song (R"({"gtr": {"mode": "riff", "articulations": {"verse": "Palm Mute"}}})"), "gtr",
                                  g, "");
    CHECK (use.mode == 1);
    CHECK_FALSE (use.picked);
    CHECK (use.articulations.empty()); // Riff has no palm mute

    use = partmodes::useFor (song (R"({"gtr": {"mode": "Normal", "articulations": {"verse": "Palm Mute",
                                      "chorus": "wah"}}})"),
                             "gtr", g, "");
    CHECK (use.mode == 0);
    REQUIRE (use.articulations.size() == 1);
    CHECK (use.articulations["verse"] == "palm mute");

    // A mode the song can't use (Strummer has no playing keys yet): picked by the words instead.
    use = partmodes::useFor (song (R"({"gtr": {"mode": "Strummer"}})"), "gtr", g, "funk");
    CHECK (use.mode == 1);
    CHECK (use.picked);
}

TEST_CASE ("Part modes: notes kept to the mode's keys, switch keys written at each section's start",
           "[instruments][songwriting]")
{
    std::map<juce::String, partmodes::Instrument> instruments { { "gtr", { guitar(), "lead guitar" } } };

    SECTION ("Normal mode: palm mute in the verse carries into the chorus; no switch where the part rests")
    {
        const auto out = partmodes::apply (
            song (R"({"gtr": {"mode": "Normal", "articulations": {"verse": "palm mute"}}})"), instruments);
        const auto verse = notes (out, "verse");
        REQUIRE (verse.size() == 3);
        CHECK ((int)verse[0]["pitch"] == 24);
        CHECK ((bool)verse[0]["switch"]);
        CHECK ((int)verse[0]["start"] == 0);
        CHECK ((int)verse[0]["length"] == (int)partmodes::switchTicks);
        // 30 folded up by an octave onto the playing keys, moved just after the switch.
        CHECK ((int)verse[1]["pitch"] == 42);
        CHECK ((int)verse[1]["start"] == (int)partmodes::switchLead);
        CHECK ((int)verse[1]["length"] == 240 - (int)partmodes::switchLead);
        CHECK ((int)verse[2]["start"] == 480);
        const auto chorus = notes (out, "chorus");
        REQUIRE (chorus.size() == 2);
        CHECK ((int)chorus[0]["pitch"] == 24);
        CHECK ((int)chorus[1]["pitch"] == 88);
        CHECK ((int)chorus[1]["start"] == (int)partmodes::switchLead);
        CHECK (notes (out, "bridge").isEmpty());
        CHECK (out["modes"]["gtr"]["mode"].toString() == "Normal");
        CHECK (out["modes"]["gtr"]["articulations"]["verse"].toString() == "palm mute");

        // Applying it again changes nothing: the switch notes are written once.
        const auto again = partmodes::apply (out, instruments);
        CHECK (juce::JSON::toString (again) == juce::JSON::toString (out));
    }

    SECTION ("An articulation named later goes back to the first section")
    {
        const auto out = partmodes::apply (song (R"({"gtr": {"mode": "Normal", "articulations": {"chorus": "open"}}})"),
                                           instruments);
        CHECK ((int)notes (out, "verse")[0]["pitch"] == 26);
        CHECK ((int)notes (out, "chorus")[0]["pitch"] == 26);
    }

    SECTION ("A mode entered by a key: that key starts every section, and the notes keep to its keys")
    {
        const auto out = partmodes::apply (song (R"({"gtr": {"mode": "Riff"}})"), instruments);
        const auto verse = notes (out, "verse");
        REQUIRE (verse.size() == 3);
        CHECK ((int)verse[0]["pitch"] == 85);
        CHECK ((int)verse[1]["pitch"] == 54);
        CHECK ((int)verse[2]["pitch"] == 64);
        CHECK ((int)notes (out, "chorus")[1]["pitch"] == 64);
    }

    SECTION ("No mode named: picked by the part's words, no switches")
    {
        const auto out = partmodes::apply (song(), instruments);
        CHECK (out["modes"]["gtr"]["mode"].toString() == "Normal");
        CHECK ((bool)out["modes"]["gtr"]["picked"]);
        const auto verse = notes (out, "verse");
        REQUIRE (verse.size() == 2);
        CHECK ((int)verse[0]["start"] == 0);
        CHECK ((int)verse[0]["pitch"] == 42);
    }

    SECTION ("What the note says")
    {
        const auto out = partmodes::apply (
            song (R"({"gtr": {"mode": "Normal", "articulations": {"verse": "palm mute", "chorus": "open"}}})"),
            instruments);
        CHECK (partmodes::describe (out, instruments, { { "gtr", "Guitar" } }) ==
               juce::StringArray { "Guitar: Normal mode; palm mute, open" });
    }
}

TEST_CASE ("Instrument profiles: the ones Spinus ships, and a profile shared as a file", "[instruments][profiles]")
{
    // Spinus's own synth and the free ones it gets come with a profile; the user's own wins, and forgetting it
    // brings the shipped one back.
    const auto root = juce::File::createTempFile ("shipped");
    const auto indexFolder = root.getChildFile ("sound-index");
    InstrumentProfiles profiles (indexFolder);
    for (const auto* key : { "stock-spinus.3x-osc", "surge-xt", "ob-xf", "odin2" })
    {
        const auto shipped = profiles.get (key);
        REQUIRE (shipped.has_value());
        REQUIRE (shipped.value().modes.size() == 1);
        CHECK (shipped.value().modes[0].keys[64].source == InstrumentProfiles::Source::bundled);
        CHECK (shipped.value().modes[0].goodFor.isNotEmpty());
        CHECK_FALSE (profiles.hasOwn (key));
        CHECK (profiles.playable (key) == std::optional<PlayableKeys> (PlayableKeys::between (0, 127)));
    }
    CHECK_FALSE (profiles.get ("plugin-1234").has_value());
    auto own = guitar();
    profiles.set ("surge-xt", own);
    CHECK (profiles.hasOwn ("surge-xt"));
    CHECK (profiles.get ("surge-xt") == std::optional<Profile> (own));
    profiles.set ("surge-xt", std::nullopt);
    CHECK (profiles.get ("surge-xt") == InstrumentProfiles::bundled ("surge-xt"));

    // Shared: facts only, the web-page manuals only; read back with every known key imported.
    own.manuals = { { "/home/me/Guitar.pdf", "1.0" }, { "https://example.org/guitar-manual", "1.0" } };
    const auto shared = InstrumentProfiles::toShared (own, "Guitar M");
    CHECK (shared["kind"].toString() == "spinus.instrument-profile");
    CHECK (shared["instrument"].toString() == "Guitar M");
    CHECK_FALSE (juce::JSON::toString (shared).contains ("/home/me"));
    juce::String name, problem;
    const auto back = InstrumentProfiles::fromShared (juce::JSON::parse (juce::JSON::toString (shared)), name, problem);
    REQUIRE (back.has_value());
    CHECK (name == "Guitar M");
    REQUIRE (back.value().modes.size() == own.modes.size());
    CHECK (back.value().modes[0].keys[24].what == "palm mute");
    CHECK (back.value().modes[0].keys[24].source == InstrumentProfiles::Source::imported);
    CHECK (back.value().modes[0].keys[10].job == InstrumentProfiles::Job::unknown);
    CHECK (back.value().manuals.size() == 1);
    CHECK (back.value().modes[1].enterKey == 85);

    CHECK_FALSE (InstrumentProfiles::fromShared (juce::JSON::parse ("{\"kind\": \"spinus.song\"}"), name, problem));
    CHECK (problem == "it is not an instrument profile Spinus exported.");
    CHECK_FALSE (InstrumentProfiles::fromShared (
        juce::JSON::parse ("{\"kind\": \"spinus.instrument-profile\", \"version\": 2}"), name, problem));
    CHECK (problem == "it was exported by a newer Spinus.");
    root.deleteRecursively();
}
