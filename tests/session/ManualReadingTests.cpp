#include <catch2/catch_test_macros.hpp>

#include "session/ManualReading.h"

using namespace spinus;
using Job = InstrumentProfiles::Job;
using Source = InstrumentProfiles::Source;

namespace
{
juce::var parse (const char* json) { return juce::JSON::parse (juce::String (json)); }

/** What the AI service answers for a sampled guitar's two manuals (keys as MIDI notes). */
juce::var guitarAnswer (bool middleCSaid = true)
{
    auto answer = parse (R"({
        "middleC": "C3",
        "modes": [
            {"name": "Normal", "enter": "", "enterKey": -1, "goodFor": "picked lines", "voices": "one",
             "zones": [{"from": 40, "to": 84, "job": "plays", "what": ""},
                       {"from": 24, "to": 26, "job": "switches", "what": "palm mute"}]},
            {"name": "Strummer", "enter": "the C#6 key", "enterKey": 97, "goodFor": "strummed rhythm",
             "voices": "chords",
             "zones": [{"from": 60, "to": 71, "job": "special", "what": "chord keys"}]}
        ],
        "notes": "Chord keys pick the chord.",
        "read": [{"source": "/m/Guitar.pdf", "title": "", "pages": 30, "chars": 9000},
                 {"source": "/m/Gone.pdf", "problem": "there is no file at /m/Gone.pdf."}],
        "by": "claude-test"
    })");
    answer.getDynamicObject()->setProperty ("middleCSaid", middleCSaid);
    return answer;
}
} // namespace

TEST_CASE ("Manual reading: the AI's answer as a proposal", "[instruments][manuals]")
{
    const auto found = manualreading::fromVar (guitarAnswer());
    REQUIRE (found.has_value());
    const auto& reading = found.value();
    REQUIRE (reading.modes.size() == 2);
    CHECK (reading.modes[0].keys[40].job == Job::plays);
    CHECK (reading.modes[0].keys[40].source == Source::manual);
    CHECK (reading.modes[0].keys[25].what == "palm mute");
    CHECK (reading.modes[0].keys[30].job == Job::unknown);
    CHECK (reading.modes[1].enterKey == 97);
    CHECK (reading.modes[1].voices == InstrumentProfiles::Voices::chords);
    CHECK (reading.problems == juce::StringArray { "Gone.pdf: there is no file at /m/Gone.pdf." });
    CHECK (manualreading::describe (reading) == "2 modes: Normal, Strummer (entered by C#8, MIDI 97)");
    CHECK_FALSE (manualreading::fromVar (parse (R"({"modes": []})")).has_value());
}

TEST_CASE ("Manual reading: Apply adds modes, fills what is missing and keeps what the user set",
           "[instruments][manuals]")
{
    InstrumentProfiles::Profile profile { { InstrumentProfiles::Mode {} } };
    profile.modes[0].setKeys (52, 52, { Job::special, "slide", Source::user });
    profile.modes[0].goodFor = "my words";
    const auto found = manualreading::fromVar (guitarAnswer());
    REQUIRE (found.has_value());
    const auto& reading = found.value();
    CHECK (manualreading::matchingMode (profile, reading.modes[0]) == 0);
    CHECK (manualreading::matchingMode (profile, reading.modes[1]) == -1);

    const auto changes = manualreading::apply (profile, reading);
    CHECK (changes > 40);
    REQUIRE (profile.modes.size() == 2);
    const auto& normal = profile.modes[0];
    CHECK (normal.name == "Normal");
    CHECK (normal.goodFor == "my words"); // the user's
    CHECK (normal.voices == InstrumentProfiles::Voices::one);
    CHECK (normal.keys[52].job == Job::special); // the user's
    CHECK (normal.keys[52].source == Source::user);
    CHECK (normal.keys[60].job == Job::plays);
    CHECK (normal.keys[24].job == Job::switches);
    CHECK (profile.modes[1].name == "Strummer");
    CHECK (profile.modes[1].keys[65].what == "chord keys");
    // Applied again: nothing new.
    CHECK (manualreading::apply (profile, reading) == 0);
    CHECK (profile.modes.size() == 2);
}

TEST_CASE ("Manual reading: the first mode takes the manual's name while it is still Normal", "[instruments][manuals]")
{
    InstrumentProfiles::Profile profile { { InstrumentProfiles::Mode {} } };
    auto reading = manualreading::fromVar (
                       parse (R"({"modes": [{"name": "Sustain", "zones": [{"from": 30, "to": 60, "job": "plays"}]}]})"))
                       .value();
    CHECK (manualreading::apply (profile, reading) > 0);
    CHECK (profile.modes.size() == 1);
    CHECK (profile.modes[0].name == "Sustain");
    profile.modes[0].name = "Mine";
    reading.modes[0].name = "Other";
    manualreading::apply (profile, reading);
    CHECK (profile.modes[0].name == "Mine");
}

TEST_CASE ("Manual reading: keys moved by octaves to fit what was heard when the manual doesn't say its middle C",
           "[instruments][manuals]")
{
    // Heard: E3 to E6 in Spinus's names (MIDI 52 to 88) play, 36 to 38 switch.
    InstrumentProfiles::Profile heard { { InstrumentProfiles::Mode {} } };
    heard.modes[0].setKeys (52, 88, { Job::plays, {}, Source::probing });
    heard.modes[0].setKeys (36, 38, { Job::switches, {}, Source::probing });
    heard.modes[0].setKeys (0, 35, { Job::unused, {}, Source::probing });
    heard.modes[0].setKeys (89, 127, { Job::unused, {}, Source::probing });

    // The manual (read with C3 as middle C) says 40 to 76 play and 24 to 26 switch: an octave low.
    auto said = parse (R"({"modes": [{"name": "Normal", "zones": [{"from": 40, "to": 76, "job": "plays"},
                                                                 {"from": 24, "to": 26, "job": "switches"}]}]})");
    said.getDynamicObject()->setProperty ("middleCSaid", false);
    auto reading = manualreading::fromVar (said).value();
    CHECK (manualreading::fitOctaves (reading, heard) == 1);
    CHECK (reading.modes[0].keys[52].job == Job::plays);
    CHECK (reading.modes[0].keys[36].job == Job::switches);
    CHECK (reading.modes[0].keys[40].job == Job::unknown);

    // A manual that says its middle C is not moved; nothing heard to compare with: not moved either.
    auto told = manualreading::fromVar (guitarAnswer (true)).value();
    CHECK (manualreading::fitOctaves (told, heard) == 0);
    auto unheard = manualreading::fromVar (said).value();
    CHECK (manualreading::fitOctaves (unheard, InstrumentProfiles::Profile { { InstrumentProfiles::Mode {} } }) == 0);
}

TEST_CASE ("Manual reading: a profile keeps its manuals and the version each was read for", "[instruments][manuals]")
{
    InstrumentProfiles::Profile profile { { InstrumentProfiles::Mode {} } };
    profile.manuals = { { "/home/me/Guitar_Strummer.pdf", "1.0.2" }, { "https://example.org/manual", {} } };
    CHECK_FALSE (profile.manuals[0].isWebPage());
    CHECK (profile.manuals[1].isWebPage());
    const auto back = InstrumentProfiles::fromVar (InstrumentProfiles::toVar (profile));
    REQUIRE (back.has_value());
    CHECK (back.value() == profile);
}
