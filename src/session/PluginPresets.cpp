#include "PluginPresets.h"

#include "session/HiddenInstrument.h"
#include "model/StockDevices.h"
#include "session/SoundIndex.h"

#include <map>

namespace spinus::pluginPresets
{

namespace
{
/** A plugin's own presets, found once per run: Surge XT has about 3,000 patch files to read. */
std::map<juce::String, std::vector<InstrumentPreset>>& foundBefore()
{
    static std::map<juce::String, std::vector<InstrumentPreset>> found;
    return found;
}

/** The last states made for a preset (a proposal checked, then applied, asks twice), by plugin and preset. */
std::map<juce::String, std::pair<juce::MemoryBlock, juce::String>>& madeBefore()
{
    static std::map<juce::String, std::pair<juce::MemoryBlock, juce::String>> made;
    return made;
}

juce::String keyOf (const model::Device& device)
{
    return InstrumentPresets::libraryOf (device) + "|" + device.plugin.fileOrIdentifier;
}

/** The sounds the user saved for this plugin (Save this sound), as presets that are its state. */
std::vector<InstrumentPreset> savedSoundsFor (const model::Device& device)
{
    SavedSounds saved (SoundIndex::getDefaultFolder());
    saved.load();
    std::vector<InstrumentPreset> presets;
    for (const auto& sound : saved.forInstrument (InstrumentPresets::libraryOf (device)))
    {
        InstrumentPreset p;
        p.kind = InstrumentPreset::Kind::state;
        p.name = sound.name;
        p.category = SavedSounds::category;
        p.id = sound.id;
        p.file = saved.stateFile (sound.id);
        if (p.file.loadFileAsData (p.state) && ! p.state.isEmpty())
            presets.push_back (std::move (p));
    }
    return presets;
}

juce::String someNames (const std::vector<InstrumentPreset>& presets)
{
    juce::StringArray names;
    for (const auto& p : presets)
        if (names.size() < 30)
            names.add (p.name);
    return names.joinIntoString (", ") + (presets.size() > 30 ? "..." : "");
}
} // namespace

std::vector<InstrumentPreset> find (Session& session, const model::Device& device, juce::String& problem)
{
    auto presets = savedSoundsFor (device);
    const auto key = keyOf (device);
    auto& found = foundBefore();
    if (auto it = found.find (key); it != found.end())
    {
        presets.insert (presets.begin(), it->second.begin(), it->second.end());
        return presets;
    }
    HiddenInstrument hidden (session, device);
    if (hidden.getInstance() == nullptr)
    {
        problem = hidden.getProblem().isNotEmpty() ? hidden.getProblem()
                                                   : model::getDeviceDisplayName (device) + " cannot be loaded here.";
        return presets;
    }
    auto own = InstrumentPresets::find (*hidden.getInstance(), device);
    found[key] = own;
    presets.insert (presets.begin(), own.begin(), own.end());
    return presets;
}

const InstrumentPreset* match (const std::vector<InstrumentPreset>& presets, const juce::String& said)
{
    const auto wanted = said.trim();
    for (const auto& p : presets)
        if (p.name.equalsIgnoreCase (wanted) || (p.category + "/" + p.name).equalsIgnoreCase (wanted))
            return &p;
    const auto words = juce::StringArray::fromTokens (wanted.toLowerCase(), " /-_", "");
    const InstrumentPreset* only = nullptr;
    int count = 0;
    for (const auto& p : presets)
    {
        const auto name = p.name.toLowerCase();
        bool all = ! words.isEmpty();
        for (const auto& w : words)
            all = all && name.contains (w);
        if (all && ++count == 1)
            only = &p;
    }
    return count == 1 ? only : nullptr;
}

juce::Result stateWith (Session& session, const model::Device& device, const juce::String& preset,
                        juce::MemoryBlock& state, juce::String& name)
{
    const auto title = model::getDeviceDisplayName (device);
    const auto madeKey = keyOf (device) + "|" + preset.trim().toLowerCase();
    if (auto made = madeBefore().find (madeKey); made != madeBefore().end())
    {
        state = made->second.first;
        name = made->second.second;
        return juce::Result::ok();
    }
    juce::String problem;
    const auto presets = find (session, device, problem);
    if (presets.empty())
        return juce::Result::fail (
            problem.isNotEmpty() ? problem
                                 : title + " has no presets Spinus can list. A sound set up in its window can "
                                           "be kept with Save this sound (Track creator) and loaded by its name.");
    const auto* found = match (presets, preset);
    if (found == nullptr)
        return juce::Result::fail (title + " has no preset \"" + preset + "\" (or more than one fits); some of its " +
                                   juce::String ((int)presets.size()) + " presets: " + someNames (presets));

    // On a copy of the plugin, which waits until the preset is in place (Surge XT loads on its own thread).
    HiddenInstrument hidden (session, device);
    if (hidden.getInstance() == nullptr)
        return juce::Result::fail (hidden.getProblem().isNotEmpty() ? hidden.getProblem()
                                                                    : title + " cannot be loaded here.");
    if (auto loaded = hidden.loadPreset (*found); loaded.failed())
        return loaded;
    state.reset();
    hidden.getInstance()->getStateInformation (state);
    if (state.isEmpty())
        return juce::Result::fail (title + " gave Spinus no state for the preset \"" + found->name + "\".");
    name = found->name;
    // A saved sound may be saved again under its name: only the plugin's own presets are kept.
    if (found->category != SavedSounds::category)
    {
        if (madeBefore().size() > 64)
            madeBefore().clear();
        madeBefore()[madeKey] = { state, name };
    }
    return juce::Result::ok();
}

} // namespace spinus::pluginPresets
