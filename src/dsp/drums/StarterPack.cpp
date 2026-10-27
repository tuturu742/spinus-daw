#include "StarterPack.h"

#include "../sampler/SampleStorage.h"

namespace spinus::dsp::starter
{

namespace
{
bool isPack (const juce::File& folder) { return folder.getChildFile (kitsFileName).existsAsFile(); }
} // namespace

juce::File findPack (const juce::File& programFolder)
{
    if (const auto beside = programFolder.getChildFile ("samples").getChildFile (packName); isPack (beside))
        return beside;

    auto folder = programFolder;
    for (int up = 0; up < 6 && folder.getParentDirectory() != folder; ++up)
    {
        folder = folder.getParentDirectory();
        if (const auto shared = folder.getChildFile ("share/spinus/samples").getChildFile (packName); isPack (shared))
            return shared;
    }

#if defined(SPINUS_SOURCE_DIR)
    if (const auto source = juce::File (SPINUS_SOURCE_DIR).getChildFile ("samples").getChildFile (packName);
        isPack (source))
        return source;
#endif
    return {};
}

juce::File findPack()
{
    // Spinus's program, or a plugin's library inside another program (JUCE finds the module its code is in).
    auto program = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
#if JUCE_LINUX || JUCE_BSD
    if (const juce::File self ("/proc/self/exe"); self.isSymbolicLink())
        if (const auto target = self.getLinkedTarget();
            target.existsAsFile() && target.getFileNameWithoutExtension() == program.getFileNameWithoutExtension())
            program = target;
#endif
    if (const auto found = findPack (program.getParentDirectory()); found != juce::File())
        return found;
    for (int i = 0; i < 8 && program.isSymbolicLink(); ++i)
        program = program.getLinkedTarget();
    return findPack (program.getParentDirectory());
}

std::vector<Kit> readKits (const juce::File& pack)
{
    std::vector<Kit> kits;
    const auto json = juce::JSON::parse (pack.getChildFile (kitsFileName).loadFileAsString());
    if (const auto* list = json["kits"].getArray())
        for (const auto& k : *list)
        {
            Kit kit;
            kit.name = k["name"].toString();
            kit.description = k["description"].toString();
            if (const auto* pads = k["pads"].getArray())
                for (const auto& p : *pads)
                {
                    if (kit.pads.size() >= 16)
                        break;
                    KitPad pad;
                    pad.name = p["name"].toString();
                    pad.file = p["file"].toString();
                    pad.chokeGroup = juce::jlimit (0, 8, (int)p.getProperty ("choke", 0));
                    pad.tuneSemitones = (float)juce::jlimit (-24.0, 24.0, (double)p.getProperty ("tune", 0.0));
                    pad.volumeDb = (float)juce::jlimit (-48.0, 12.0, (double)p.getProperty ("volume", 0.0));
                    kit.pads.push_back (std::move (pad));
                }
            if (kit.name.isNotEmpty())
                kits.push_back (std::move (kit));
        }
    return kits;
}

juce::File padFile (const juce::File& pack, const KitPad& pad)
{
    // Only files inside the pack: "../" in kits.json would reach out of it.
    if (pad.file.isEmpty() || pad.file.contains ("..") || juce::File::isAbsolutePath (pad.file))
        return {};
    return pack.getChildFile (pad.file.replaceCharacter ('\\', '/'));
}

PackCheck checkPack (const juce::File& pack)
{
    PackCheck result;
    for (const auto& entry : juce::RangedDirectoryIterator (pack, true, "*.flac", juce::File::findFiles))
    {
        ++result.sounds;
        if (std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (entry.getFile()));
            reader == nullptr || reader->lengthInSamples <= 0)
            result.problems.add (entry.getFile().getRelativePathFrom (pack) + " does not read");
    }
    if (result.sounds == 0)
        result.problems.add ("no sounds");

    const auto kits = readKits (pack);
    result.kits = (int)kits.size();
    if (kits.empty())
        result.problems.add (juce::String ("no kits in ") + kitsFileName);
    for (const auto& kit : kits)
    {
        if (kit.pads.size() != 16)
            result.problems.add ("the kit " + kit.name + " has " + juce::String ((int)kit.pads.size()) + " pads");
        for (const auto& pad : kit.pads)
            if (! padFile (pack, pad).existsAsFile())
                result.problems.add ("the kit " + kit.name + "'s " + pad.name + " (" + pad.file + ") is missing");
    }
    return result;
}

} // namespace spinus::dsp::starter
