#include "EffectProfiles.h"

namespace spinus
{

namespace
{
const juce::StringArray sourceNames { "user", "probing", "plugin", "manual", "bundled", "imported" };

EffectProfiles::Source sourceFrom (const juce::var& v)
{
    const auto i = sourceNames.indexOf (v.toString());
    return i >= 0 ? (EffectProfiles::Source)i : EffectProfiles::Source::user;
}
} // namespace

const EffectProfiles::Setting* EffectProfiles::Profile::find (const juce::String& id) const
{
    for (const auto& s : settings)
        if (s.id == id)
            return &s;
    return nullptr;
}

EffectProfiles::EffectProfiles (const juce::File& indexFolder) : file (fileIn (indexFolder)) {}

void EffectProfiles::load()
{
    profiles.clear();
    const auto saved = juce::JSON::parse (file);
    if (auto* effects = saved["profiles"].getDynamicObject())
        for (const auto& [effect, profile] : effects->getProperties())
            if (auto read = fromVar (profile))
                profiles[effect.toString()] = std::move (*read);
}

bool EffectProfiles::save() const
{
    auto* all = new juce::DynamicObject();
    for (const auto& [effect, profile] : profiles)
        all->setProperty (effect, toVar (profile));
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("profiles", juce::var (all));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (juce::var (root)) + "\n");
}

std::optional<EffectProfiles::Profile> EffectProfiles::get (const juce::String& effect) const
{
    if (const auto found = profiles.find (effect); found != profiles.end())
        return found->second;
    return std::nullopt;
}

void EffectProfiles::set (const juce::String& effect, std::optional<Profile> profile)
{
    if (profile.has_value())
        std::erase_if (profile->settings, [] (const Setting& s) { return s.isEmpty(); });
    if (! profile.has_value() || profile->isEmpty())
        profiles.erase (effect);
    else
        profiles[effect] = std::move (*profile);
}

juce::var EffectProfiles::describeForAgents (const Profile& profile)
{
    juce::Array<juce::var> settings;
    for (const auto& s : profile.settings)
    {
        if (s.isEmpty())
            continue;
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", s.id);
        o->setProperty ("name", s.name);
        if (s.what.isNotEmpty())
            o->setProperty ("what", s.what);
        if (s.low.isNotEmpty() || s.high.isNotEmpty())
            o->setProperty ("safeRange", (s.low.isNotEmpty() ? s.low : juce::String ("its lowest")) + " to " +
                                             (s.high.isNotEmpty() ? s.high : juce::String ("its highest")));
        if (s.matters)
            o->setProperty ("matters", true);
        settings.add (juce::var (o));
    }
    if (profile.goodFor.isEmpty() && settings.isEmpty())
        return {};
    auto* o = new juce::DynamicObject();
    if (profile.goodFor.isNotEmpty())
        o->setProperty ("goodFor", profile.goodFor);
    if (! settings.isEmpty())
        o->setProperty ("settings", settings);
    return juce::var (o);
}

std::optional<EffectProfiles::Reading> EffectProfiles::readingFrom (const juce::var& v)
{
    Reading reading;
    reading.goodFor = v["goodFor"].toString().trim();
    if (const auto* settings = v["settings"].getArray())
        for (const auto& r : *settings)
        {
            Setting s { r["id"].toString(),
                        {},
                        r["what"].toString().trim(),
                        r["low"].toString().trim(),
                        r["high"].toString().trim(),
                        (bool)r["matters"],
                        Source::manual };
            if (s.id.isNotEmpty() && ! s.isEmpty())
                reading.settings.push_back (std::move (s));
        }
    if (reading.goodFor.isEmpty() && reading.settings.empty())
        return std::nullopt;
    reading.notes = v["notes"].toString();
    reading.by = v["by"].toString();
    if (const auto* read = v["read"].getArray())
        for (const auto& r : *read)
            if (r["problem"].toString().isNotEmpty())
                reading.problems.add (r["source"].toString() + ": " + r["problem"].toString());
    return reading;
}

juce::var EffectProfiles::toVar (const Profile& profile)
{
    auto* o = new juce::DynamicObject();
    if (profile.goodFor.isNotEmpty())
        o->setProperty ("goodFor", profile.goodFor);
    juce::Array<juce::var> settings;
    for (const auto& s : profile.settings)
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("id", s.id);
        r->setProperty ("name", s.name);
        if (s.what.isNotEmpty())
            r->setProperty ("what", s.what);
        if (s.low.isNotEmpty())
            r->setProperty ("low", s.low);
        if (s.high.isNotEmpty())
            r->setProperty ("high", s.high);
        if (s.matters)
            r->setProperty ("matters", true);
        r->setProperty ("source", sourceNames[(int)s.source]);
        settings.add (juce::var (r));
    }
    o->setProperty ("settings", settings);
    juce::Array<juce::var> manuals;
    for (const auto& m : profile.manuals)
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("source", m.source);
        if (m.version.isNotEmpty())
            r->setProperty ("version", m.version);
        manuals.add (juce::var (r));
    }
    if (! manuals.isEmpty())
        o->setProperty ("manuals", manuals);
    return juce::var (o);
}

std::optional<EffectProfiles::Profile> EffectProfiles::fromVar (const juce::var& v)
{
    if (! v.isObject())
        return std::nullopt;
    Profile profile;
    profile.goodFor = v["goodFor"].toString();
    if (const auto* settings = v["settings"].getArray())
        for (const auto& r : *settings)
        {
            Setting s { r["id"].toString(),   r["name"].toString(), r["what"].toString(),    r["low"].toString(),
                        r["high"].toString(), (bool)r["matters"],   sourceFrom (r["source"]) };
            if (s.id.isNotEmpty() && ! s.isEmpty())
                profile.settings.push_back (std::move (s));
        }
    if (const auto* manuals = v["manuals"].getArray())
        for (const auto& r : *manuals)
            if (r["source"].toString().isNotEmpty())
                profile.manuals.push_back ({ r["source"].toString(), r["version"].toString() });
    if (profile.isEmpty())
        return std::nullopt;
    return profile;
}

juce::var EffectProfiles::toShared (const Profile& profile, const juce::String& effectName)
{
    auto shared = profile;
    std::erase_if (shared.manuals, [] (const Manual& m) { return ! m.isWebPage(); });
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", "spinus.effect-profile");
    o->setProperty ("version", 1);
    o->setProperty ("effect", effectName);
    o->setProperty ("profile", toVar (shared));
    return juce::var (o);
}

std::optional<EffectProfiles::Profile> EffectProfiles::fromShared (const juce::var& v, juce::String& effectName,
                                                                   juce::String& problem)
{
    if (v["kind"].toString() != "spinus.effect-profile")
    {
        problem = "it is not an effect profile Spinus exported.";
        return std::nullopt;
    }
    if ((int)v["version"] > 1)
    {
        problem = "it was exported by a newer Spinus.";
        return std::nullopt;
    }
    auto profile = fromVar (v["profile"]);
    if (! profile.has_value())
    {
        problem = "it holds nothing Spinus can use.";
        return std::nullopt;
    }
    for (auto& s : profile->settings)
        s.source = Source::imported;
    effectName = v["effect"].toString();
    return profile;
}

} // namespace spinus
