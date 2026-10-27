#include "UserRoles.h"

namespace spinus
{

UserRoles::UserRoles (const juce::File& indexFolder) : file (fileIn (indexFolder)) {}

void UserRoles::load()
{
    roles.clear();
    const auto saved = juce::JSON::parse (file);
    if (auto* instruments = saved["roles"].getDynamicObject())
        for (const auto& [instrument, presets] : instruments->getProperties())
            if (auto* o = presets.getDynamicObject())
                for (const auto& [preset, role] : o->getProperties())
                    if (role.toString().isNotEmpty())
                        roles[instrument.toString()][preset.toString() == "*" ? juce::String() : preset.toString()] =
                            role.toString();
}

bool UserRoles::save() const
{
    auto* all = new juce::DynamicObject();
    for (const auto& [instrument, presets] : roles)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [preset, role] : presets)
            o->setProperty (preset.isEmpty() ? juce::Identifier ("*") : juce::Identifier (preset), role);
        all->setProperty (instrument, juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("roles", juce::var (all));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (juce::var (root)) + "\n");
}

juce::String UserRoles::get (const juce::String& instrument, const juce::String& preset) const
{
    const auto found = roles.find (instrument);
    if (found == roles.end())
        return {};
    if (preset.isNotEmpty())
        if (const auto p = found->second.find (preset); p != found->second.end())
            return p->second;
    const auto whole = found->second.find ({});
    return whole != found->second.end() ? whole->second : juce::String();
}

void UserRoles::set (const juce::String& instrument, const juce::String& preset, const juce::String& role)
{
    if (role.isEmpty())
    {
        if (const auto found = roles.find (instrument); found != roles.end())
        {
            found->second.erase (preset);
            if (found->second.empty())
                roles.erase (found);
        }
        return;
    }
    roles[instrument][preset] = role;
}

} // namespace spinus
