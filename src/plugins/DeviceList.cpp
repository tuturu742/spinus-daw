#include "DeviceList.h"

#include <algorithm>
#include <map>

#include "model/ModelJson.h"
#include "PluginDownloads.h"
#include "plugins/windows/PluginDialogs.h"
#include "model/StockDevices.h"
#include "session/ProgramFolder.h"

namespace spinus::plugins
{

DeviceList::DeviceList() { refresh(); }

void DeviceList::setCatalog (PluginCatalog* c)
{
    catalog = c;
    refresh();
}

void DeviceList::setPreferencesFile (const juce::File& file)
{
    preferencesFile = file;
    preferredFormats.clear();
    const auto saved = juce::JSON::parse (file);
    if (auto* o = saved.getDynamicObject())
        for (auto& p : o->getProperties())
            preferredFormats[p.name.toString()] = p.value.toString();
    refresh();
}

bool DeviceList::preferFormat (const juce::String& key, const juce::String& format)
{
    const auto entry = std::find_if (entries.begin(), entries.end(), [&key] (const Entry& e) { return e.key == key; });
    if (entry == entries.end() || ! entry->formats.contains (format))
        return false;
    preferredFormats[key] = format;
    if (preferencesFile != juce::File())
    {
        auto* o = new juce::DynamicObject();
        for (auto& [k, f] : preferredFormats)
            o->setProperty (k, f);
        preferencesFile.getParentDirectory().createDirectory();
        preferencesFile.replaceWithText (juce::JSON::toString (juce::var (o)));
    }
    refresh();
    return true;
}

void DeviceList::setBundledFolders (std::vector<juce::File> folders)
{
    bundledFolders = std::move (folders);
    refresh();
}

juce::String DeviceList::getBadge (const juce::PluginDescription& d)
{
    // Windows plugins through yabridge: Spinus's bridged folder, or a yabridge setup of the
    // user's own.
    if (PluginDialogs::isWindowsPlugin (d.fileOrIdentifier))
        return "WIN";
    if (d.pluginFormatName == "VST")
        return "VST2";
    return d.pluginFormatName;
}

model::Device DeviceList::makeDevice (const juce::PluginDescription& d)
{
    model::Device device;
    device.kind = model::Device::Kind::plugin;
    // Projects keep the computer's path, the same for the Flatpak and the native build.
    device.plugin = { d.pluginFormatName, d.name, d.manufacturerName, toPortablePluginPath (d.fileOrIdentifier),
                      d.uniqueId };
    return device;
}

void DeviceList::refresh()
{
    entries.clear();

    for (auto& own : model::getSpinusDevices())
    {
        Entry e;
        e.name = own.displayName;
        e.maker = "Spinus";
        e.instrument = own.isInstrument;
        e.origin = Origin::spinus;
        e.device.kind = model::Device::Kind::stock;
        e.device.stockType = own.stockType;
        e.detail = e.name + " (made by Spinus)";
        entries.push_back (std::move (e));
    }

    // Installed plugins a bundled device uses (e.g. Surge XT from the system's package manager) are
    // listed once, as the bundled device; so is another copy of the same plugin file elsewhere (Surge XT
    // downloaded through Spinus and also installed in ~/.clap or as a Flatpak extension).
    std::set<std::pair<juce::String, juce::String>> usedByBundled, copiesOfBundled; // (file, plugin name)

    for (auto& bundled : model::getBundledDevices())
    {
        // Not published for this system and nothing standing in (LSP Plugins on Windows): not offered.
        const bool standIn = model::findStandIn (bundled.stockType) != nullptr;
        if (PluginDownloads::findStandardPack (bundled.pack) == nullptr && ! standIn)
            continue;
        Entry e;
        e.name = bundled.displayName;
        e.maker = bundled.project;
        e.instrument = bundled.isInstrument;
        e.origin = Origin::bundled;
        e.device.kind = model::Device::Kind::stock;
        e.device.stockType = bundled.stockType;
        e.pack = bundled.pack;
        if (bundledFolders)
        {
            const auto file = PluginDownloads::findFile (*bundledFolders, bundled.file);
            e.available = file != juce::File();
            if (e.available)
            {
                usedByBundled.insert ({ file.getFullPathName(), bundled.pluginName });
                copiesOfBundled.insert ({ file.getFileName(), bundled.pluginName });
            }
        }
        if (standIn && PluginDownloads::findStandardPack (bundled.pack) == nullptr)
            e.available = true; // Tracktion's own effect plays it on this system
        e.detail = e.name + " from " + e.maker + " (open source, " + bundled.license + "; not made by Spinus" +
                   (e.available ? juce::String (")") : juce::String ("). Not downloaded yet: double-click to get it"));
        entries.push_back (std::move (e));
    }

    if (catalog != nullptr)
    {
        const auto bundledFolder = catalog->getBundledFolder();
        // One entry per plugin: a plugin installed in several formats is listed in the best one (CLAP, then
        // VST3, LV2, VST2) or the one the user prefers; the hint bar says which others are installed.
        auto formatRank = [] (const juce::String& format)
        {
            const juce::StringArray order { "CLAP", "VST3", "LV2", "VST2" };
            const auto at = order.indexOf (format);
            return at >= 0 ? at : order.size();
        };
        struct Found
        {
            Entry entry;
            int rank = 0;
            juce::StringArray otherFormats;
        };
        std::map<juce::String, Found> unique; // by name, maker and kind
        std::vector<Entry> installed;
        for (auto& type : catalog->getKnownPlugins().getTypes())
        {
            const juce::File file (type.fileOrIdentifier);
            if (juce::File::isAbsolutePath (type.fileOrIdentifier) && file.isAChildOf (bundledFolder))
                continue;
            // Spinus's own devices installed as plugins for other programs: Spinus lists its own copies.
            if (type.manufacturerName == "Spinus")
                continue;
            if (usedByBundled.contains ({ type.fileOrIdentifier, type.name }) ||
                (type.pluginFormatName == "CLAP" && copiesOfBundled.contains ({ file.getFileName(), type.name })))
                continue;

            Entry e;
            // A VST2 plugin's name is often its file's ("TubeSaturatorVintage_64", "AGML2"); its product
            // name says what it is.
            e.name =
                type.pluginFormatName == "VST" && type.descriptiveName.isNotEmpty() ? type.descriptiveName : type.name;
            e.maker = type.manufacturerName.isNotEmpty() ? type.manufacturerName : juce::String ("Unknown maker");
            e.badge = getBadge (type);
            e.instrument = type.isInstrument;
            e.device = makeDevice (type);
            const auto format = type.pluginFormatName == "VST" ? juce::String ("VST2") : type.pluginFormatName;
            e.detail = e.name + " by " + e.maker + ", " + (e.badge == format ? format : e.badge + " " + format) + ": " +
                       type.fileOrIdentifier;
            e.format = format;

            const auto key = e.name.toLowerCase() + "|" + e.maker.toLowerCase() + (e.instrument ? "|i" : "|e");
            e.key = key;
            const auto preferred = preferredFormats.find (key);
            const auto rank =
                preferred != preferredFormats.end() && preferred->second == format ? -1 : formatRank (format);
            auto found = unique.find (key);
            if (found == unique.end())
                unique[key] = { std::move (e), rank, { format } };
            else
            {
                found->second.otherFormats.addIfNotAlreadyThere (format);
                if (rank < found->second.rank)
                {
                    found->second.entry = std::move (e);
                    found->second.rank = rank;
                }
            }
        }
        for (auto& [key, found] : unique)
        {
            std::sort (found.otherFormats.begin(), found.otherFormats.end(),
                       [&formatRank] (const juce::String& a, const juce::String& b)
                       { return formatRank (a) < formatRank (b); });
            found.entry.formats = found.otherFormats;
            juce::StringArray others = found.otherFormats;
            others.removeString (found.entry.format);
            if (! others.isEmpty())
                found.entry.detail << " (also installed as " << others.joinIntoString (", ")
                                   << ": right-click to use another)";
            installed.push_back (std::move (found.entry));
        }

        std::sort (installed.begin(), installed.end(),
                   [] (const Entry& a, const Entry& b)
                   {
                       if (auto c = a.name.compareNatural (b.name); c != 0)
                           return c < 0;
                       return a.badge < b.badge;
                   });
        for (auto& e : installed)
            entries.push_back (std::move (e));
    }

    sendChangeMessage();
}

juce::var DeviceList::toVar (const Entry& e)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("spinusDevice", model::deviceToJson (e.device));
    o->setProperty ("name", e.name);
    o->setProperty ("instrument", e.instrument);
    return o;
}

bool DeviceList::fromVar (const juce::var& v, Entry& result)
{
    if (! v.hasProperty ("spinusDevice"))
        return false;

    Entry e;
    if (model::deviceFromJson (v["spinusDevice"], e.device).isNotEmpty())
        return false;
    e.name = v["name"].toString();
    e.instrument = (bool)v["instrument"];
    result = std::move (e);
    return true;
}

} // namespace spinus::plugins
