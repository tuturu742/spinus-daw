#include <catch2/catch_test_macros.hpp>

#include "ProgramsTestPlugin.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "plugins/DeviceList.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/InstrumentPresets.h"
#include "session/PresetPreviews.h"
#include "session/SynthMatch.h"

#include <cmath>

using namespace spinus;

namespace
{
float peakOf (const juce::File& wav)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (wav.createInputStream().release(), true));
    if (reader == nullptr)
        return -1.0f;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio.getMagnitude (0, audio.getNumSamples());
}

/** How bright a recording is: the energy of its sample-to-sample change against its own (high
    frequencies count most). */
double brightnessOf (const juce::File& wav)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (wav.createInputStream().release(), true));
    if (reader == nullptr)
        return -1.0;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    double change = 0.0, level = 0.0;
    for (int i = 1; i < audio.getNumSamples(); ++i)
    {
        change += juce::square ((double)audio.getSample (0, i) - audio.getSample (0, i - 1));
        level += juce::square ((double)audio.getSample (0, i));
    }
    return level > 0.0 ? change / level : 0.0;
}

struct TempFolder
{
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-instrument-presets-" + juce::Uuid().toString());
};

/** The two test CLAP instruments, installed (tests/plugins/TestSynthClap.c). */
std::map<juce::String, model::Device> installTestSynths (tracktion::Engine& engine)
{
    std::map<juce::String, model::Device> devices;
    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (
        found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName());
    for (auto* d : found)
    {
        engine.getPluginManager().knownPluginList.addType (*d);
        devices[d->name] = plugins::DeviceList::makeDevice (*d);
    }
    return devices;
}

/** An OB-Xf patch file: a VST program file whose chunk is the patch as JUCE binary XML. */
juce::MemoryBlock makeObxfPatch (const juce::String& name, float cutoff)
{
    juce::XmlElement patch ("OB-Xf");
    patch.setAttribute ("ob-xf_version", "1.0.3");
    patch.setAttribute ("Cutoff", cutoff);
    patch.setAttribute ("voiceCount", 32);
    patch.setAttribute ("programName", name);
    juce::MemoryBlock chunk;
    juce::AudioProcessor::copyXmlToBinary (patch, chunk);

    juce::MemoryOutputStream out;
    const auto bigEndian = [&out] (int value) { out.writeIntBigEndian (value); };
    out.write ("CcnK", 4);
    bigEndian ((int)chunk.getSize() + 52);
    out.write ("FPCh", 4);
    bigEndian (1);          // format version
    bigEndian (0x4f425866); // plugin ID
    bigEndian (1);          // plugin version
    bigEndian (1);          // programs
    char programName[28] = {};
    name.copyToUTF8 (programName, sizeof (programName));
    out.write (programName, sizeof (programName));
    bigEndian ((int)chunk.getSize());
    out.write (chunk.getData(), chunk.getSize());
    return out.getMemoryBlock();
}
} // namespace

TEST_CASE ("Instrument presets: numbered slots and placeholders are not presets", "[session][presets]")
{
    for (auto* name : { "", "  ", "Default", "INIT", "Init Patch", "init_patch", "Program 3", "Preset 12", "Patch-4",
                        "Init 2", "007", "Untitled", "-" })
    {
        INFO (name);
        CHECK (InstrumentPresets::isPlaceholderName (name));
    }
    for (auto* name : { "Warm Pad", "Bass 3", "Program Change Lead", "Initial D", "Default Strings", "808" })
    {
        INFO (name);
        CHECK ((InstrumentPresets::isPlaceholderName (name) == (juce::String (name) == "808")));
    }
}

TEST_CASE ("Instrument presets: a plugin's programs with real names", "[session][presets]")
{
    test::ProgramsTestPlugin plugin;
    const auto found = InstrumentPresets::findPrograms (plugin, "1.0");
    juce::StringArray names;
    for (auto& p : found)
        names.add (p.name);
    CHECK (names == juce::StringArray { "Warm Sine", "Bright Saw", "Hollow Square", "Dull Saw" });
    REQUIRE (found.size() == 4);
    CHECK (found[3].program == 5);
    CHECK (found[3].kind == InstrumentPreset::Kind::program);
    CHECK (found[3].id == "program:5:Dull Saw");
    CHECK (found[3].version == "1.0");
    REQUIRE (InstrumentPresets::load (plugin, found[1]).wasOk());
    CHECK (plugin.getCurrentProgram() == 2);

    // One real program is no choice.
    struct One : test::ProgramsTestPlugin
    {
        int getNumPrograms() override { return 2; }
        const juce::String getProgramName (int i) override { return i == 0 ? "Default" : "Only Sound"; }
    } one;
    CHECK (InstrumentPresets::findPrograms (one).empty());
}

TEST_CASE ("Instrument presets: OB-Xf's patch files become its state", "[session][presets]")
{
    const auto state = InstrumentPresets::obxfStateFromPatch (makeObxfPatch ("Fat Bass", 0.25f));
    REQUIRE (! state.isEmpty());
    const auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int)state.getSize());
    REQUIRE (xml != nullptr);
    CHECK (xml->hasTagName ("OB-Xf"));
    CHECK (xml->getStringAttribute ("ob-xf_version") == "1.0.3");
    CHECK (xml->getIntAttribute ("single-program-format") == 1);
    const auto* program = xml->getChildByName ("program");
    REQUIRE (program != nullptr);
    CHECK (program->getStringAttribute ("programName") == "Fat Bass");
    CHECK (std::abs (program->getDoubleAttribute ("Cutoff") - 0.25) < 1.0e-6);
    CHECK (! program->hasAttribute ("ob-xf_version"));

    // Anything else is not a patch.
    CHECK (InstrumentPresets::obxfStateFromPatch (juce::MemoryBlock ("CcnK", 4)).isEmpty());
    auto broken = makeObxfPatch ("Broken", 0.5f);
    static_cast<char*> (broken.getData())[9] = 'X'; // not "FPCh"
    CHECK (InstrumentPresets::obxfStateFromPatch (broken).isEmpty());

    // The patches in a folder (and its folders), by category and name.
    TempFolder t;
    for (auto* sub : { "Basses", "Leads" })
        REQUIRE (t.root.getChildFile (sub).createDirectory());
    for (auto [path, cutoff] : { std::pair { "Basses/Fat Bass.fxp", 0.2f }, std::pair { "Leads/Saw Lead.fxp", 0.8f },
                                 std::pair { "Basses/Acid.fxp", 0.5f } })
        REQUIRE (t.root.getChildFile (path).replaceWithData (makeObxfPatch (path, cutoff).getData(),
                                                             makeObxfPatch (path, cutoff).getSize()));
    REQUIRE (t.root.getChildFile ("Leads/readme.txt").replaceWithText ("not a patch"));
    REQUIRE (t.root.getChildFile ("Leads/Empty.fxp").replaceWithText ("CcnK"));
    const auto found = InstrumentPresets::findObxfPatches ({ t.root });
    REQUIRE (found.size() == 3);
    CHECK (found[0].name == "Acid");
    CHECK (found[0].category == "Basses");
    CHECK (found[2].name == "Saw Lead");
    CHECK (found[2].kind == InstrumentPreset::Kind::state);
    CHECK (found[2].id == t.root.getChildFile ("Leads/Saw Lead.fxp").getFullPathName());
    // The downloaded patches beside the plugin come first.
    const auto plugin = t.root.getChildFile ("ob-xf/OB-Xf.clap");
    REQUIRE (plugin.getSiblingFile ("Patches").createDirectory());
    CHECK (InstrumentPresets::getObxfFolders (plugin).front() == plugin.getSiblingFile ("Patches"));
}

TEST_CASE ("Instrument presets: a CLAP plugin's own, through preset discovery", "[session][presets]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    const auto devices = installTestSynths (host.getEngine());
    REQUIRE (devices.contains ("Spinus Test Synth"));
    const auto& synth = devices.at ("Spinus Test Synth");

    PresetPreviews previews (session, synth);
    REQUIRE (previews.getProblem().isEmpty());
    auto& instance = *previews.getInstance();
    const auto found = InstrumentPresets::find (instance, synth);
    juce::StringArray names;
    for (auto& p : found)
        names.add (p.name);
    CHECK (names == juce::StringArray { "Bright Saw", "Dark Square", "Soft Sine Pad", "Square Pluck", "Resonant Saw",
                                        "Sine Pluck" });
    REQUIRE (found.size() == 6);
    CHECK (found[1].kind == InstrumentPreset::Kind::discovered);
    CHECK (found[1].category == "Factory");
    CHECK (found[1].id == "plugin|1");
    CHECK (InstrumentPresets::libraryOf (synth).startsWith ("plugin-"));
    CHECK (InstrumentPresets::libraryOf (synth) != InstrumentPresets::libraryOf (devices.at ("Spinus Test Organ")));

    // Loaded: the plugin's settings are the preset's.
    REQUIRE (InstrumentPresets::load (instance, found[1]).wasOk());
    for (auto* parameter : instance.getParameters())
        if (parameter->getName (100) == "Cutoff")
            CHECK (std::abs (parameter->getValue() - 0.3f) < 1.0e-3f); // Dark Square's (from 0 to 1)

    // Each preset plays the phrase in its own way.
    TempFolder t;
    const auto bright = t.root.getChildFile ("bright.wav"), dark = t.root.getChildFile ("dark.wav");
    REQUIRE (previews.renderOne (found[0], bright).wasOk());
    REQUIRE (previews.renderOne (found[1], dark).wasOk());
    CHECK (peakOf (bright) > 0.01f);
    CHECK (brightnessOf (bright) > 1.5 * brightnessOf (dark));

    // The organ has none.
    PresetPreviews organ (session, devices.at ("Spinus Test Organ"));
    REQUIRE (organ.getProblem().isEmpty());
    CHECK (InstrumentPresets::find (*organ.getInstance(), devices.at ("Spinus Test Organ")).empty());
    CHECK (plugins::ClapPluginFormat::discoverPresets (*organ.getInstance()).empty());
}

TEST_CASE ("Instrument presets: an installed plugin's programs, played by a hidden instrument", "[session][presets]")
{
    EngineHost host { { .openAudioDevice = false } };
    test::ProgramsTestPlugin::install (host.getEngine());
    Session session (host.getEngine());
    const auto device = plugins::DeviceList::makeDevice (test::ProgramsTestPlugin::describe());

    PresetPreviews previews (session, device);
    REQUIRE (previews.getProblem().isEmpty());
    const auto found = InstrumentPresets::find (*previews.getInstance(), device);
    REQUIRE (found.size() == 4);
    CHECK (found[0].version.startsWith ("1.0:"));
    TempFolder t;
    const auto bright = t.root.getChildFile ("bright.wav"), dull = t.root.getChildFile ("dull.wav");
    REQUIRE (previews.renderOne (found[1], bright).wasOk()); // Bright Saw
    REQUIRE (previews.renderOne (found[3], dull).wasOk());   // Dull Saw
    CHECK (peakOf (bright) > 0.01f);
    CHECK (brightnessOf (bright) > 1.5 * brightnessOf (dull));
    CHECK (previews.getInstance()->getCurrentProgram() == 5);
}

/** OB-Xf's patches and Odin 2's built-in presets, with the downloaded plugins: Spinus finds them, each plays
    in its own way, and a preset given to the plugin is in its saved state. Point SPINUS_CHECK_DOWNLOADED at
    a folder holding the unpacked CLAP files (searched recursively) with OB-Xf's Patches folder beside
    OB-Xf.clap, as Spinus's download unpacks them. */
TEST_CASE ("Instrument presets: OB-Xf and Odin 2, downloaded", "[session][presets][downloaded]")
{
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (! folder.isDirectory())
        SKIP ("SPINUS_CHECK_DOWNLOADED is not set to a folder");

    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    std::vector<juce::File> folders { folder };
    for (auto* file : { "Odin2.clap", "OB-Xf.clap" })
        for (const auto& found : folder.findChildFiles (juce::File::findFiles, true, file))
            folders.push_back (found.getParentDirectory());
    engine::registerBundledDevices (*devices, host.getEngine(), folders);
    Session session { host.getEngine(), std::move (devices) };
    TempFolder t;

    for (auto* stockType : { "bundled.ob-xf", "bundled.odin2" })
    {
        INFO (stockType);
        model::Device device;
        device.kind = model::Device::Kind::stock;
        device.stockType = stockType;
        PresetPreviews previews (session, device);
        if (previews.getProblem().isNotEmpty())
        {
            WARN (stockType << ": not in " << folder.getFullPathName());
            continue;
        }
        auto& instance = *previews.getInstance();
        // Neither loads presets for a host: Spinus gives them as the plugin's state.
        CHECK (! plugins::ClapPluginFormat::canLoadPresets (instance));
        auto started = juce::Time::getMillisecondCounterHiRes();
        const auto found = InstrumentPresets::find (instance, device);
        WARN (stockType << ": " << found.size() << " presets, found in "
                        << (juce::Time::getMillisecondCounterHiRes() - started) << " ms");
        REQUIRE (found.size() > 200);
        CHECK (InstrumentPresets::libraryOf (device) ==
               (juce::String (stockType) == "bundled.ob-xf" ? "ob-xf" : "odin2"));

        // The plugin saves the state Spinus gives it as its own (CLAP state is the plugin's own data).
        juce::MemoryBlock saved;
        instance.getStateInformation (saved);
        CHECK (juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int)saved.getSize()) != nullptr);

        // Twenty presets, each recorded: they sound, and differently.
        std::vector<float> peaks;
        std::vector<double> brightness;
        started = juce::Time::getMillisecondCounterHiRes();
        for (size_t i = 0; i < 20; ++i)
        {
            const auto& preset = found[i * found.size() / 20];
            const auto wav = t.root.getChildFile (juce::String (stockType) + juce::String ((int)i) + ".wav");
            INFO (preset.name);
            REQUIRE (previews.renderOne (preset, wav).wasOk());
            peaks.push_back (peakOf (wav));
            brightness.push_back (brightnessOf (wav));
        }
        WARN (stockType << ": 20 previews in " << (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0
                        << " s");
        CHECK (std::count_if (peaks.begin(), peaks.end(), [] (float p) { return p > 0.01f; }) >= 16);
        std::sort (brightness.begin(), brightness.end());
        CHECK (brightness.back() > 1.5 * brightness[2]);

        // The preset is the plugin's own now: its saved state names it.
        const auto& named = found[found.size() / 2];
        REQUIRE (InstrumentPresets::load (instance, named).wasOk());
        instance.getStateInformation (saved);
        const auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int)saved.getSize());
        REQUIRE (xml != nullptr);
        INFO (named.name);
        CHECK (xml->toString().contains (named.name.replace ("&", "&amp;")));
    }
}

/** For checking closest preset by hand with OB-Xf and Odin 2 (as the Surge XT check in PresetPreviewsTests):
    SPINUS_PRESET_CHECK_FOLDER gets <library>/previews/<n>.wav of every preset (and index.txt), and
    <library>/queries/<n>.wav of every tenth one playing a different melody with its filter a little
    brighter, as a stand-in for a stem. Needs SPINUS_CHECK_DOWNLOADED as above. */
TEST_CASE ("Write OB-Xf and Odin 2 preset previews and queries (for checking closest preset)", "[.presetcheck]")
{
    const juce::File out (juce::SystemStats::getEnvironmentVariable ("SPINUS_PRESET_CHECK_FOLDER", {}));
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (out == juce::File() || ! folder.isDirectory())
        SKIP ("set SPINUS_PRESET_CHECK_FOLDER and SPINUS_CHECK_DOWNLOADED");
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    std::vector<juce::File> folders { folder };
    for (auto* file : { "Odin2.clap", "OB-Xf.clap" })
        for (const auto& found : folder.findChildFiles (juce::File::findFiles, true, file))
            folders.push_back (found.getParentDirectory());
    engine::registerBundledDevices (*devices, host.getEngine(), folders);
    Session session { host.getEngine(), std::move (devices) };
    const std::vector<PresetPreviews::PhraseNote> melody { { 43, 0.0, 0.4 },  { 46, 0.5, 0.4 },   { 50, 1.0, 0.9 },
                                                           { 55, 2.0, 0.25 }, { 53, 2.25, 0.25 }, { 50, 2.5, 1.0 } };
    for (auto [stockType, cutoffName] :
         { std::pair { "bundled.ob-xf", "Filter Cutoff" }, std::pair { "bundled.odin2", "Filter1 Frequency" } })
    {
        model::Device device;
        device.kind = model::Device::Kind::stock;
        device.stockType = stockType;
        PresetPreviews previews (session, device);
        REQUIRE (previews.getProblem().isEmpty());
        auto& instance = *previews.getInstance();
        const auto presets = InstrumentPresets::find (instance, device);
        const auto library = out.getChildFile (InstrumentPresets::libraryOf (device));
        juce::String index;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        for (size_t n = 0; n < presets.size(); ++n)
        {
            const auto name = juce::String ((int)n);
            REQUIRE (previews.renderOne (presets[n], library.getChildFile ("previews/" + name + ".wav")).wasOk());
            index << name << "\t" << presets[n].category << "/" << presets[n].name << "\n";
        }
        WARN (stockType << ": " << presets.size() << " previews in "
                        << (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0 << " s");
        for (size_t n = 0; n < presets.size(); n += 10)
        {
            REQUIRE (InstrumentPresets::load (instance, presets[n]).wasOk());
            for (auto* parameter : instance.getParameters())
                if (parameter->getName (100) == cutoffName)
                    parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, parameter->getValue() + 0.08f));
            // The preset is in place, its filter changed: played as it is.
            REQUIRE (previews.renderAsIs (library.getChildFile ("queries/" + juce::String ((int)n) + ".wav"), melody)
                         .wasOk());
        }
        library.getChildFile ("index.txt").replaceWithText (index);
    }
}

/** For deciding whether a synth is tuned from its closest preset: SPINUS_TUNE_CHECK names a text file of
    "<stock type>\t<target WAV, 48 kHz, playing the check melody above>\t<the synth's closest preset, by its
    place in InstrumentPresets::find>" lines; each target is tuned to from the synth's default sound and
    from that preset, and both distances are said. Needs SPINUS_CHECK_DOWNLOADED as above. */
TEST_CASE ("Tune from the default sound or from the closest preset (for deciding a profile's start)", "[.tunecheck]")
{
    const juce::File list (juce::SystemStats::getEnvironmentVariable ("SPINUS_TUNE_CHECK", {}));
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (! list.existsAsFile() || ! folder.isDirectory())
        SKIP ("set SPINUS_TUNE_CHECK and SPINUS_CHECK_DOWNLOADED");
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    std::vector<juce::File> folders { folder };
    for (auto* file : { "Odin2.clap", "OB-Xf.clap" })
        for (const auto& found : folder.findChildFiles (juce::File::findFiles, true, file))
            folders.push_back (found.getParentDirectory());
    engine::registerBundledDevices (*devices, host.getEngine(), folders);
    Session session { host.getEngine(), std::move (devices) };
    const std::vector<HiddenInstrument::Note> melody {
        { 43, 0.0, 0.4, 100.0f / 127.0f },  { 46, 0.5, 0.4, 100.0f / 127.0f },   { 50, 1.0, 0.9, 100.0f / 127.0f },
        { 55, 2.0, 0.25, 100.0f / 127.0f }, { 53, 2.25, 0.25, 100.0f / 127.0f }, { 50, 2.5, 1.0, 100.0f / 127.0f }
    };
    double fromDefault = 0.0, fromPreset = 0.0;
    int count = 0;
    for (const auto& line : juce::StringArray::fromLines (list.loadFileAsString()))
    {
        const auto parts = juce::StringArray::fromTokens (line, "\t", "");
        if (parts.size() < 3)
            continue;
        model::Device device;
        device.kind = model::Device::Kind::stock;
        device.stockType = parts[0];
        std::unique_ptr<juce::AudioFormatReader> reader (
            juce::WavAudioFormat().createReaderFor (juce::File (parts[1]).createInputStream().release(), true));
        REQUIRE (reader != nullptr);
        juce::AudioBuffer<float> target (1, (int)reader->lengthInSamples);
        reader->read (&target, 0, target.getNumSamples(), 0, true, false);
        const auto profile = SynthProfile::find (device);
        REQUIRE (profile.has_value());
        std::vector<double> distances;
        for (bool startFromPreset : { false, true })
        {
            SynthMatcher matcher (session, device, profile);
            REQUIRE (matcher.getProblem().isEmpty());
            if (startFromPreset)
            {
                PresetPreviews finder (session, device);
                const auto presets = InstrumentPresets::find (*finder.getInstance(), device);
                REQUIRE (matcher.loadPreset (presets[(size_t)parts[2].getIntValue()]).wasOk());
            }
            bool done = false;
            matcher.start (target, melody, profile.value_or (SynthProfile {}).getTries(), {}, [&] { done = true; });
            for (int i = 0; i < 12000 && ! done; ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            REQUIRE (done);
            const auto& result = matcher.getResult();
            WARN (parts[0] << " " << juce::File (parts[1]).getFileName()
                           << (startFromPreset ? " from preset " : " from default ") << parts[2] << ": "
                           << result.startDistance << " -> " << result.distance);
            distances.push_back (result.distance);
        }
        fromDefault += distances[0];
        fromPreset += distances[1];
        ++count;
    }
    WARN ("average over " << count << ": from the default sound " << fromDefault / juce::jmax (1, count)
                          << ", from the closest preset " << fromPreset / juce::jmax (1, count));
}
