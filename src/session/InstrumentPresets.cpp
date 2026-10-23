#include "InstrumentPresets.h"

#include "plugins/SurgePresets.h"
#include "session/ProgramFolder.h"

#include <cstring>
#include <set>

namespace spinus
{

namespace
{
enum class Family
{
    surge,
    obxf,
    odin,
    other
};

/** Which plugin it is: by stock type when downloaded through Spinus, by name for the same plugin installed
    as CLAP (the way Spinus loads its presets depends on CLAP). */
Family familyOf (const model::Device& device)
{
    if (device.kind == model::Device::Kind::stock)
        return device.stockType == "bundled.surge-xt" ? Family::surge
               : device.stockType == "bundled.ob-xf"  ? Family::obxf
               : device.stockType == "bundled.odin2"  ? Family::odin
                                                      : Family::other;
    if (device.plugin.format != plugins::ClapPluginFormat::formatName)
        return Family::other;
    const auto& name = device.plugin.name;
    if (name.equalsIgnoreCase ("Surge XT"))
        return Family::surge;
    if (name.equalsIgnoreCase ("OB-Xf"))
        return Family::obxf;
    if (name.equalsIgnoreCase ("Odin2") || name.equalsIgnoreCase ("Odin 2"))
        return Family::odin;
    return Family::other;
}

constexpr juce::int64 megabyte = (juce::int64)1024 * 1024;

juce::String fileVersion (const juce::File& file)
{
    return juce::String (file.getSize()) + ":" + juce::String (file.getLastModificationTime().toMilliseconds());
}

/** FNV-1a: the same on every computer and Spinus version (the index outlives both). */
juce::String hashOf (const void* data, size_t size)
{
    juce::uint64 hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<const juce::uint8*> (data)[i];
        hash *= 1099511628211ull;
    }
    return juce::String::toHexString ((juce::int64)hash);
}

juce::String hashOf (const juce::String& text) { return hashOf (text.toRawUTF8(), text.getNumBytesAsUTF8()); }

std::vector<InstrumentPreset> findSurgePatches (const juce::File& pluginFile)
{
    std::vector<InstrumentPreset> found;
    for (const auto& p : plugins::SurgePresets::scan (plugins::SurgePresets::findDataFolder (pluginFile),
                                                      plugins::SurgePresets::getUserFolder()))
    {
        InstrumentPreset preset;
        preset.name = p.name;
        preset.category = p.category;
        preset.kind = InstrumentPreset::Kind::file;
        preset.file = p.file;
        preset.id = p.file.getFullPathName(); // as before: the measurements are kept
        preset.version = fileVersion (p.file);
        found.push_back (std::move (preset));
    }
    return found;
}

std::vector<InstrumentPreset> findDiscovered (juce::AudioPluginInstance& instance, const juce::String& version)
{
    std::vector<InstrumentPreset> found;
    std::set<juce::String> seen;
    for (auto& d : plugins::ClapPluginFormat::discoverPresets (instance, InstrumentPresets::most))
    {
        InstrumentPreset preset;
        preset.name = d.name;
        preset.category = d.category;
        preset.kind = InstrumentPreset::Kind::discovered;
        const bool inFile = d.kind == plugins::ClapPluginFormat::DiscoveredPreset::inFile;
        preset.id = (inFile ? d.location : juce::String ("plugin")) + "|" + d.loadKey;
        preset.version = inFile ? fileVersion (juce::File (d.location)) : version;
        if (inFile)
            preset.file = juce::File (d.location);
        preset.discovered = std::move (d);
        if (seen.insert (preset.id).second)
            found.push_back (std::move (preset));
    }
    return found;
}
} // namespace

juce::String InstrumentPresets::libraryOf (const model::Device& device)
{
    switch (familyOf (device))
    {
    case Family::surge:
        return "surge-xt";
    case Family::obxf:
        return "ob-xf";
    case Family::odin:
        return "odin2";
    case Family::other:
        break;
    }
    if (device.kind == model::Device::Kind::stock)
        return "stock-" + device.stockType.retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789-.");
    return "plugin-" + hashOf (device.plugin.format + "|" + device.plugin.fileOrIdentifier + "|" + device.plugin.name);
}

std::vector<InstrumentPreset> InstrumentPresets::find (juce::AudioPluginInstance& instance, const model::Device& device)
{
    const auto description = instance.getPluginDescription();
    const juce::File pluginFile (description.fileOrIdentifier);
    const auto version = description.version + ":" +
                         juce::String (pluginFile.exists() ? pluginFile.getLastModificationTime().toMilliseconds() : 0);
    std::vector<InstrumentPreset> found;
    switch (familyOf (device))
    {
    case Family::surge:
        return findSurgePatches (pluginFile); // all of them
    case Family::obxf:
        found = findObxfPatches (getObxfFolders (pluginFile));
        break;
    case Family::odin:
        found = findOdinPresets (pluginFile);
        break;
    case Family::other:
        found = findDiscovered (instance, version);
        if (found.size() < 2)
            found = findPrograms (instance, version);
        break;
    }
    if (found.size() < 2)
        return {}; // one preset is no choice
    if ((int)found.size() > most)
        found.resize ((size_t)most);
    return found;
}

juce::Result InstrumentPresets::load (juce::AudioPluginInstance& instance, const InstrumentPreset& preset)
{
    switch (preset.kind)
    {
    case InstrumentPreset::Kind::file:
        if (! plugins::ClapPluginFormat::canLoadPresets (instance))
            return juce::Result::fail (instance.getName() + " cannot load presets from Spinus.");
        return plugins::ClapPluginFormat::loadPreset (instance, preset.file);
    case InstrumentPreset::Kind::state:
        if (preset.state.isEmpty())
            return juce::Result::fail ("The preset " + preset.name + " is empty.");
        instance.setStateInformation (preset.state.getData(), (int)preset.state.getSize());
        return juce::Result::ok();
    case InstrumentPreset::Kind::program:
        if (! juce::isPositiveAndBelow (preset.program, instance.getNumPrograms()))
            return juce::Result::fail (instance.getName() + " has no program " + juce::String (preset.program) + ".");
        instance.setCurrentProgram (preset.program);
        return juce::Result::ok();
    case InstrumentPreset::Kind::discovered:
        return plugins::ClapPluginFormat::loadPreset (instance, preset.discovered);
    }
    return juce::Result::fail ("Unknown preset.");
}

//==============================================================================
bool InstrumentPresets::isPlaceholderName (const juce::String& name)
{
    const auto plain = name.replaceCharacter ('_', ' ').trim().toLowerCase(); // "init_patch" too
    if (plain.isEmpty() || plain.containsOnly ("0123456789 -_.#"))
        return true;
    for (auto* placeholder : { "default", "init", "initial", "initialize", "initialise", "init patch", "init preset",
                               "init program", "empty", "empty patch", "untitled", "no name", "unnamed", "new preset",
                               "basic", "program", "preset", "patch" })
        if (plain == placeholder)
            return true;
    // "Program 3", "Preset 12", "Init 2", "Default 1": a numbered slot, not a sound.
    for (auto* slot : { "program", "prog", "preset", "patch", "init", "default", "empty", "untitled", "slot" })
        if (plain.startsWith (slot) && plain.substring ((int)std::strlen (slot)).containsOnly ("0123456789 -_.#:"))
            return true;
    return false;
}

std::vector<InstrumentPreset> InstrumentPresets::findPrograms (juce::AudioPluginInstance& instance,
                                                               const juce::String& version)
{
    std::vector<InstrumentPreset> found;
    const auto count = instance.getNumPrograms();
    if (count < 2)
        return found;
    std::set<juce::String> seen;
    for (int i = 0; i < count && (int)found.size() < most; ++i)
    {
        const auto name = instance.getProgramName (i).trim();
        if (isPlaceholderName (name) || ! seen.insert (name.toLowerCase()).second)
            continue;
        InstrumentPreset preset;
        preset.name = name;
        preset.kind = InstrumentPreset::Kind::program;
        preset.program = i;
        preset.id = "program:" + juce::String (i) + ":" + name;
        preset.version = version;
        found.push_back (std::move (preset));
    }
    if (found.size() < 2)
        found.clear();
    return found;
}

//==============================================================================
std::vector<juce::File> InstrumentPresets::getObxfFolders (const juce::File& pluginFile)
{
    std::vector<juce::File> folders;
    // Downloaded through Spinus: its patches in the pack's folder.
    if (auto beside = pluginFile.getParentDirectory().getChildFile ("Patches"); beside.isDirectory())
        folders.push_back (beside);
    else
    {
        // Installed otherwise: OB-Xf's own shared folder (the first one there is).
        std::vector<juce::File> shared;
#if JUCE_WINDOWS
        for (auto location : { juce::File::commonApplicationDataDirectory, juce::File::userApplicationDataDirectory })
            shared.push_back (juce::File::getSpecialLocation (location).getChildFile ("Surge Synth Team/OB-Xf"));
#else
        for (auto* path : { "/usr/share/OB-Xf", "/usr/share/Surge Synth Team/OB-Xf", "/usr/local/share/OB-Xf",
                            "/usr/local/share/Surge Synth Team/OB-Xf" })
            shared.push_back (juce::File (path));
        shared.push_back (getUserDataRoot().getChildFile ("OB-Xf"));
        shared.push_back (getUserDataRoot().getChildFile ("Surge Synth Team/OB-Xf"));
#endif
        for (const auto& folder : shared)
            if (folder.getChildFile ("Patches").isDirectory())
            {
                folders.push_back (folder.getChildFile ("Patches"));
                break;
            }
    }
    // The user's own.
    if (auto user = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("OB-Xf/Patches");
        user.isDirectory())
        folders.push_back (user);
    return folders;
}

juce::MemoryBlock InstrumentPresets::obxfStateFromPatch (const juce::MemoryBlock& fxp)
{
    // A VST program file: "CcnK", size, "FPCh", format version, plugin ID, plugin version, program count, a
    // 28-byte name, the chunk's size (big-endian) at 56 and the chunk at 60. OB-Xf's chunk is its patch's
    // settings as JUCE binary XML: <OB-Xf ob-xf_version=... programName=... (one attribute per setting)/>.
    constexpr size_t header = 60;
    const auto* bytes = static_cast<const char*> (fxp.getData());
    if (fxp.getSize() < header || std::memcmp (bytes, "CcnK", 4) != 0 || std::memcmp (bytes + 8, "FPCh", 4) != 0)
        return {};
    const auto chunkSize = (size_t)juce::ByteOrder::bigEndianInt (bytes + 56);
    if (chunkSize == 0 || chunkSize > fxp.getSize() - header)
        return {};
    const auto patch = juce::AudioProcessor::getXmlFromBinary (bytes + header, (int)chunkSize);
    if (patch == nullptr || ! patch->hasTagName ("OB-Xf"))
        return {};
    // OB-Xf's state holds one program: <OB-Xf ob-xf_version single-program-format="1"><program .../></OB-Xf>.
    juce::XmlElement state ("OB-Xf");
    state.setAttribute ("ob-xf_version", patch->getStringAttribute ("ob-xf_version"));
    state.setAttribute ("single-program-format", 1);
    auto* program = state.createNewChildElement ("program");
    for (int i = 0; i < patch->getNumAttributes(); ++i)
        if (patch->getAttributeName (i) != "ob-xf_version")
            program->setAttribute (patch->getAttributeName (i), patch->getAttributeValue (i));
    juce::MemoryBlock result;
    juce::AudioProcessor::copyXmlToBinary (state, result);
    return result;
}

std::vector<InstrumentPreset> InstrumentPresets::findObxfPatches (const std::vector<juce::File>& folders)
{
    std::vector<InstrumentPreset> found;
    for (const auto& root : folders)
    {
        std::vector<juce::File> files;
        for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*.fxp", juce::File::findFiles))
            files.push_back (entry.getFile());
        std::sort (files.begin(), files.end(), [&root] (const juce::File& a, const juce::File& b)
                   { return a.getRelativePathFrom (root).compareNatural (b.getRelativePathFrom (root)) < 0; });
        for (const auto& file : files)
        {
            if ((int)found.size() >= most)
                return found;
            juce::MemoryBlock data;
            if (file.getSize() > megabyte || ! file.loadFileAsData (data))
                continue;
            InstrumentPreset preset;
            preset.state = obxfStateFromPatch (data);
            if (preset.state.isEmpty())
                continue;
            const auto folder = file.getParentDirectory();
            preset.name = file.getFileNameWithoutExtension();
            preset.category = folder == root ? juce::String() : folder.getRelativePathFrom (root).replace ("\\", "/");
            preset.kind = InstrumentPreset::Kind::state;
            preset.file = file;
            preset.id = file.getFullPathName();
            preset.version = fileVersion (file);
            found.push_back (std::move (preset));
        }
    }
    return found;
}

//==============================================================================
std::vector<InstrumentPreset> InstrumentPresets::findOdinPresets (const juce::File& pluginFile)
{
    // Odin 2 (2.4) builds its factory presets into the plugin; each is a JUCE ValueTree written as binary
    // (as its .odin files are): the type "Odin", no properties, then its children, the first a "PARAM". Its
    // state is the same tree as binary XML.
    std::vector<InstrumentPreset> found;
    juce::MemoryBlock binary;
    if (pluginFile.getSize() > 256 * megabyte || ! pluginFile.loadFileAsData (binary))
        return found;
    const auto* data = static_cast<const char*> (binary.getData());
    const auto size = binary.getSize();
    const char start[] = "Odin\0\0"; // the type, then 0 properties
    constexpr size_t startSize = sizeof (start) - 1;
    std::set<juce::String> seen;
    for (size_t at = 0; at + startSize + 8 < size && (int)found.size() < most; ++at)
    {
        const auto* candidate = static_cast<const char*> (std::memchr (data + at, 'O', size - at));
        if (candidate == nullptr)
            break;
        at = (size_t)(candidate - data);
        if (at + startSize + 8 >= size || std::memcmp (candidate, start, startSize) != 0)
            continue;
        // The number of children (a compressed int: its byte count, then the bytes), then "PARAM".
        const auto countBytes = (size_t)(juce::uint8)candidate[startSize];
        const auto* first = candidate + startSize + 1 + countBytes;
        if (countBytes == 0 || countBytes > 4 || (size_t)(first - data) + 6 > size ||
            std::memcmp (first, "PARAM\0", 6) != 0)
            continue;
        const auto length = juce::jmin (size - at, (size_t)megabyte); // a preset is at most a few tens of kB
        juce::MemoryInputStream in (candidate, length, false);
        const auto tree = juce::ValueTree::readFromStream (in);
        const auto name = tree.getChildWithName ("misc").getProperty ("patch_name").toString().trim();
        const auto used = (size_t)in.getPosition();
        if (! tree.hasType ("Odin") || isPlaceholderName (name) || ! seen.insert (name).second)
            continue;
        const auto xml = tree.createXml();
        if (xml == nullptr)
            continue;
        InstrumentPreset preset;
        preset.name = name;
        preset.category = "Factory";
        preset.kind = InstrumentPreset::Kind::state;
        juce::AudioProcessor::copyXmlToBinary (*xml, preset.state);
        preset.id = "odin2/" + name;
        preset.version = hashOf (candidate, used);
        found.push_back (std::move (preset));
        at += used - 1;
    }
    std::sort (found.begin(), found.end(), [] (const InstrumentPreset& a, const InstrumentPreset& b)
               { return a.name.compareNatural (b.name) < 0; });
    return found;
}

} // namespace spinus
