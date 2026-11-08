#include "PluginKeyNames.h"

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wshadow", "-Wnon-virtual-dtor", "-Wzero-as-null-pointer-constant",
                                     "-Wsign-conversion", "-Wextra-semi", "-Wpedantic")
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100 4458)
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
JUCE_END_IGNORE_WARNINGS_MSVC
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace spinus::pluginkeys
{

namespace
{
/** A VST3 instrument's key switches (IKeyswitchController), when its controller is the component itself (most
    are); keys `keyswitchMin` to `keyswitchMax` of each, named by its title. */
std::map<int, juce::String> keySwitchesOf (juce::AudioPluginInstance& instance)
{
    std::map<int, juce::String> found;
    auto* client = instance.getVST3Client();
    auto* component = client != nullptr ? client->getIComponentPtr() : nullptr;
    if (component == nullptr)
        return found;
    Steinberg::Vst::IKeyswitchController* switches = nullptr;
    if (component->queryInterface (Steinberg::Vst::IKeyswitchController_iid, (void**)&switches) !=
            Steinberg::kResultOk ||
        switches == nullptr)
        return found;
    for (Steinberg::int32 i = 0, count = switches->getKeyswitchCount (0, 0); i < count && i < 128; ++i)
    {
        Steinberg::Vst::KeyswitchInfo info {};
        if (switches->getKeyswitchInfo (0, 0, i, info) != Steinberg::kResultOk)
            continue;
        const auto title =
            juce::String (juce::CharPointer_UTF16 ((const juce::CharPointer_UTF16::CharType*)info.title)).trim();
        for (auto key = juce::jmax (0, (int)info.keyswitchMin); key <= juce::jmin (127, (int)info.keyswitchMax); ++key)
            found[key] = title.isNotEmpty() ? title : juce::String ("key switch");
    }
    switches->release();
    return found;
}
} // namespace

bool isNoteName (const juce::String& name)
{
    const auto t = name.trim();
    if (t.isEmpty() || ! juce::String ("ABCDEFGabcdefg").containsChar (t[0]))
        return false;
    auto rest = t.substring (1);
    if (rest.startsWithChar ('#') || rest.startsWithChar ('b'))
        rest = rest.substring (1);
    if (rest.startsWithChar ('-'))
        rest = rest.substring (1);
    return rest.isNotEmpty() && rest.length() <= 2 && rest.containsOnly ("0123456789");
}

bool namesASwitch (const juce::String& name)
{
    const auto lower = name.toLowerCase();
    if (lower.contains ("switch") || lower.contains ("articulation"))
        return true;
    // "KS" as a word of its own ("KS Legato", "Legato (KS)")
    const auto words = juce::StringArray::fromTokens (lower, " -_:()[]/", "");
    return words.contains ("ks");
}

std::vector<Named> read (juce::AudioPluginInstance& instance)
{
    std::vector<Named> named;
    const auto switches = keySwitchesOf (instance);
    for (int key = 0; key < 128; ++key)
    {
        if (auto it = switches.find (key); it != switches.end())
        {
            named.push_back ({ key, it->second, true });
            continue;
        }
        const auto name = instance.getNameForMidiNoteNumber (key, 0).value_or (juce::String()).trim();
        if (name.isNotEmpty() && ! isNoteName (name))
            named.push_back ({ key, name, namesASwitch (name) });
    }
    return named;
}

std::array<InstrumentProfiles::Key, 128> asKeys (const std::vector<Named>& named)
{
    using Job = InstrumentProfiles::Job;
    std::array<InstrumentProfiles::Key, 128> keys {};
    for (const auto& n : named)
        if (n.key >= 0 && n.key < 128)
            keys[(size_t)n.key] = { n.switches ? Job::switches : Job::plays, n.name,
                                    InstrumentProfiles::Source::plugin };
    return keys;
}

} // namespace spinus::pluginkeys
