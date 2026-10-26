#include "Browser.h"

#include "Theme.h"
#include "core/HiddenFiles.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "project/AudioImport.h"
#include "project/ProjectFiles.h"

namespace spinus::ui
{

namespace
{
constexpr int headerHeight = 24, searchHeight = 26, footerHeight = 52, rowHeight = 22;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::Colour badgeColour (const juce::String& badge)
{
    if (badge == "VST3")
        return juce::Colour (0xff4f8fd6);
    if (badge == "CLAP")
        return juce::Colour (0xffd68a3c);
    if (badge == "LV2")
        return juce::Colour (0xff5fae6a);
    if (badge == "VST2")
        return juce::Colour (0xff8a74c9);
    if (badge == "WIN")
        return juce::Colour (0xffc95c5c);
    return juce::Colour (0xff808080);
}
} // namespace

//==============================================================================
/** Runs PluginCatalog::scan on its own thread; the Browser polls the progress. The catalog is not
    touched by the message thread until the scan has finished. */
class Browser::ScanThread final : public juce::Thread
{
public:
    explicit ScanThread (plugins::PluginCatalog& c) : juce::Thread ("Spinus plugin scan"), catalog (c) {}

    ~ScanThread() override
    {
        cancel = true;
        stopThread (-1);
    }

    void run() override
    {
        summary = catalog.scan (
            [this] (const plugins::PluginScanner::Progress& p)
            {
                const juce::ScopedLock lock (progressLock);
                progress = p;
            },
            &cancel);
        done = true;
    }

    plugins::PluginScanner::Progress getProgress() const
    {
        const juce::ScopedLock lock (progressLock);
        return progress;
    }

    plugins::PluginCatalog& catalog;
    std::atomic<bool> cancel { false }, done { false };
    plugins::PluginScanner::Summary summary;

private:
    juce::CriticalSection progressLock;
    plugins::PluginScanner::Progress progress;
};

//==============================================================================
Browser::Browser (Session& s) : session (s), devices (s.getDeviceList())
{
    setHelpText ("Browser: double-click an instrument to add a channel, an effect to add it to the selected mixer "
                 "insert, or drag it there");

    search.setTextToShowWhenEmpty ("Search", juce::Colours::grey);
    search.setHelpText (
        "Type to find an instrument, effect or plugin by name, maker or format, or a sample or project by name");
    search.onTextChange = [this] { rebuildRows(); };
    search.onEscapeKey = [this] { search.clear(); };
    addAndMakeVisible (search);

    list.setRowHeight (rowHeight);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.addMouseListener (this, true);
    addAndMakeVisible (list);

    status.setFont (juce::FontOptions (11.0f));
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (status);

    scanButton.setHelpText ("Look for installed plugins (VST3, CLAP, LV2, VST2). Each file is checked in a separate "
                            "program, so a broken plugin cannot crash Spinus");
    scanButton.onClick = [this]
    {
        if (isScanning())
            cancelScan();
        else
            startScan();
    };
    addAndMakeVisible (scanButton);

    devices.addChangeListener (this);
    projects.addChangeListener (this);
    rebuildRows();
}

Browser::~Browser()
{
    projects.removeChangeListener (this);
    devices.removeChangeListener (this);
    scan = nullptr;
}

void Browser::setSearchText (const juce::String& text)
{
    search.setText (text, false); // the editor's own change message arrives asynchronously
    rebuildRows();
}

//==============================================================================
void Browser::rebuildRows()
{
    rows.clear();
    const auto query = search.getText().trim();
    const bool searching = query.isNotEmpty();
    auto isOpen = [&] (const juce::String& key) { return searching || ! closed.contains (key); };
    auto* catalog = devices.getCatalog();

    const auto& entries = devices.getEntries();
    auto matches = [&] (const plugins::DeviceList::Entry& e)
    {
        return ! searching || e.name.containsIgnoreCase (query) || e.maker.containsIgnoreCase (query) ||
               e.badge.equalsIgnoreCase (query);
    };

    // Projects first, for opening one quickly; then the user's own samples, a library used every day (two rows
    // until a folder is added).
    rows.push_back ({ RowKind::section, "Projects", {}, "projects" });
    if (isOpen ("projects"))
        addProjectRows (query);

    rows.push_back ({ RowKind::section, "Samples", {}, "samples" });
    if (isOpen ("samples"))
        addSampleRows (query);

    for (bool instruments : { true, false })
    {
        const juce::String section = instruments ? "instruments" : "effects";
        rows.push_back ({ RowKind::section, instruments ? "Instruments" : "Effects", {}, section });
        if (! isOpen (section))
            continue;

        using Origin = plugins::DeviceList::Origin;
        for (auto origin : { Origin::installed, Origin::spinus, Origin::bundled })
        {
            if (origin == Origin::installed && catalog == nullptr)
                continue;

            const auto group = section + (origin == Origin::installed ? ".installed"
                                          : origin == Origin::spinus  ? ".spinus"
                                                                      : ".bundled");
            std::vector<Row> items;
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].instrument == instruments && entries[i].origin == origin && matches (entries[i]))
                {
                    Row row { RowKind::device, entries[i].name, entries[i].badge, {}, (int)i };
                    if (origin == Origin::bundled)
                        row.note = entries[i].maker + (entries[i].available ? "" : "  - get it");
                    row.available = entries[i].available;
                    items.push_back (std::move (row));
                }

            // Spinus's own devices arrive with the Sampler; until then the group is not shown.
            if (items.empty() && (searching || origin == Origin::spinus))
                continue;

            const juce::String title = origin == Origin::installed ? "Installed plugins"
                                       : origin == Origin::spinus  ? "Spinus"
                                                                   : "Free open source";
            rows.push_back ({ RowKind::group, title + "  (" + juce::String ((int)items.size()) + ")", {}, group });
            if (! isOpen (group))
                continue;

            if (items.empty())
                rows.push_back ({ RowKind::message, "None yet: click Scan plugins", {}, {} });
            for (auto& r : items)
                rows.push_back (r);
        }
    }

    if (catalog != nullptr && ! isScanning() && ! catalog->getProblems().getAll().empty())
    {
        const auto& problems = catalog->getProblems().getAll();
        rows.push_back (
            { RowKind::section, "Problem plugins  (" + juce::String ((int)problems.size()) + ")", {}, "problems" });
        if (isOpen ("problems"))
            for (size_t i = 0; i < problems.size(); ++i)
                if (! searching || problems[i].file.containsIgnoreCase (query))
                    rows.push_back ({ RowKind::problem,
                                      juce::File (problems[i].file).getFileName(),
                                      problems[i].format == "VST" ? juce::String ("VST2") : problems[i].format,
                                      {},
                                      -1,
                                      (int)i });
    }

    list.updateContent();
    list.repaint();

    scanButton.setVisible (catalog != nullptr);
    if (catalog != nullptr && ! isScanning() && status.getText().isEmpty())
    {
        const auto installed = countInstalled();
        status.setText (installed == 0
                            ? juce::String ("Click Scan plugins to find the plugins installed on this computer.")
                            : juce::String ((int)installed) + " installed plugins.",
                        juce::dontSendNotification);
    }
}

//==============================================================================
namespace
{
bool isSampleFile (const juce::File& f)
{
    return f.hasFileExtension (project::ownAudioFormats) || f.hasFileExtension (project::ffmpegAudioFormats);
}

/** A folder's subfolders, then its audio files, each by name; hidden ones left out. */
std::pair<juce::Array<juce::File>, juce::Array<juce::File>> listFolder (const juce::File& folder)
{
    juce::Array<juce::File> folders, files;
    for (const auto& entry : juce::RangedDirectoryIterator (
             folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
    {
        const auto& f = entry.getFile();
        if (core::isHidden (f))
            continue;
        if (entry.isDirectory())
            folders.add (f);
        else if (isSampleFile (f))
            files.add (f);
    }
    struct ByName
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareNatural (b.getFileName());
        }
    } byName;
    folders.sort (byName);
    files.sort (byName);
    return { folders, files };
}
} // namespace

void Browser::addSampleRows (const juce::String& query)
{
    if (query.isEmpty())
    {
        for (auto& folder : sampleFolders)
            addFolderRows (folder, 0);
    }
    else
    {
        int found = 0;
        for (auto& file : getSampleIndex())
            if (file.getFileName().containsIgnoreCase (query))
            {
                if (++found > maxSearchResults)
                {
                    rows.push_back ({ RowKind::message, "More found: type more of the name", {}, {} });
                    break;
                }
                Row row { RowKind::sample, file.getFileNameWithoutExtension(),
                          file.getFileExtension().substring (1).toUpperCase() };
                row.path = file.getFullPathName();
                row.note = file.getParentDirectory().getFileName();
                rows.push_back (std::move (row));
            }
        if (found == 0 && ! sampleFolders.isEmpty())
            rows.push_back ({ RowKind::message, "No samples with that name", {}, {} });
    }

    if (query.isEmpty() || sampleFolders.isEmpty())
        rows.push_back ({ RowKind::action, "+ Add a folder of samples...", {}, "addSamples" });
}

//==============================================================================
void Browser::addProjectRows (const juce::String& query)
{
    const bool searching = query.isNotEmpty();
    auto matches = [&] (const juce::String& name) { return ! searching || name.containsIgnoreCase (query); };
    auto isOpen = [&] (const juce::String& key) { return searching || ! closed.contains (key); };
    auto projectRow = [] (const ProjectList::Project& p, const juce::String& note)
    {
        Row row { RowKind::project, p.name };
        row.path = p.folder.getFullPathName();
        row.note = note;
        row.depth = 1;
        return row;
    };

    // The demo songs that come with Spinus, first.
    int shown = 0;
    for (auto& song : demos)
        if (const auto name = song.getFileName() + " (demo)"; matches (name))
        {
            Row demo { RowKind::project, name, {}, "demo" };
            demo.path = song.getFullPathName();
            rows.push_back (std::move (demo));
            ++shown;
        }

    // The folders added, each with the projects found in it (and the subfolder each is in).
    std::vector<Row> items;
    for (auto& folder : projects.getFolders())
    {
        items.clear();
        for (auto& p : folder.projects)
            if (matches (p.name))
            {
                const auto parent = p.folder.getParentDirectory();
                items.push_back (projectRow (p, parent == folder.folder ? juce::String()
                                                                        : parent.getRelativePathFrom (folder.folder)));
            }
        if (searching && items.empty())
            continue;

        const auto key = "projects.folder:" + folder.folder.getFullPathName();
        Row group {
            RowKind::group, folder.folder.getFileName() + "  (" + juce::String ((int)items.size()) + ")", {}, key
        };
        group.path = folder.folder.getFullPathName();
        group.note = folder.missing                                     ? juce::String ("missing")
                     : projects.isScanning() && folder.projects.empty() ? juce::String ("searching...")
                     : folder.truncated                                 ? juce::String ("partly searched")
                                                                        : juce::String();
        rows.push_back (std::move (group));
        if (isOpen (key))
        {
            for (auto& r : items)
                rows.push_back (r);
            if (items.empty())
                rows.push_back ({ RowKind::message,
                                  projects.isScanning() ? "Looking for projects..." : "No projects in this folder" });
        }
        shown += (int)items.size();
    }

    const bool anyListed = ! demos.isEmpty() || ! projects.getAllProjects().isEmpty();
    if (searching && shown == 0 && anyListed)
        rows.push_back ({ RowKind::message, "No projects with that name" });
    if (! searching)
        rows.push_back ({ RowKind::action, "+ Add a folder of projects...", {}, "addProjects" });
}

void Browser::setProjectsFile (const juce::File& f) { projects.setFile (f); }

void Browser::setDemos (juce::Array<juce::File> songs)
{
    demos = std::move (songs);
    rebuildRows();
}

bool Browser::addProjectFolder (const juce::File& folder)
{
    if (! projects.addFolder (folder))
    {
        status.setText (folder.getFileName() + " is already in the list of projects.", juce::dontSendNotification);
        return false;
    }
    closed.erase ("projects");
    return true;
}

void Browser::chooseProjectFolder()
{
    folderChooser = std::make_unique<core::FileDialog> (
        "Add a folder of projects", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [safe = juce::Component::SafePointer<Browser> (this)] (const core::FileDialog& fc)
                                {
                                    if (safe != nullptr && fc.getResult().isDirectory())
                                        safe->addProjectFolder (fc.getResult());
                                });
}

void Browser::addFolderRows (const juce::File& folder, int depth)
{
    Row row { RowKind::folder, folder.getFileName() };
    row.path = folder.getFullPathName();
    row.depth = depth;
    if (depth == 0)
        row.note = folder.isDirectory() ? juce::String() : juce::String ("missing");
    rows.push_back (std::move (row));
    if (! openFolders.contains (folder.getFullPathName()) || ! folder.isDirectory())
        return;

    const auto [folders, files] = listFolder (folder);
    for (auto& sub : folders)
        addFolderRows (sub, depth + 1);
    for (auto& file : files)
    {
        Row sample { RowKind::sample, file.getFileNameWithoutExtension(),
                     file.getFileExtension().substring (1).toUpperCase() };
        sample.path = file.getFullPathName();
        sample.depth = depth + 1;
        rows.push_back (std::move (sample));
    }
    if (folders.isEmpty() && files.isEmpty())
    {
        Row empty { RowKind::message, "No audio files here" };
        empty.depth = depth + 1;
        rows.push_back (std::move (empty));
    }
}

const std::vector<juce::File>& Browser::getSampleIndex()
{
    if (! sampleIndexValid)
    {
        sampleIndex.clear();
        for (auto& folder : sampleFolders)
            for (const auto& entry : juce::RangedDirectoryIterator (
                     folder, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if ((int)sampleIndex.size() >= maxIndexedFiles)
                    break;
                if (isSampleFile (entry.getFile()) && ! core::isHiddenBelow (entry.getFile(), folder))
                    sampleIndex.push_back (entry.getFile());
            }
        // By folder, then name, as the folders list them.
        std::sort (sampleIndex.begin(), sampleIndex.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getFullPathName().compareNatural (b.getFullPathName()) < 0; });
        sampleIndexValid = true;
    }
    return sampleIndex;
}

void Browser::setSampleFoldersFile (const juce::File& f)
{
    sampleFoldersFile = f;
    sampleFolders.clear();
    juce::StringArray lines;
    f.readLines (lines);
    for (auto& line : lines)
        if (line.trim().isNotEmpty() && juce::File::isAbsolutePath (line.trim()))
            sampleFolders.addIfNotAlreadyThere (juce::File (line.trim()));
    sampleIndexValid = false;
    rebuildRows();
}

void Browser::saveSampleFolders() const
{
    if (sampleFoldersFile == juce::File())
        return;
    juce::StringArray lines;
    for (auto& folder : sampleFolders)
        lines.add (folder.getFullPathName());
    sampleFoldersFile.getParentDirectory().createDirectory();
    sampleFoldersFile.replaceWithText (lines.joinIntoString ("\n") + "\n");
}

bool Browser::addSampleFolder (const juce::File& folder)
{
    if (! folder.isDirectory() || sampleFolders.contains (folder))
        return false;
    sampleFolders.add (folder);
    openFolders.insert (folder.getFullPathName());
    sampleIndexValid = false;
    saveSampleFolders();
    rebuildRows();
    return true;
}

void Browser::removeSampleFolder (const juce::File& folder)
{
    sampleFolders.removeFirstMatchingValue (folder);
    sampleIndexValid = false;
    saveSampleFolders();
    rebuildRows();
}

void Browser::toggleFolder (const juce::File& folder)
{
    if (! openFolders.erase (folder.getFullPathName()))
        openFolders.insert (folder.getFullPathName());
    rebuildRows();
}

void Browser::chooseSampleFolder()
{
    folderChooser = std::make_unique<core::FileDialog> (
        "Add a folder of samples", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [safe = juce::Component::SafePointer<Browser> (this)] (const core::FileDialog& fc)
                                {
                                    if (safe != nullptr && fc.getResult().isDirectory())
                                        safe->addSampleFolder (fc.getResult());
                                });
}

std::optional<juce::File> Browser::sampleFromDrag (const juce::var& description)
{
    if (auto* o = description.getDynamicObject(); o != nullptr && o->hasProperty ("sampleFile"))
        if (const auto path = o->getProperty ("sampleFile").toString(); juce::File::isAbsolutePath (path))
            return juce::File (path);
    return std::nullopt;
}

void Browser::toggleOpen (const juce::String& key)
{
    if (closed.contains (key))
        closed.erase (key);
    else
        closed.insert (key);
    rebuildRows();
}

void Browser::useRow (int row)
{
    if (juce::isPositiveAndBelow (row, (int)rows.size()) && rows[(size_t)row].kind == RowKind::project)
    {
        const auto& r = rows[(size_t)row];
        if (r.key == "demo")
        {
            if (onOpenDemo)
                onOpenDemo (juce::File (r.path));
        }
        else if (! r.available)
        {
            status.setText (r.text + " is not there any more. Right-click it to remove it from the list.",
                            juce::dontSendNotification);
        }
        else if (onOpenProject)
        {
            onOpenProject (juce::File (r.path));
        }
        return;
    }
    if (juce::isPositiveAndBelow (row, (int)rows.size()) && rows[(size_t)row].kind == RowKind::sample)
    {
        session.getSamplePreview().stop();
        if (onUseSample)
            onUseSample (juce::File (rows[(size_t)row].path));
        return;
    }
    if (! juce::isPositiveAndBelow (row, (int)rows.size()) || rows[(size_t)row].kind != RowKind::device)
        return;
    const auto& entry = devices.getEntries()[(size_t)rows[(size_t)row].entry];
    if (! entry.available)
    {
        if (onDownload)
            onDownload (entry.pack);
    }
    else if (onUse)
    {
        onUse (entry);
    }
}

//==============================================================================
void Browser::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, (int)rows.size()))
        return;

    const auto& theme = themeOf (*this);
    const auto& r = rows[(size_t)row];
    auto area = juce::Rectangle<int> (0, 0, width, height);

    switch (r.kind)
    {
    case RowKind::section:
    case RowKind::group:
    {
        const bool section = r.kind == RowKind::section;
        const bool open = search.getText().trim().isNotEmpty() || ! closed.contains (r.key);
        area.removeFromLeft (section ? 6 : 16);
        g.setColour (theme.get (section ? "text" : "textDim"));
        g.setFont (juce::FontOptions (section ? 13.0f : 12.0f, juce::Font::bold));
        g.drawText (open ? juce::CharPointer_UTF8 ("\xe2\x96\xbe") : juce::CharPointer_UTF8 ("\xe2\x96\xb8"), // ▾ ▸
                    area.removeFromLeft (14), juce::Justification::centredLeft);
        if (r.note.isNotEmpty())
        {
            g.setColour (r.note == "missing" ? theme.get ("danger") : theme.get ("textDim"));
            g.setFont (juce::FontOptions (10.5f, juce::Font::italic));
            g.drawText (r.note, area.removeFromRight (96).withTrimmedRight (4), juce::Justification::centredRight);
            g.setColour (theme.get (section ? "text" : "textDim"));
            g.setFont (juce::FontOptions (section ? 13.0f : 12.0f, juce::Font::bold));
        }
        g.drawText (r.text, area, juce::Justification::centredLeft, true);
        break;
    }
    case RowKind::project:
    {
        area.removeFromLeft (r.depth == 0 ? 18 : 30);
        const bool demo = r.key == "demo";
        g.setColour (! r.available ? theme.get ("textDim").withAlpha (0.5f)
                     : demo        ? theme.get ("accent")
                                   : theme.get ("accentAlt").brighter (0.2f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::CharPointer_UTF8 ("\xe2\x99\xab"), area.removeFromLeft (16), // ♫
                    juce::Justification::centredLeft);
        if (r.note.isNotEmpty())
        {
            g.setFont (juce::FontOptions (10.5f, r.available ? juce::Font::plain : juce::Font::italic));
            g.setColour (r.available ? theme.get ("textDim") : theme.get ("danger").withAlpha (0.8f));
            const auto noteWidth = juce::jmin (
                area.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), r.note) + 8);
            g.drawText (r.note, area.removeFromRight (noteWidth).withTrimmedRight (4),
                        juce::Justification::centredRight, true);
        }
        g.setColour (r.available ? theme.get ("text") : theme.get ("textDim").withAlpha (0.6f));
        g.setFont (juce::FontOptions (12.5f, demo ? juce::Font::bold : juce::Font::plain));
        g.drawText (r.text, area.withTrimmedRight (4), juce::Justification::centredLeft, true);
        break;
    }
    case RowKind::device:
    case RowKind::problem:
    {
        area.removeFromLeft (30);
        if (r.badge.isNotEmpty())
        {
            auto badge = area.removeFromRight (40).reduced (3, 4).toFloat();
            g.setColour (badgeColour (r.badge).withAlpha (r.kind == RowKind::problem ? 0.5f : 1.0f));
            g.fillRoundedRectangle (badge, 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.drawText (r.badge, badge, juce::Justification::centred);
        }
        if (r.note.isNotEmpty())
        {
            // The open-source project, dimmed after the name: these plugins are not made by Spinus.
            g.setFont (juce::FontOptions (10.5f));
            const auto noteWidth = juce::jmin (
                area.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), r.note) + 8);
            g.setColour (theme.get ("textDim"));
            g.drawText (r.note, area.removeFromRight (noteWidth).withTrimmedRight (4),
                        juce::Justification::centredRight, true);
        }
        g.setColour (theme.get (r.kind == RowKind::problem ? "danger" : r.available ? "text" : "textDim"));
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (r.text, area.withTrimmedRight (4), juce::Justification::centredLeft, true);
        break;
    }
    case RowKind::message:
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (11.5f, juce::Font::italic));
        g.drawText (r.text, area.withTrimmedLeft (30 + 12 * r.depth), juce::Justification::centredLeft, true);
        break;
    case RowKind::folder:
    {
        area.removeFromLeft (16 + 12 * r.depth);
        const bool open = openFolders.contains (r.path);
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (open ? juce::CharPointer_UTF8 ("\xe2\x96\xbe") : juce::CharPointer_UTF8 ("\xe2\x96\xb8"),
                    area.removeFromLeft (14), juce::Justification::centredLeft);
        if (r.note.isNotEmpty())
        {
            g.setColour (theme.get ("danger"));
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (r.note, area.removeFromRight (52).withTrimmedRight (4), juce::Justification::centredRight);
        }
        g.setColour (theme.get ("text"));
        g.setFont (juce::FontOptions (12.5f, r.depth == 0 ? juce::Font::bold : juce::Font::plain));
        g.drawText (r.text, area.withTrimmedRight (4), juce::Justification::centredLeft, true);
        break;
    }
    case RowKind::sample:
    {
        area.removeFromLeft (30 + 12 * r.depth);
        const bool playing =
            session.getSamplePreview().isPlaying() && session.getSamplePreview().getFile().getFullPathName() == r.path;
        g.setColour (playing ? theme.get ("accent") : theme.get ("textDim"));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (playing ? juce::CharPointer_UTF8 ("\xe2\x96\xb6") : juce::CharPointer_UTF8 ("\xe2\x99\xaa"), // ▶ ♪
                    area.removeFromLeft (14), juce::Justification::centredLeft);
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (9.5f));
        g.drawText (r.badge, area.removeFromRight (38).withTrimmedRight (4), juce::Justification::centredRight);
        if (r.note.isNotEmpty())
        {
            g.setFont (juce::FontOptions (10.5f));
            const auto noteWidth = juce::jmin (
                area.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), r.note) + 8);
            g.drawText (r.note, area.removeFromRight (noteWidth).withTrimmedRight (4),
                        juce::Justification::centredRight, true);
        }
        g.setColour (playing ? theme.get ("accent") : theme.get ("text"));
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (r.text, area.withTrimmedRight (4), juce::Justification::centredLeft, true);
        break;
    }
    case RowKind::action:
        g.setColour (theme.get ("accent"));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (r.text, area.withTrimmedLeft (22), juce::Justification::centredLeft, true);
        break;
    }
}

void Browser::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (row, (int)rows.size()))
        return;

    const auto& r = rows[(size_t)row];
    if (e.mods.isPopupMenu())
        showRowMenu (row);
    else if (r.kind == RowKind::section || r.kind == RowKind::group)
        toggleOpen (r.key);
    else if (r.kind == RowKind::folder)
        toggleFolder (juce::File (r.path));
    else if (r.kind == RowKind::action)
    {
        if (r.key == "addProjects")
            chooseProjectFolder();
        else
            chooseSampleFolder();
    }
    else if (r.kind == RowKind::sample)
    {
        // Click to hear it; click it again while it plays to stop.
        auto& preview = session.getSamplePreview();
        if (preview.isPlaying() && preview.getFile().getFullPathName() == r.path)
            preview.stop();
        else if (! preview.play (juce::File (r.path), dsp::SamplerProcessor::getFormats()))
            status.setText (juce::File (r.path).getFileName() + " cannot be previewed (it needs ffmpeg); drag it in to "
                                                                "use it",
                            juce::dontSendNotification);
        list.repaint();
        startTimerHz (10); // to show when it stops
    }
}

void Browser::listBoxItemDoubleClicked (int row, const juce::MouseEvent&) { useRow (row); }

juce::var Browser::getDragSourceDescription (const juce::SparseSet<int>& selected)
{
    if (selected.size() != 1)
        return {};
    const auto row = selected[0];
    if (juce::isPositiveAndBelow (row, (int)rows.size()) && rows[(size_t)row].kind == RowKind::sample)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("sampleFile", rows[(size_t)row].path);
        return juce::var (o);
    }
    if (! juce::isPositiveAndBelow (row, (int)rows.size()) || rows[(size_t)row].kind != RowKind::device ||
        ! rows[(size_t)row].available)
        return {};
    return plugins::DeviceList::toVar (devices.getEntries()[(size_t)rows[(size_t)row].entry]);
}

void Browser::showRowMenu (int row)
{
    const auto r = rows[(size_t)row];
    juce::PopupMenu menu;

    if (r.kind == RowKind::device)
    {
        const auto& entry = devices.getEntries()[(size_t)r.entry];
        menu.addSectionHeader (entry.name);
        if (entry.available)
            menu.addItem (entry.instrument ? "Add to the Channel Rack" : "Add to the selected mixer insert",
                          [this, row] { useRow (row); });
        else
            menu.addItem ("Download " + entry.maker + "...", [this, row] { useRow (row); });
        // Installed in several formats (VST3 and VST2, say): which one is listed and added.
        if (entry.formats.size() > 1)
        {
            juce::PopupMenu formats;
            for (const auto& format : entry.formats)
                formats.addItem (format, true, format == entry.format,
                                 [this, key = entry.key, format] { devices.preferFormat (key, format); });
            menu.addSubMenu ("Use the version", formats);
        }
    }
    else if (r.kind == RowKind::problem)
    {
        auto* catalog = devices.getCatalog();
        if (catalog == nullptr)
            return;
        const auto problem = catalog->getProblems().getAll()[(size_t)r.problem];
        menu.addSectionHeader (juce::File (problem.file).getFileName());
        menu.addItem ("Why: " + problem.reason, false, false, [] {});
        menu.addItem ("Try again on the next scan",
                      [this, problem]
                      {
                          if (auto* c = devices.getCatalog(); c != nullptr && ! isScanning())
                          {
                              c->getProblems().remove (problem.format, problem.file);
                              c->save();
                              rebuildRows();
                          }
                      });
    }
    else if (r.kind == RowKind::sample)
    {
        const juce::File file (r.path);
        menu.addSectionHeader (file.getFileName());
        menu.addItem ("Add to the Channel Rack", [this, row] { useRow (row); });
        menu.addItem ("Add as an audio track",
                      [this, file]
                      {
                          if (onAddSampleAsTrack)
                              onAddSampleAsTrack (file);
                      });
        menu.addItem ("Open in Beat Slicer (map its hits to keys)",
                      [this, file]
                      {
                          if (onSliceSample)
                              onSliceSample (file);
                      });
        menu.addItem ("Show in the file manager", [file] { file.revealToUser(); });
    }
    else if (r.kind == RowKind::folder)
    {
        const juce::File folder (r.path);
        menu.addSectionHeader (folder.getFileName());
        if (r.depth == 0)
            menu.addItem ("Remove from the Browser (the files stay)", [this, folder] { removeSampleFolder (folder); });
        menu.addItem ("Look again for new files",
                      [this]
                      {
                          sampleIndexValid = false;
                          rebuildRows();
                      });
        menu.addItem ("Show in the file manager", [folder] { folder.revealToUser(); });
    }
    else if (r.kind == RowKind::section && r.key == "samples")
    {
        menu.addItem ("Add a folder of samples...", [this] { chooseSampleFolder(); });
    }
    else if (r.kind == RowKind::project && r.key == "demo")
    {
        menu.addSectionHeader (r.text);
        menu.addItem ("Open (your own copy, in the Music folder)", [this, row] { useRow (row); });
    }
    else if (r.kind == RowKind::project)
    {
        const juce::File folder (r.path);
        menu.addSectionHeader (r.text);
        menu.addItem ("Open", r.available, false, [this, row] { useRow (row); });
        menu.addItem ("Show in the file manager", r.available, false,
                      [folder] { project::findProjectFile (folder).revealToUser(); });
        menu.addItem ("Remove from the list (the files stay)", [this, folder] { projects.remove (folder); });
    }
    else if (r.kind == RowKind::group && r.key.startsWith ("projects.folder:"))
    {
        const juce::File folder (r.path);
        menu.addSectionHeader (folder.getFileName());
        menu.addItem ("Look again for projects", [this] { projects.rescan(); });
        menu.addItem ("Remove from the Browser (the projects stay)",
                      [this, folder] { projects.removeFolder (folder); });
        menu.addItem ("Show in the file manager", [folder] { folder.revealToUser(); });
    }
    else if (r.kind == RowKind::section && r.key == "projects")
    {
        menu.addItem ("Add a folder of projects...", [this] { chooseProjectFolder(); });
        menu.addItem ("Look again (find new projects, check which are still there)", [this] { projects.rescan(); });
    }
    else
    {
        return;
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void Browser::mouseMove (const juce::MouseEvent& e)
{
    // The hint bar shows what the row under the mouse is.
    juce::String hint;
    const auto row = list.getRowContainingPosition (e.getEventRelativeTo (&list).x, e.getEventRelativeTo (&list).y);
    if (juce::isPositiveAndBelow (row, (int)rows.size()))
    {
        const auto& r = rows[(size_t)row];
        if (r.kind == RowKind::device)
        {
            const auto& entry = devices.getEntries()[(size_t)r.entry];
            hint = ! entry.available
                       ? entry.detail
                       : entry.detail + (entry.instrument
                                             ? ". Double-click to add a channel, or drag it to the Channel Rack"
                                             : ". Double-click to add it to the selected mixer insert, or drag "
                                               "it onto a mixer strip");
        }
        else if (r.kind == RowKind::sample)
            hint = r.path + ": click to hear it, double-click to add it to the Channel Rack, or drag it to the Channel "
                            "Rack, the Playlist or Audio tracks";
        else if (r.kind == RowKind::folder)
            hint = r.path + (r.depth == 0 ? ": click to open; right-click to remove it from the Browser" : "");
        else if (r.kind == RowKind::project && r.key == "demo")
            hint = juce::File (r.path).getFileName() +
                   ", a song made in Spinus that comes with it: double-click to open your own copy (Spinus puts it in "
                   "your Music folder)";
        else if (r.kind == RowKind::project)
            hint =
                r.path + (r.available ? juce::String (": double-click to open; right-click to show it in the file "
                                                      "manager or remove it from the list")
                                      : juce::String (": not there any more; right-click to remove it from the list"));
        else if (r.kind == RowKind::group && r.path.isNotEmpty())
            hint = r.path + ": the projects found in this folder and its subfolders; right-click to look again or "
                            "remove it";
        else if (r.kind == RowKind::problem && devices.getCatalog() != nullptr)
        {
            const auto& p = devices.getCatalog()->getProblems().getAll()[(size_t)r.problem];
            hint = p.file + ": " + p.reason + ". Right-click to try it again on the next scan";
        }
    }
    list.setHelpText (hint);
}

//==============================================================================
void Browser::startScan()
{
    auto* catalog = devices.getCatalog();
    if (catalog == nullptr)
        return;
    if (isScanning())
    {
        rescanPending = true; // the running scan may have looked at the folders before the new plugin arrived
        return;
    }
    rescanPending = false;

    scan = std::make_unique<ScanThread> (*catalog);
    scan->startThread();
    scanButton.setButtonText ("Cancel scan");
    status.setText ("Scanning...", juce::dontSendNotification);
    rebuildRows();
    startTimerHz (10);
}

void Browser::cancelScan()
{
    if (scan != nullptr)
        scan->cancel = true;
}

bool Browser::isScanning() const { return scan != nullptr; }

void Browser::finishScanForTests()
{
    if (scan == nullptr)
        return;
    scan->waitForThreadToExit (-1);
    scanFinished();
}

void Browser::timerCallback()
{
    if (scan == nullptr)
    {
        // A preview playing: its row shows it until it ends.
        list.repaint();
        if (! session.getSamplePreview().isPlaying())
            stopTimer();
        return;
    }

    if (scan->done)
    {
        scanFinished();
        return;
    }

    const auto p = scan->getProgress();
    if (p.total > 0)
        status.setText ("Scanning " + juce::String (juce::jmin (p.done + 1, p.total)) + " of " +
                            juce::String (p.total) + ": " + juce::File (p.currentFile).getFileName(),
                        juce::dontSendNotification);
}

void Browser::scanFinished()
{
    stopTimer();
    if (scan == nullptr)
        return;

    scan->waitForThreadToExit (-1);
    const auto summary = scan->summary;
    const bool cancelled = scan->cancel;
    auto& catalog = scan->catalog;
    scan = nullptr;

    // Plugins found can now be created by the engine (e.g. in projects that use them).
    auto& pluginManager = session.getEdit().engine.getPluginManager();
    plugins::PluginCatalog::refreshFormats (pluginManager.pluginFormatManager);
    plugins::forgetVanishedPlugins (pluginManager.knownPluginList); // removed plugins leave the list too
    catalog.addKnownTypesTo (pluginManager.knownPluginList);
    devices.refresh();

    scanButton.setButtonText ("Scan plugins");
    status.setText ({}, juce::dontSendNotification);
    rebuildRows();

    // What the list shows: one entry per plugin (a plugin in several formats, or with many parts inside one
    // file, is counted as the list shows it), not every description the scanner read.
    juce::String text = cancelled ? "Scan cancelled. " : "Scan finished: ";
    text << countInstalled() << " installed plugins";
    if (summary.newProblems > 0)
        text << ", " << summary.newProblems << " could not be loaded"; // listed under Problem plugins
    status.setText (text + ".", juce::dontSendNotification);

    // Asked to scan while this one ran: again, for what arrived meanwhile (only new files are read).
    if (rescanPending && ! cancelled)
        startScan();
    rescanPending = false;
}

int Browser::countInstalled() const
{
    const auto& entries = devices.getEntries();
    return (int)std::count_if (entries.begin(), entries.end(),
                               [] (auto& e) { return e.origin == plugins::DeviceList::Origin::installed; });
}

//==============================================================================
void Browser::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    g.setColour (theme.get ("panelHeader"));
    g.fillRect (getLocalBounds().removeFromTop (headerHeight));
    g.setColour (theme.get ("text"));
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("Browser", getLocalBounds().removeFromTop (headerHeight).reduced (8, 0),
                juce::Justification::centredLeft);
    g.setColour (theme.get ("border"));
    g.drawRect (getLocalBounds());
    g.fillRect (getLocalBounds().removeFromBottom (footerHeight).removeFromTop (1));
    status.setColour (juce::Label::textColourId, theme.get ("textDim"));
}

void Browser::resized()
{
    auto area = getLocalBounds().reduced (1);
    area.removeFromTop (headerHeight);
    search.setBounds (area.removeFromTop (searchHeight).reduced (6, 2));

    auto footer = area.removeFromBottom (footerHeight);
    scanButton.setBounds (footer.removeFromTop (28).reduced (6, 3).removeFromLeft (110));
    status.setBounds (footer.reduced (4, 0));

    list.setBounds (area.reduced (0, 2));
}

} // namespace spinus::ui
