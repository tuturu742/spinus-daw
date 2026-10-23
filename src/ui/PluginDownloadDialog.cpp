#include "PluginDownloadDialog.h"

#include "Theme.h"
#include "WindowStacking.h"

#include "model/StockDevices.h"
#include "plugins/clap/ClapPluginFormat.h"

namespace spinus::ui
{

using plugins::DownloadPack;
using plugins::PluginDownloads;

namespace
{
constexpr int rowHeight = 70, margin = 14;

juce::String megabytes (juce::int64 bytes) { return juce::String (juce::roundToInt ((double)bytes / 1.0e6)) + " MB"; }
} // namespace

//==============================================================================
/** Installs the chosen packs one after another; the dialog polls its state. */
class PluginDownloadDialog::DownloadThread final : public juce::Thread
{
public:
    DownloadThread (const PluginDownloads& d, std::vector<const DownloadPack*> p)
        : juce::Thread ("Spinus plugin downloads"), downloads (d), packs (std::move (p))
    {
    }

    ~DownloadThread() override
    {
        cancel = true;
        stopThread (-1);
    }

    void run() override
    {
        for (size_t i = 0; i < packs.size() && ! cancel; ++i)
        {
            {
                const juce::ScopedLock sl (lock);
                current = i;
            }
            auto result = downloads.install (
                *packs[i],
                [this] (const PluginDownloads::Progress& p)
                {
                    const juce::ScopedLock sl (lock);
                    progress = p;
                },
                &cancel);

            const juce::ScopedLock sl (lock);
            results.emplace_back (packs[i]->id, result);
        }
        done = true;
    }

    struct State
    {
        size_t current = 0;
        PluginDownloads::Progress progress;
        std::vector<std::pair<juce::String, juce::Result>> results;
    };

    State getState() const
    {
        const juce::ScopedLock sl (lock);
        return { current, progress, results };
    }

    const PluginDownloads& downloads;
    const std::vector<const DownloadPack*> packs;
    std::atomic<bool> cancel { false }, done { false };

private:
    juce::CriticalSection lock;
    size_t current = 0;
    PluginDownloads::Progress progress;
    std::vector<std::pair<juce::String, juce::Result>> results;
};

//==============================================================================
struct PluginDownloadDialog::PackRow
{
    explicit PackRow (const DownloadPack& p) : pack (p)
    {
        tick.setButtonText (pack.name);
        description.setText (pack.description, juce::dontSendNotification);
        details.setText (megabytes (pack.getDownloadSize()) + "  -  " + pack.license + "  -  by the " + pack.name +
                             " project",
                         juce::dontSendNotification);
        website.setButtonText (pack.homepage.fromFirstOccurrenceOf ("://", false, false));
        website.setURL (juce::URL (pack.homepage));
        website.setJustificationType (juce::Justification::centredLeft);
        website.setFont (juce::FontOptions (12.0f), false, juce::Justification::centredLeft);
        for (auto* l : { &description, &details, &state })
            l->setFont (juce::FontOptions (12.0f));
        state.setJustificationType (juce::Justification::centredRight);
    }

    /** Remove's steps: the row shows its description, asks to confirm, or says why the pack stays. */
    enum class Removing
    {
        no,
        asking,
        refused
    };

    void setRemoving (Removing r, const juce::String& message = {})
    {
        removing = r;
        description.setText (r == Removing::no ? pack.description : message, juce::dontSendNotification);
        description.setJustificationType (r == Removing::no ? juce::Justification::centredLeft
                                                            : juce::Justification::topLeft);
        keepButton.setButtonText (r == Removing::refused ? "OK" : "Keep");
    }

    const DownloadPack& pack;
    juce::ToggleButton tick;
    juce::Label description, details, state;
    juce::HyperlinkButton website;
    juce::TextButton removeButton { "Remove" }, keepButton { "Keep" };
    Removing removing = Removing::no;
};

//==============================================================================
PluginDownloadDialog::PluginDownloadDialog (const PluginDownloads& d, std::vector<juce::File> folders,
                                            const juce::String& onlyPack, std::vector<DownloadPack> p)
    : downloads (d), searchFolders (std::move (folders)), packs (std::move (p))
{
    intro.setText ("Spinus works with these free, open-source plugins. They are made by other projects and "
                   "downloaded from those projects' own websites. You can get them now or later, from "
                   "Options > Get free plugins.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    intro.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (intro);

    for (auto& pack : packs)
    {
        auto row = std::make_unique<PackRow> (pack);
        row->tick.onClick = [this] { refreshRows(); };
        const auto id = pack.id;
        row->removeButton.onClick = [this, id]
        {
            if (isAskingToRemove (id))
                confirmRemove (id);
            else
                askToRemove (id);
        };
        row->keepButton.onClick = [this, id] { keepPack (id); };
        for (juce::Component* c : std::initializer_list<juce::Component*> { &row->tick, &row->description,
                                                                            &row->details, &row->website, &row->state })
            addAndMakeVisible (c);
        addChildComponent (row->removeButton);
        addChildComponent (row->keepButton);
        packRows.push_back (std::move (row));
    }

    refreshRows();
    // Tick what is missing (or only the packs asked for).
    const auto asked = juce::StringArray::fromTokens (onlyPack, ",", {});
    for (auto& row : packRows)
        if (row->tick.isEnabled())
            row->tick.setToggleState (onlyPack.isEmpty() || asked.contains (row->pack.id), juce::dontSendNotification);
    refreshRows();

    status.setJustificationType (juce::Justification::centredLeft);
    status.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (status);
    addChildComponent (progressBar);

    downloadButton.onClick = [this]
    {
        if (isDownloading())
            cancelDownloads();
        else
            startDownloads();
    };
    closeButton.onClick = [this]
    {
        if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
            window->closeButtonPressed();
    };
    addAndMakeVisible (downloadButton);
    addAndMakeVisible (closeButton);

    setSize (560, 110 + (int)packRows.size() * rowHeight + 70);
}

PluginDownloadDialog::~PluginDownloadDialog() { thread = nullptr; } // cancels and waits

namespace
{
/** The dialog's own window: not modal, so Spinus can be used while plugins download. Closing it
    cancels a running download. */
class DownloadWindow final : public juce::DocumentWindow
{
public:
    DownloadWindow (PluginDownloadDialog* dialog, juce::Component* owner)
        : DocumentWindow ("Get free plugins", juce::Colour (0xff1e2227), DocumentWindow::closeButton)
    {
        if (owner != nullptr)
            if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&owner->getLookAndFeel()))
            {
                setBackgroundColour (lf->getTheme().get ("panel"));
                dialog->setLookAndFeel (lf);
            }
        setUsingNativeTitleBar (true);
        setContentOwned (dialog, true);
        setResizable (false, false);
        if (owner != nullptr)
            centreAroundComponent (owner->getTopLevelComponent(), getWidth(), getHeight());
        else
            centreWithSize (getWidth(), getHeight());
        setVisible (true);
        if (owner != nullptr)
            keepAboveOwner (*this, *owner->getTopLevelComponent());
    }

    ~DownloadWindow() override
    {
        if (auto* content = getContentComponent())
            content->setLookAndFeel (nullptr);
        clearContentComponent();
    }

    PluginDownloadDialog& getDialog() { return *static_cast<PluginDownloadDialog*> (getContentComponent()); }

    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync ([safe = SafePointer<DownloadWindow> (this)] { delete safe.getComponent(); });
    }
};

juce::Component::SafePointer<DownloadWindow> openWindow;
} // namespace

void PluginDownloadDialog::show (const PluginDownloads& downloads, std::vector<juce::File> searchFolders,
                                 juce::Component* centreAround, const juce::String& onlyPack,
                                 std::function<void()> onChanged,
                                 std::function<juce::String (const DownloadPack&)> checkRemove)
{
    // Already open: bring it forward (with the asked-for pack ticked).
    if (openWindow != nullptr)
    {
        if (onlyPack.isNotEmpty() && ! openWindow->getDialog().isDownloading())
            for (const auto& pack : juce::StringArray::fromTokens (onlyPack, ",", {}))
                openWindow->getDialog().setSelected (pack, true);
        openWindow->toFront (true);
        return;
    }

    auto* dialog = new PluginDownloadDialog (downloads, std::move (searchFolders), onlyPack);
    dialog->onInstalled = onChanged;
    dialog->onRemoved = std::move (onChanged);
    dialog->checkRemove = std::move (checkRemove);
    openWindow = new DownloadWindow (dialog, centreAround);
}

void PluginDownloadDialog::closeWindow() { delete openWindow.getComponent(); }

//==============================================================================
PluginDownloadDialog::PackRow* PluginDownloadDialog::findRow (const juce::String& pack) const
{
    for (auto& row : packRows)
        if (row->pack.id == pack)
            return row.get();
    return nullptr;
}

void PluginDownloadDialog::setSelected (const juce::String& pack, bool selected)
{
    if (auto* row = findRow (pack); row != nullptr && row->tick.isEnabled())
        row->tick.setToggleState (selected, juce::dontSendNotification);
    refreshRows();
}

bool PluginDownloadDialog::isSelected (const juce::String& pack) const
{
    auto* row = findRow (pack);
    return row != nullptr && row->tick.getToggleState();
}

juce::String PluginDownloadDialog::getPackStatus (const juce::String& pack) const
{
    auto* row = findRow (pack);
    return row != nullptr ? row->state.getText() : juce::String();
}

void PluginDownloadDialog::refreshRows()
{
    const bool busy = isDownloading();
    juce::int64 selectedBytes = 0;

    for (auto& row : packRows)
    {
        const bool installed = downloads.isInstalled (row->pack);
        const bool onSystem =
            ! installed && std::all_of (row->pack.files.begin(), row->pack.files.end(), [this] (auto& f)
                                        { return PluginDownloads::findFile (searchFolders, f) != juce::File(); });
        if (installed || onSystem)
        {
            row->tick.setToggleState (false, juce::dontSendNotification);
            row->state.setText (installed ? "Downloaded" : "Installed on this computer", juce::dontSendNotification);
        }
        else if (! busy && row->state.getText().isEmpty())
        {
            row->state.setText ("Not downloaded", juce::dontSendNotification);
        }
        row->tick.setEnabled (! busy && ! installed && ! onSystem);
        // Remove: only what Spinus downloaded (not what the system installed), and not while downloading.
        // (A refusal or a removal's problem stays until OK, even once the pack is gone.)
        if (busy || (! installed && row->removing == PackRow::Removing::asking))
            row->setRemoving (PackRow::Removing::no);
        row->removeButton.setVisible (! busy && installed && row->removing != PackRow::Removing::refused);
        row->keepButton.setVisible (! busy && row->removing != PackRow::Removing::no);
        row->state.setVisible (row->removing != PackRow::Removing::asking);
        row->details.setVisible (row->removing == PackRow::Removing::no);
        row->website.setVisible (row->removing == PackRow::Removing::no);
        if (row->tick.getToggleState())
            selectedBytes += row->pack.getDownloadSize();
    }

    downloadButton.setButtonText (busy ? "Cancel" : "Download");
    downloadButton.setEnabled (busy || selectedBytes > 0);
    progressBar.setVisible (busy);
    status.setVisible (! busy);
    if (! busy && status.getText().isEmpty() && selectedBytes > 0)
        status.setText ("Download " + megabytes (selectedBytes) + ".", juce::dontSendNotification);
    resized(); // the rows' buttons come and go
}

//==============================================================================
juce::String PluginDownloadDialog::describeUseInProject (const model::Project& project, const DownloadPack& pack,
                                                         const juce::File& packFolder)
{
    const auto uses = [&] (const model::Device& device)
    {
        if (device.kind == model::Device::Kind::stock)
        {
            const auto* bundled = model::findBundledDevice (device.stockType);
            return bundled != nullptr && pack.id == bundled->pack;
        }
        return juce::File::isAbsolutePath (device.plugin.fileOrIdentifier) &&
               juce::File (device.plugin.fileOrIdentifier).isAChildOf (packFolder);
    };

    juce::StringArray channels, inserts;
    for (auto& channel : project.channels)
        if (channel.instrument && uses (*channel.instrument))
            channels.add (channel.name);
    const auto checkInsert = [&] (const model::Insert& insert)
    {
        if (std::any_of (insert.effects.begin(), insert.effects.end(), [&] (auto& slot) { return uses (slot.device); }))
            inserts.add (insert.name);
    };
    checkInsert (project.mixer.master);
    for (auto& insert : project.mixer.inserts)
        checkInsert (insert);

    const auto describe = [] (const juce::StringArray& names, const juce::String& one, const juce::String& many)
    {
        if (names.isEmpty())
            return juce::String();
        auto text = juce::String (names.size()) + " " + (names.size() == 1 ? one : many) + " (";
        text << names.joinIntoString (", ", 0, juce::jmin (3, names.size()));
        if (names.size() > 3)
            text << ", ...";
        return text + ")";
    };
    juce::StringArray parts;
    parts.add (describe (channels, "channel", "channels"));
    parts.add (describe (inserts, "mixer insert", "mixer inserts"));
    parts.removeEmptyStrings();
    if (parts.isEmpty())
        return {};
    return pack.name + " is used by " + parts.joinIntoString (" and ") +
           " in this project. Take it out of them first (or delete them), then remove it.";
}

bool PluginDownloadDialog::isLoadedFrom (const juce::File& folder)
{
    const auto files = plugins::ClapPluginFormat::getLoadedFiles();
    return std::any_of (files.begin(), files.end(), [&] (auto& path) { return juce::File (path).isAChildOf (folder); });
}

juce::String PluginDownloadDialog::whyNotRemovable (const DownloadPack& pack) const
{
    if (checkRemove)
        if (auto why = checkRemove (pack); why.isNotEmpty())
            return why;
    if (isLoadedFrom (downloads.getPackFolder (pack)))
        return pack.name + " is still running (the Analyzer may be playing it, or a channel that used it is "
                           "still closing). Wait for it to finish and try again.";
    return {};
}

bool PluginDownloadDialog::canRemove (const juce::String& pack) const
{
    auto* row = findRow (pack);
    return row != nullptr && ! isDownloading() && downloads.isInstalled (row->pack);
}

void PluginDownloadDialog::askToRemove (const juce::String& pack)
{
    auto* row = findRow (pack);
    if (row == nullptr || ! canRemove (pack))
        return;
    if (const auto why = whyNotRemovable (row->pack); why.isNotEmpty())
        row->setRemoving (PackRow::Removing::refused, why);
    else
        row->setRemoving (PackRow::Removing::asking, "Remove " + row->pack.name +
                                                         "? Its files are deleted from this computer. Projects that "
                                                         "use it will show it as missing until you download it again.");
    refreshRows();
}

bool PluginDownloadDialog::isAskingToRemove (const juce::String& pack) const
{
    auto* row = findRow (pack);
    return row != nullptr && row->removing == PackRow::Removing::asking;
}

juce::String PluginDownloadDialog::getRowMessage (const juce::String& pack) const
{
    auto* row = findRow (pack);
    return row != nullptr && row->removing != PackRow::Removing::no ? row->description.getText() : juce::String();
}

void PluginDownloadDialog::keepPack (const juce::String& pack)
{
    if (auto* row = findRow (pack))
        row->setRemoving (PackRow::Removing::no);
    refreshRows();
}

void PluginDownloadDialog::confirmRemove (const juce::String& pack)
{
    auto* row = findRow (pack);
    if (row == nullptr || ! isAskingToRemove (pack) || ! canRemove (pack))
        return;

    // Things may have changed since the question (a channel added, the Analyzer started): check again.
    if (const auto why = whyNotRemovable (row->pack); why.isNotEmpty())
    {
        row->setRemoving (PackRow::Removing::refused, why);
        refreshRows();
        return;
    }

    const auto result = downloads.remove (row->pack);
    const bool removed = ! downloads.isInstalled (row->pack);
    // A problem (files in use, or left behind) is told in the row, where there is room for it.
    if (result.wasOk())
        row->setRemoving (PackRow::Removing::no);
    else
        row->setRemoving (PackRow::Removing::refused, result.getErrorMessage());
    if (removed)
        row->state.setText ({}, juce::dontSendNotification); // shown again as Not downloaded, or as on the system
    status.setText (removed ? "Removed " + row->pack.name + "." : "Could not remove " + row->pack.name + ".",
                    juce::dontSendNotification);
    refreshRows();
    if (removed && onRemoved)
        onRemoved();
}

//==============================================================================
void PluginDownloadDialog::startDownloads()
{
    if (isDownloading())
        return;

    std::vector<const DownloadPack*> chosen;
    for (auto& row : packRows)
        if (row->tick.getToggleState() && row->tick.isEnabled())
        {
            chosen.push_back (&row->pack);
            row->state.setText ("Waiting", juce::dontSendNotification);
        }
    if (chosen.empty())
        return;

    progress = 0.0;
    reportedInstalls = 0;
    status.setText ({}, juce::dontSendNotification);
    thread = std::make_unique<DownloadThread> (downloads, std::move (chosen));
    thread->startThread();
    refreshRows();
    startTimerHz (10);
}

void PluginDownloadDialog::cancelDownloads()
{
    if (thread != nullptr)
        thread->cancel = true;
}

bool PluginDownloadDialog::isDownloading() const { return thread != nullptr; }

void PluginDownloadDialog::finishForTests()
{
    if (thread != nullptr)
        thread->waitForThreadToExit (-1);
    timerCallback();
}

void PluginDownloadDialog::timerCallback()
{
    if (thread == nullptr)
    {
        stopTimer();
        return;
    }

    const auto state = thread->getState();
    for (size_t i = 0; i < thread->packs.size(); ++i)
        if (auto* row = findRow (thread->packs[i]->id);
            row != nullptr && i == state.current && state.results.size() <= i)
        {
            const auto& p = state.progress;
            row->state.setText (
                p.stage == "Downloading" && p.total > 0
                    ? "Downloading " + juce::String (juce::roundToInt (100.0 * (double)p.done / (double)p.total)) + "%"
                    : p.stage,
                juce::dontSendNotification);
            progress = p.total > 0 ? (double)p.done / (double)p.total : -1.0;
            progressBar.setTextToDisplay (row->pack.name + ": " + row->state.getText());
        }

    for (auto& [id, result] : state.results)
        if (auto* row = findRow (id))
            row->state.setText (result.wasOk()                             ? "Downloaded"
                                : result.getErrorMessage() == "Cancelled." ? "Not downloaded"
                                                                           : "Failed",
                                juce::dontSendNotification);

    // Make each installed pack usable at once.
    for (; reportedInstalls < state.results.size(); ++reportedInstalls)
        if (state.results[reportedInstalls].second.wasOk() && onInstalled)
            onInstalled();

    if (thread->done)
        finished();
}

void PluginDownloadDialog::finished()
{
    stopTimer();
    const auto state = thread->getState();
    const bool cancelled = thread->cancel;
    // Packs the download did not reach.
    for (size_t i = state.results.size(); i < thread->packs.size(); ++i)
        if (auto* row = findRow (thread->packs[i]->id))
            row->state.setText ("Not downloaded", juce::dontSendNotification);
    thread = nullptr;

    juce::StringArray problems;
    int installed = 0;
    for (auto& [id, result] : state.results)
    {
        if (result.wasOk())
            ++installed;
        else if (result.getErrorMessage() != "Cancelled.")
            problems.add (result.getErrorMessage());
    }

    juce::String text;
    if (cancelled)
        text = "Cancelled. ";
    if (installed > 0)
        text << "Downloaded " << installed << (installed == 1 ? " plugin set" : " plugin sets")
             << ": they are in the Browser now. ";
    if (! problems.isEmpty())
        text << problems.joinIntoString (" ");
    status.setText (text.trim(), juce::dontSendNotification);
    refreshRows();
}

//==============================================================================
void PluginDownloadDialog::paint (juce::Graphics& g)
{
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&getLookAndFeel());
    const auto theme = lf != nullptr ? lf->getTheme() : Theme::dark();
    g.fillAll (theme.get ("panel"));
    g.setColour (theme.get ("border"));
    for (size_t i = 1; i < packRows.size(); ++i)
        g.fillRect (margin, packRows[i]->tick.getY() - 6, getWidth() - 2 * margin, 1);
}

void PluginDownloadDialog::layOutRow (PackRow& row, juce::Rectangle<int> r)
{
    auto top = r.removeFromTop (24);
    if (row.removeButton.isVisible())
    {
        row.removeButton.setBounds (top.removeFromRight (76).reduced (0, 1));
        top.removeFromRight (6);
    }
    if (row.keepButton.isVisible())
    {
        row.keepButton.setBounds (top.removeFromRight (66).reduced (0, 1));
        top.removeFromRight (6);
    }
    if (row.state.isVisible())
        row.state.setBounds (top.removeFromRight (150));
    row.tick.setBounds (top);
    r.removeFromLeft (26);
    if (row.removing != PackRow::Removing::no)
    {
        // The question or refusal: up to three lines where the description and details were.
        row.description.setBounds (r);
        return;
    }
    row.description.setBounds (r.removeFromTop (18));
    auto bottom = r.removeFromTop (18);
    row.website.setBounds (bottom.removeFromRight (250));
    row.details.setBounds (bottom);
}

void PluginDownloadDialog::resized()
{
    auto area = getLocalBounds().reduced (margin);
    intro.setBounds (area.removeFromTop (60));
    area.removeFromTop (10);

    for (auto& row : packRows)
        layOutRow (*row, area.removeFromTop (rowHeight));

    auto buttons = getLocalBounds().reduced (margin).removeFromBottom (28);
    closeButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    downloadButton.setBounds (buttons.removeFromRight (110));
    buttons.removeFromRight (12);
    status.setBounds (buttons);
    progressBar.setBounds (buttons.reduced (0, 3));
}

} // namespace spinus::ui
