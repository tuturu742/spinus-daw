#include "session/DrumKeys.h"
#include "session/SoundIndex.h"

#include <catch2/catch_test_macros.hpp>

using namespace spinus;

namespace
{
juce::var mapOf (std::initializer_list<std::pair<int, const char*>> entries)
{
    juce::Array<juce::var> list;
    for (const auto& [note, cls] : entries)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("note", note);
        o->setProperty ("class", juce::String (cls));
        list.add (juce::var (o));
    }
    return list;
}
} // namespace

TEST_CASE ("A kit not measured plays the drum hits on General MIDI's keys", "[analyzer][drums]")
{
    const auto keys = drumkeys::fromMap ({});
    CHECK (keys.at ("kick") == 36);
    CHECK (keys.at ("snare") == 38);
    CHECK (keys.at ("closed_hat") == 42);
    CHECK (keys.at ("open_hat") == 46);
    CHECK (keys.at ("crash") == 49);
    CHECK (keys.at ("ride") == 51);
}

TEST_CASE ("A kit on General MIDI's keys keeps them where its sounds are those drums, or like them",
           "[analyzer][drums]")
{
    // As the sound index measures one: an open hat rings like a cymbal, a low tom like a bass note.
    const auto keys = drumkeys::fromMap (mapOf ({ { 35, "kick" },
                                                  { 36, "kick" },
                                                  { 37, "snare" },
                                                  { 38, "snare" },
                                                  { 39, "snare" },
                                                  { 41, "tom" },
                                                  { 42, "hat" },
                                                  { 45, "bass" },
                                                  { 46, "cymbal" },
                                                  { 47, "tom" },
                                                  { 49, "cymbal" },
                                                  { 50, "tom" },
                                                  { 51, "cymbal" } }));
    CHECK (keys.at ("kick") == 36);
    CHECK (keys.at ("snare") == 38);
    CHECK (keys.at ("clap") == 39);
    CHECK (keys.at ("rim") == 37);
    CHECK (keys.at ("closed_hat") == 42);
    CHECK (keys.at ("open_hat") == 46);
    CHECK (keys.at ("low_tom") == 45);
    CHECK (keys.at ("mid_tom") == 47);
    CHECK (keys.at ("high_tom") == 50);
    CHECK (keys.at ("crash") == 49);
    CHECK (keys.at ("ride") == 51);
}

TEST_CASE ("A kit laid out its own way plays each drum on its first key of that drum, another for the next",
           "[analyzer][drums]")
{
    const auto keys = drumkeys::fromMap (mapOf ({ { 60, "kick" },
                                                  { 61, "snare" },
                                                  { 62, "clap" },
                                                  { 63, "hat" },
                                                  { 64, "hat" },
                                                  { 65, "tom" },
                                                  { 66, "tom" },
                                                  { 67, "tom" },
                                                  { 70, "cymbal" } }));
    CHECK (keys.at ("kick") == 60);
    CHECK (keys.at ("snare") == 61);
    CHECK (keys.at ("clap") == 62);
    CHECK (keys.at ("closed_hat") == 63);
    CHECK (keys.at ("open_hat") == 64);
    CHECK (keys.at ("low_tom") == 65);
    CHECK (keys.at ("mid_tom") == 66);
    CHECK (keys.at ("high_tom") == 67);
    CHECK (keys.at ("crash") == 70);
    CHECK (keys.at ("ride") == 70); // no other cymbal: the crash's again
}

TEST_CASE ("The Drum Machine's pads name the drums the hits are, a second sound's pad its drum", "[analyzer][drums]")
{
    CHECK (drumkeys::drumOfPad (60) == "kick");
    CHECK (drumkeys::drumOfPad (64) == "open_hat");
    CHECK (drumkeys::drumOfPad (74) == "ride");
    CHECK (drumkeys::drumOfPad (71).isEmpty());
    CHECK (drumkeys::drumOfPad (71, { { 71, "snare" } }) == "snare");
}

TEST_CASE ("The sound index's kits of an instrument are its sounds with a kick, a snare and a hat on its keys",
           "[analyzer][drums]")
{
    const auto folder = juce::File::createTempFile ("drum-keys");
    folder.createDirectory();
    auto sound = [] (const char* id, const juce::var& map)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", juce::String (id));
        o->setProperty ("name", juce::String (id) + " kit");
        o->setProperty ("drumMap", map);
        return juce::var (o);
    };
    auto* file = new juce::DynamicObject();
    file->setProperty ("version", SoundIndex::version);
    file->setProperty ("key", "plugin-kit");
    file->setProperty (
        "sounds", juce::Array<juce::var> { sound ("rock", mapOf ({ { 36, "kick" }, { 38, "snare" }, { 42, "hat" } })),
                                           sound ("toms", mapOf ({ { 41, "tom" }, { 43, "tom" } })) });
    const auto target = SoundIndex (folder).instrumentFile ("plugin-kit");
    target.getParentDirectory().createDirectory();
    REQUIRE (target.replaceWithText (juce::JSON::toString (juce::var (file))));

    const auto kits = drumkeys::measuredKits (folder, "plugin-kit");
    REQUIRE (kits.size() == 1);
    CHECK (kits[0]["id"].toString() == "rock");
    CHECK (drumkeys::measuredKits (folder, "plugin-none").isEmpty());
    folder.deleteRecursively();
}
