#include <catch2/catch_test_macros.hpp>

#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "model/ModelJson.h"
#include "session/InstrumentBanks.h"

using namespace spinus;
using Job = InstrumentProfiles::Job;

namespace
{
/** A sample player's own state, as one of them writes it: a version, then the .sfz file's path as a string with
    its length in front, then numbers. */
juce::MemoryBlock samplePlayerState (const juce::String& path)
{
    juce::MemoryOutputStream out;
    out.writeInt64 (0);
    const auto utf8 = path.toUTF8();
    out.writeInt ((int)utf8.sizeInBytes());
    out.write (utf8.getAddress(), utf8.sizeInBytes());
    out.writeFloat (0.0f);
    out.writeInt (64);
    out.writeInt (0);
    out.writeInt (8192);
    return out.getMemoryBlock();
}

/** The same state as a host built on JUCE keeps a VST3's: XML with the plugin's data in JUCE's base64, as binary
    XML. */
juce::MemoryBlock hostWrapped (const juce::MemoryBlock& plugin)
{
    juce::XmlElement xml ("VST3PluginState");
    xml.createNewChildElement ("IComponent")->addTextElement (plugin.toBase64Encoding());
    xml.createNewChildElement ("IEditController")->addTextElement (juce::MemoryBlock().toBase64Encoding());
    const auto text = xml.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    juce::MemoryOutputStream out;
    out.writeInt (0x21324356);
    out.writeInt ((int)text.getNumBytesAsUTF8() + 1);
    out.write (text.toRawUTF8(), text.getNumBytesAsUTF8() + 1);
    return out.getMemoryBlock();
}

juce::MemoryBlock textBlock (const juce::String& text)
{
    return juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8());
}

InstrumentProfiles::Profile playing (int low, int high, const juce::String& switchName = {}, int switchKey = -1)
{
    InstrumentProfiles::Mode mode;
    mode.setKeys (low, high, { Job::plays, {}, InstrumentProfiles::Source::probing });
    if (switchKey >= 0)
        mode.setKeys (switchKey, switchKey, { Job::switches, switchName, InstrumentProfiles::Source::probing });
    return { { mode } };
}
} // namespace

TEST_CASE ("A sample player's bank is found from the files its saved sound names", "[banks]")
{
    SECTION ("in the plugin's own data")
    {
        const auto state = samplePlayerState ("/home/me/Samples/Growlybass/Growlybass.sfz");
        CHECK (instrumentbanks::filesNamedIn (state) ==
               juce::StringArray { "/home/me/Samples/Growlybass/Growlybass.sfz" });
        const auto bank = instrumentbanks::fromState (state);
        CHECK (bank.isKnown());
        CHECK (bank.name == "Growlybass");
    }
    SECTION ("in the plugin's data inside the host's XML and base64")
    {
        const auto inner = samplePlayerState ("/home/me/Samples/Black and Green Guitars/Green Guitar.sfz");
        const auto bank = instrumentbanks::fromState (hostWrapped (inner));
        CHECK (bank.name == "Green Guitar");
        CHECK (bank == instrumentbanks::fromState (inner)); // the same bank however it is wrapped
    }
    SECTION ("a drum kit and its key map, in XML text: both tell the bank apart")
    {
        const auto kit =
            textBlock (R"(<config><value name="drumkitfile">/home/me/Kits/DRSKit/DRSKit_full.xml</value>)"
                       R"(<value name="midimapfile">/home/me/Kits/DRSKit/Midimap_full.xml</value></config>)");
        CHECK (instrumentbanks::filesNamedIn (kit) ==
               juce::StringArray { "/home/me/Kits/DRSKit/DRSKit_full.xml", "/home/me/Kits/DRSKit/Midimap_full.xml" });
        const auto other =
            textBlock (R"(<config><value name="drumkitfile">/home/me/Kits/DRSKit/DRSKit_full.xml</value>)"
                       R"(<value name="midimapfile">/home/me/Kits/DRSKit/Midimap_minimal.xml</value></config>)");
        CHECK (instrumentbanks::fromState (kit).name == "DRSKit_full");
        CHECK (instrumentbanks::fromState (kit) != instrumentbanks::fromState (other));
    }
    SECTION ("a drum kit's settings in base64 inside an LV2 host's preset, its lines broken every 76 letters")
    {
        const juce::String config =
            R"(<config version="1.0"><value name="drumkitfile">/home/me/Kits/ShittyKit/ShittyKit.xml</value>)"
            R"(<value name="midimapfile">/home/me/Kits/ShittyKit/midimap.xml</value>)"
            R"(<value name="voice_limit_max">15</value></config>)";
        auto encoded = juce::Base64::toBase64 (config);
        juce::String lines;
        for (int i = 0; i < encoded.length(); i += 76)
            lines << encoded.substring (i, i + 76) << "\n";
        const auto preset = textBlock ("<http://example.org/kit/savedState>\n\ta pset:Preset ;\n\tstate:state [\n"
                                       "\t\t<http://example.org/kit#config> \"\"\"" +
                                       lines.trimEnd() + "\"\"\"^^xsd:base64Binary\n\t] .\n");
        CHECK (instrumentbanks::filesNamedIn (preset) ==
               juce::StringArray { "/home/me/Kits/ShittyKit/ShittyKit.xml", "/home/me/Kits/ShittyKit/midimap.xml" });
        CHECK (instrumentbanks::fromState (preset).name == "ShittyKit");
    }
    SECTION ("file:// addresses and Windows paths")
    {
        CHECK (instrumentbanks::filesNamedIn (textBlock ("{\"bank\": \"file:///home/me/My%20Kits/Kit.dspreset\"}")) ==
               juce::StringArray { "/home/me/My Kits/Kit.dspreset" });
        CHECK (instrumentbanks::filesNamedIn (textBlock (R"(path=C:\Samples\Strings\Legato.nki;)")) ==
               juce::StringArray { "C:/Samples/Strings/Legato.nki" });
    }
    SECTION ("binary data around it (a sampler's audio), even cut in the middle of a letter")
    {
        juce::MemoryOutputStream out;
        juce::Random random (42);
        for (int i = 0; i < 20000; ++i)
            out.writeByte ((char)random.nextInt (256));
        const juce::String path (juce::CharPointer_UTF8 ("/home/\xd1\x8f/\xd0\x91\xd0\xb0\xd1\x81/Kit.sfz"));
        out.writeByte (0);
        out.write (path.toRawUTF8(), path.getNumBytesAsUTF8());
        out.writeByte (0);
        for (int i = 0; i < 2000; ++i)
            out.writeByte ((char)random.nextInt (256));
        out.writeByte ((char)0xe2); // the start of a three-byte letter, cut off
        CHECK (instrumentbanks::filesNamedIn (out.getMemoryBlock()).contains (path));
        CHECK (instrumentbanks::fromState (out.getMemoryBlock()).name == "Kit");
    }
    SECTION ("a state that names no file: no bank known")
    {
        CHECK (! instrumentbanks::fromState (textBlock ("<synth cutoff=\"0.5\" resonance=\"0.2\"/>")).isKnown());
        CHECK (! instrumentbanks::fromState (juce::MemoryBlock()).isKnown());
        CHECK (! instrumentbanks::fromState (textBlock ("a/b and 1/2 are not files")).isKnown());
    }
    SECTION ("a bank the user named, or a program's name")
    {
        CHECK (instrumentbanks::named ("  Strings Legato ").name == "Strings Legato");
        CHECK (! instrumentbanks::named (" ").isKnown());
        CHECK (instrumentbanks::named ("Kit") != instrumentbanks::fromProgram ("Kit"));
    }
}

TEST_CASE ("When the loaded bank decides the keys, each bank keeps its own", "[banks]")
{
    juce::TemporaryFile folder;
    const auto index = folder.getFile().getChildFile ("index");
    const juce::String player = "plugin-sampleplayer";
    const auto guitar = instrumentbanks::fromState (samplePlayerState ("/s/Guitars/Green Guitar.sfz"));
    const auto bass = instrumentbanks::fromState (samplePlayerState ("/s/Basses/Growlybass.sfz"));
    const auto unlearned = instrumentbanks::fromState (samplePlayerState ("/s/Pianos/Piano.sfz"));

    InstrumentProfiles profiles (index);
    InstrumentProfiles::Profile whole;
    whole.byBank = true;
    whole.manuals.push_back ({ "https://example.org/manual", {} });
    profiles.set (player, whole);
    auto guitarKeys = playing (40, 88, "palm mute", 24);
    guitarKeys.bankName = guitar.name;
    profiles.set (InstrumentProfiles::keyForBank (player, guitar), guitarKeys);
    auto bassKeys = playing (28, 60);
    bassKeys.bankName = bass.name;
    profiles.set (InstrumentProfiles::keyForBank (player, bass), bassKeys);
    REQUIRE (profiles.save());

    InstrumentProfiles read (index);
    read.load();
    CHECK (read.byBank (player));
    CHECK (read.playable (player, guitar) == PlayableKeys::between (40, 88));
    CHECK (read.playable (player, bass) == PlayableKeys::between (28, 60));
    // A bank not learned, or not known, has no keys: another bank's would be wrong.
    CHECK (! read.get (player, unlearned).has_value());
    CHECK (! read.get (player, {}).has_value());
    CHECK (read.get (player).value_or (InstrumentProfiles::Profile {}).manuals.size() == 1); // keeps its manuals
    CHECK (read.get (player, guitar).value_or (InstrumentProfiles::Profile {}).bankName == "Green Guitar");

    const auto banks = read.banksOf (player);
    REQUIRE (banks.size() == 2);
    CHECK (std::count_if (banks.begin(), banks.end(), [&] (const InstrumentBank& b) { return b == bass; }) == 1);

    // What agents are told: this bank's keys and its name, or that this bank isn't learned yet.
    const auto told = instrumentbanks::keysForAgents (read, player, guitar);
    CHECK (told["bank"].toString() == "Green Guitar");
    CHECK ((int)told["modes"][0]["switches"][0]["key"] == 24);
    const auto notLearned = instrumentbanks::keysForAgents (read, player, unlearned);
    CHECK (notLearned["learned"].equals (false));
    CHECK (notLearned["bank"].toString() == "Piano");
    CHECK (instrumentbanks::keysForAgents (read, player, {})["bank"].toString() == "not known");

    // Without the setting, the bank doesn't matter: the instrument's own keys, as before.
    read.set ("plugin-synth", playing (36, 96));
    CHECK (! read.byBank ("plugin-synth"));
    CHECK (read.playable ("plugin-synth", bass) == PlayableKeys::between (36, 96));
    CHECK (instrumentbanks::keysForAgents (read, "plugin-unknown", bass).isVoid());
}

TEST_CASE ("A channel's bank can be named, kept in the song and undone", "[banks]")
{
    model::Project project;
    commands::CommandHistory history (project);
    auto make = [] (const juce::String& name, const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
        INFO (error);
        REQUIRE (command != nullptr);
        return command;
    };
    auto add = make ("channel.add", R"({"name": "Strings"})");
    auto* added = add.get();
    REQUIRE (history.perform (std::move (add)).wasOk());
    const auto channel = added->getResult()["channel"].toString();

    auto name = make ("channel.set", R"({"channel": )" + channel + R"(, "bank": "  Strings Legato "})");
    CHECK (name->getDescription().contains ("Strings Legato"));
    REQUIRE (history.perform (std::move (name)).wasOk());
    CHECK (history.getProject().channels.back().bank == "Strings Legato");

    model::Project reread;
    const auto loaded = model::fromJsonText (model::toJsonText (history.getProject()), reread);
    INFO (loaded.getErrorMessage());
    REQUIRE (loaded.wasOk());
    CHECK (reread.channels.back().bank == "Strings Legato");

    REQUIRE (history.undo());
    CHECK (history.getProject().channels.back().bank.isEmpty());
}

TEST_CASE ("A learned bank's sound is kept beside the profiles, and goes when the bank is forgotten", "[banks]")
{
    const auto root = juce::File::createTempFile ("bank-sounds");
    REQUIRE (root.createDirectory().wasOk());
    const auto index = root.getChildFile ("sounds");
    const juce::String player = "plugin-sampleplayer";
    const auto bass = instrumentbanks::fromState (samplePlayerState ("/s/Basses/Growlybass.sfz"));
    const auto key = InstrumentProfiles::keyForBank (player, bass);

    InstrumentProfiles profiles (index);
    InstrumentProfiles::Profile whole;
    whole.byBank = true;
    profiles.set (player, whole);
    InstrumentProfiles::Profile keys;
    InstrumentProfiles::Mode mode;
    mode.setKeys (28, 60, { InstrumentProfiles::Job::plays, {}, InstrumentProfiles::Source::probing });
    keys.modes.push_back (mode);
    keys.bankName = bass.name;
    profiles.set (key, keys);
    REQUIRE (profiles.save());

    // Nothing kept yet; an empty or missing state is not kept.
    CHECK (! profiles.bankSoundFile (key).existsAsFile());
    CHECK (! profiles.keepBankSound (key, root.getChildFile ("none.state")));
    const auto state = root.getChildFile ("now.state");
    const auto bytes = samplePlayerState ("/s/Basses/Growlybass.sfz");
    REQUIRE (state.replaceWithData (bytes.getData(), bytes.getSize()));
    REQUIRE (profiles.keepBankSound (key, state));
    juce::MemoryBlock kept;
    REQUIRE (profiles.bankSoundFile (key).loadFileAsData (kept));
    CHECK (kept == bytes);
    CHECK (profiles.bankSoundFile (key).getParentDirectory() == index.getSiblingFile ("bank-sounds"));

    // Saving again keeps it; forgetting the bank removes it.
    REQUIRE (profiles.save());
    CHECK (profiles.bankSoundFile (key).existsAsFile());
    profiles.set (key, std::nullopt);
    REQUIRE (profiles.save());
    CHECK (! profiles.bankSoundFile (key).existsAsFile());
    root.deleteRecursively();
}
