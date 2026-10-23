#include "SandboxList.h"

namespace spinus::plugins
{

SandboxList::SandboxList (juce::File f) : file (std::move (f))
{
    const auto json = juce::JSON::parse (file);
    if (auto* list = json["plugins"].getArray())
        for (auto& p : *list)
            if (p["key"].toString().isNotEmpty())
                names[p["key"].toString()] = p["name"].toString();
}

void SandboxList::set (const juce::String& key, bool runSeparately, const juce::String& name)
{
    if (runSeparately == contains (key))
        return;
    if (runSeparately)
        names[key] = name;
    else
        names.erase (key);
    save();
}

std::vector<std::pair<juce::String, juce::String>> SandboxList::getPlugins() const
{
    return { names.begin(), names.end() };
}

void SandboxList::save() const
{
    juce::Array<juce::var> list;
    for (auto& [key, name] : names)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("key", key);
        o->setProperty ("name", name);
        list.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("plugins", list);
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (root)));
}

} // namespace spinus::plugins
