#include "SoundIndexWindow.h"

#include "LearnInstrumentWindow.h"

#include "Theme.h"
#include "WindowStacking.h"

namespace spinus::ui
{

namespace
{
const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::String count (int n, const char* one, const char* many) { return juce::String (n) + " " + (n == 1 ? one : many); }
} // namespace

//==============================================================================
/** The Instruments tab: the list, a tick box on each row, and buttons for the selected one. */
class SoundIndexPage::InstrumentList final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit InstrumentList (SoundIndexPage& p) : page (p)
    {
        intro.setText ("Spinus's own instruments and the free ones it downloads are always measured. Tick which of "
                       "your other plugins it measures too (it loads none of them unless you tick them). Select an "
                       "instrument to measure it again, forget its sounds or see them.",
                       juce::dontSendNotification);
        intro.setJustificationType (juce::Justification::topLeft);
        list.setModel (this);
        list.setRowHeight (48);
        againButton.setTooltip ("Forget what was measured of it and measure all its presets again");
        againButton.onClick = [this] { page.measureAgain (list.getSelectedRow()); };
        forgetButton.setTooltip ("Its sounds leave the index, and it is unticked (Spinus's own come back at the next "
                                 "update)");
        forgetButton.onClick = [this] { page.forget (list.getSelectedRow()); };
        showButton.onClick = [this] { page.showSoundsOf (list.getSelectedRow()); };
        learnButton.setTooltip ("Which of its keys play notes, and which switch what it plays or play effects: "
                                "Spinus then measures it on the keys it plays");
        learnButton.onClick = [this] { page.learn (list.getSelectedRow()); };
        for (auto* c : std::initializer_list<juce::Component*> { &intro, &list, &againButton, &forgetButton,
                                                                 &showButton, &learnButton })
            addAndMakeVisible (c);
        updateButtons();
    }

    void refresh()
    {
        list.updateContent();
        list.repaint();
        updateButtons();
    }

    void updateButtons()
    {
        const auto row = list.getSelectedRow();
        const bool chosen = juce::isPositiveAndBelow (row, (int)page.instruments.size());
        const bool busy = page.index.isUpdating();
        againButton.setEnabled (chosen && ! busy && page.instruments[(size_t)row].installed);
        forgetButton.setEnabled (chosen && ! busy);
        showButton.setEnabled (chosen);
        learnButton.setEnabled (chosen && page.instruments[(size_t)row].installed);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        intro.setBounds (area.removeFromTop (40));
        auto buttons = area.removeFromBottom (28);
        againButton.setBounds (buttons.removeFromLeft (130).reduced (0, 1));
        buttons.removeFromLeft (6);
        forgetButton.setBounds (buttons.removeFromLeft (140).reduced (0, 1));
        buttons.removeFromLeft (6);
        showButton.setBounds (buttons.removeFromLeft (130).reduced (0, 1));
        buttons.removeFromLeft (6);
        learnButton.setBounds (buttons.removeFromLeft (170).reduced (0, 1));
        area.removeFromBottom (6);
        list.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panel"));
        intro.setColour (juce::Label::textColourId, theme.get ("textDim"));
        list.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
    }

private:
    static constexpr int tickWidth = 28;

    int getNumRows() override { return (int)page.instruments.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int)page.instruments.size()))
            return;
        const auto& theme = themeOf (*this);
        const auto& r = page.instruments[(size_t)row];
        if (selected)
            g.fillAll (theme.get ("accent").withAlpha (0.25f));
        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (6, 2);
        auto tick = area.removeFromLeft (tickWidth).withSizeKeepingCentre (14, 14).toFloat();
        g.setColour (theme.get (r.always ? "textDim" : "text"));
        g.drawRoundedRectangle (tick, 2.0f, 1.0f);
        if (r.measured)
        {
            juce::Path p;
            p.startNewSubPath (tick.getX() + 3.0f, tick.getCentreY());
            p.lineTo (tick.getX() + 6.0f, tick.getBottom() - 3.0f);
            p.lineTo (tick.getRight() - 3.0f, tick.getY() + 3.0f);
            g.strokePath (p, juce::PathStrokeType (1.8f));
        }
        auto top = area.removeFromTop (18);
        g.setFont (juce::FontOptions (13.5f));
        g.setColour (theme.get (r.installed ? "text" : "textDim"));
        g.drawFittedText (r.source.name, top.removeFromLeft (top.getWidth() * 60 / 100),
                          juce::Justification::centredLeft, 1);
        g.setColour (theme.get ("textDim"));
        g.drawFittedText (r.source.badge + (r.always ? ", always measured" : juce::String()), top,
                          juce::Justification::centredRight, 1);
        g.setFont (juce::FontOptions (12.5f));
        g.setColour (r.problem || r.hint ? theme.get ("accent") : theme.get ("textDim"));
        g.drawFittedText (r.state, area, juce::Justification::topLeft, 2);
    }

    juce::String getTooltipForRow (int row) override
    {
        return juce::isPositiveAndBelow (row, (int)page.instruments.size()) ? page.instruments[(size_t)row].state
                                                                            : juce::String();
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            list.selectRow (row);
            juce::PopupMenu menu;
            const auto& r = page.instruments[(size_t)row];
            menu.addSectionHeader (r.source.name);
            if (! r.always && r.installed)
                menu.addItem (r.measured ? "Don't measure it" : "Measure it too", ! page.index.isUpdating(), false,
                              [this, row] { page.toggle (row); });
            menu.addItem ("Measure again", r.installed && ! page.index.isUpdating(), false,
                          [this, row] { page.measureAgain (row); });
            menu.addItem ("Forget its sounds", ! page.index.isUpdating(), false, [this, row] { page.forget (row); });
            menu.addItem ("Show its sounds", [this, row] { page.showSoundsOf (row); });
            menu.addItem ("Learn this instrument...", r.installed, false, [this, row] { page.learn (row); });
            menu.showMenuAsync (juce::PopupMenu::Options());
            return;
        }
        if (e.x < tickWidth + 6)
            page.toggle (row);
    }

    void selectedRowsChanged (int) override { updateButtons(); }

    SoundIndexPage& page;
    juce::Label intro;
    juce::ListBox list;
    juce::TextButton againButton { "Measure again" }, forgetButton { "Forget its sounds" },
        showButton { "Show its sounds" }, learnButton { "Learn this instrument..." };
};

//==============================================================================
/** The Samples tab: the Browser's sample folders. */
class SoundIndexPage::FolderList final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit FolderList (SoundIndexPage& p) : page (p)
    {
        intro.setText ("The sound index measures the samples in the folders of the Browser's Samples list, and the "
                       "folders inside them. Add or remove a folder there (right-click Samples). Only new or "
                       "changed files are measured at each update; Measure again measures a whole folder anew.",
                       juce::dontSendNotification);
        intro.setJustificationType (juce::Justification::topLeft);
        list.setModel (this);
        list.setRowHeight (34);
        againButton.onClick = [this] { page.measureFolderAgain (list.getSelectedRow()); };
        showButton.onClick = [this] { page.showSamplesOf (list.getSelectedRow(), false); };
        problemsButton.setTooltip ("What went wrong with this folder's samples, and why");
        problemsButton.onClick = [this] { page.showSamplesOf (list.getSelectedRow(), true); };
        for (auto* c :
             std::initializer_list<juce::Component*> { &intro, &list, &againButton, &showButton, &problemsButton })
            addAndMakeVisible (c);
        updateButtons();
    }

    void refresh()
    {
        list.updateContent();
        list.repaint();
        updateButtons();
    }

    void updateButtons()
    {
        const auto row = list.getSelectedRow();
        const bool chosen = juce::isPositiveAndBelow (row, (int)page.folders.size());
        againButton.setEnabled (chosen);
        showButton.setEnabled (chosen && page.folders[(size_t)row].samples > 0);
        problemsButton.setEnabled (chosen && page.folders[(size_t)row].problems > 0);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        intro.setBounds (area.removeFromTop (40));
        auto buttons = area.removeFromBottom (28);
        againButton.setBounds (buttons.removeFromLeft (130).reduced (0, 1));
        buttons.removeFromLeft (6);
        showButton.setBounds (buttons.removeFromLeft (140).reduced (0, 1));
        buttons.removeFromLeft (6);
        problemsButton.setBounds (buttons.removeFromLeft (140).reduced (0, 1));
        area.removeFromBottom (6);
        list.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panel"));
        intro.setColour (juce::Label::textColourId, theme.get ("textDim"));
        list.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
    }

private:
    int getNumRows() override { return juce::jmax (1, (int)page.folders.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        const auto& theme = themeOf (*this);
        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (8, 2);
        if (page.folders.empty())
        {
            g.setColour (theme.get ("textDim"));
            g.setFont (juce::FontOptions (13.0f));
            g.drawFittedText ("No sample folders yet: add one in the Browser (right-click Samples).", area,
                              juce::Justification::centredLeft, 1);
            return;
        }
        if (! juce::isPositiveAndBelow (row, (int)page.folders.size()))
            return;
        const auto& r = page.folders[(size_t)row];
        if (selected)
            g.fillAll (theme.get ("accent").withAlpha (0.25f));
        auto top = area.removeFromTop (area.getHeight() / 2);
        g.setFont (juce::FontOptions (13.5f));
        g.setColour (theme.get ("text"));
        g.drawFittedText (r.folder.getFileName(), top.removeFromLeft (top.getWidth() / 2),
                          juce::Justification::centredLeft, 1);
        g.setColour (r.problems > 0 ? theme.get ("accent") : theme.get ("textDim"));
        g.drawFittedText (count (r.samples, "sample measured", "samples measured") +
                              (r.problems > 0 ? ", " + count (r.problems, "problem", "problems") : juce::String()),
                          top, juce::Justification::centredRight, 1);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (theme.get ("textDim"));
        g.drawFittedText (r.folder.getFullPathName(), area, juce::Justification::centredLeft, 1);
    }

    void selectedRowsChanged (int) override { updateButtons(); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { page.showSamplesOf (row, false); }

    SoundIndexPage& page;
    juce::Label intro;
    juce::ListBox list;
    juce::TextButton againButton { "Measure again" }, showButton { "Show its samples" },
        problemsButton { "Show its problems" };
};

//==============================================================================
SoundIndexPage::SoundIndexPage (SoundIndexService& service) : index (service)
{
    status.setJustificationType (juce::Justification::centredLeft);
    status.setMinimumHorizontalScale (1.0f);
    updateButton.setTooltip ("Measure what is new or changed: instruments, presets and sample files, in the "
                             "background and in a separate program");
    updateButton.onClick = [this] { index.update(); };
    stopButton.onClick = [this] { index.cancel(); };
    againButton.setTooltip ("Every instrument that did not load, or had a preset that crashed it or sounded silent, "
                            "measured anew");
    againButton.onClick = [this] { index.measureAgain ({}); };
    deleteButton.setTooltip ("Delete everything measured (your saved sounds, roles and preset folders stay)");
    deleteButton.onClick = [this] { confirmDelete(); };

    instrumentList = std::make_unique<InstrumentList> (*this);
    sounds = std::make_unique<SoundIndexView> (index.getFolder());
    sounds->onLeaveOut = [this] (const std::vector<SoundIndex::Row>& rows)
    {
        std::vector<LeftOutSounds::Entry> entries;
        entries.reserve (rows.size());
        for (const auto& r : rows)
            entries.push_back (SoundIndexService::entryOf (r));
        if (index.isUpdating())
            return sounds->setMessage ("Wait for the update to finish (or Stop it), then leave them out.");
        const auto n = index.leaveOut (entries);
        sounds->setMessage (juce::String (n) + (n == 1 ? " sound" : " sounds") +
                            " left out. Show \"Left out\" puts them back.");
    };
    sounds->onPutBack = [this] (const std::vector<SoundIndex::Row>& rows)
    {
        std::vector<LeftOutSounds::Entry> entries;
        entries.reserve (rows.size());
        for (const auto& r : rows)
        {
            LeftOutSounds::Entry e;
            e.sample = r.path;
            e.instrument = r.instrument;
            e.preset = r.id;
            entries.push_back (e);
        }
        const auto n = index.putBack (entries);
        sounds->setMessage (juce::String (n) + (n == 1 ? " sound" : " sounds") +
                            " put back: the update measures them again.");
        sounds->reload();
    };
    sounds->onListen = [this] (const SoundIndex::Row& row)
    {
        sounds->setMessage (row.sample ? "Playing " + row.name + "."
                                       : "Playing " + row.name + " (" + row.source + ")...");
        index.listen (
            row,
            [safe = juce::Component::SafePointer (sounds.get()), name = row.name] (const juce::String& problem)
            {
                if (safe != nullptr)
                    safe->setMessage (problem.isEmpty() ? "Playing " + name + "."
                                                        : name + " could not be played: " + problem + ".");
            });
    };
    folderList = std::make_unique<FolderList> (*this);
    tabs.addTab ("Instruments", juce::Colours::transparentBlack, instrumentList.get(), false);
    tabs.addTab ("Sounds", juce::Colours::transparentBlack, sounds.get(), false);
    tabs.addTab ("Samples", juce::Colours::transparentBlack, folderList.get(), false);
    tabs.setTabBarDepth (28);
    for (auto* c :
         std::initializer_list<juce::Component*> { &status, &updateButton, &againButton, &deleteButton, &tabs })
        addAndMakeVisible (c);
    addChildComponent (stopButton);
    index.addChangeListener (this);
    setSize (820, 600);
    reload();
}

SoundIndexPage::~SoundIndexPage() { index.removeChangeListener (this); }

void SoundIndexPage::reload()
{
    SoundIndex measured (index.getFolder());
    measured.load();
    InstrumentProfiles profiles (index.getFolder());
    profiles.load();
    // Whether it is learned: Spinus's test notes keep to the keys it plays, or may land on ones that switch it.
    auto learning = [&profiles] (InstrumentRow& row, const SoundIndex::Instrument* i)
    {
        row.learned = profiles.get (row.source.key).has_value();
        if (i == nullptr || i->problem.isNotEmpty() || ! row.installed)
            return;
        const auto keys = SoundIndex::keysStamp (profiles.playable (row.source.key));
        if (keys.isNotEmpty())
        {
            if (i->playedKeys == keys)
                row.state << "; measured on the keys it plays (" << profiles.playable (row.source.key)->describe()
                          << ")";
            else
                row.state << "; learned since it was measured: the next update measures it on the keys it plays";
            return;
        }
        if (row.learned || row.always)
            return;
        int odd = 0;
        for (const auto& s : i->sounds)
            odd += (bool)s["oddKeys"] ? 1 : 0;
        row.hint = odd > 0;
        row.state << (row.hint ? "; " + count (odd, "preset sounds", "presets sound") +
                                     " a noise, not a note, on the highest keys: it seems to have keys that play "
                                     "effects or switch it. Learn this instrument for better guesses"
                               : juce::String ("; not learned: if some of its keys switch it or play effects, its "
                                               "guesses may be off (Learn this instrument)"));
    };

    instruments.clear();
    const auto included = index.getIncludedPlugins();
    std::set<juce::String> listed;
    auto describe = [] (InstrumentRow& row, const SoundIndex::Instrument& i)
    {
        int silent = 0;
        for (const auto& s : i.sounds)
            silent += (bool)s["silent"] ? 1 : 0;
        if (i.problem.isNotEmpty())
        {
            row.state = "Did not load: " + i.problem;
            row.problem = true;
            return;
        }
        row.state = count (i.sounds.size(), "preset measured", "presets measured");
        if (! i.complete)
            row.state << " so far (stopped part way: the next update carries on)";
        if (! i.problemPresets.isEmpty())
            row.state << "; " << count (i.problemPresets.size(), "preset", "presets")
                      << " crashed or froze it (Show its sounds, then Problems)";
        if (silent > 0)
            row.state << "; " << count (silent, "is silent", "are silent");
        row.problem = ! i.problemPresets.isEmpty() || silent > 0;
    };
    for (const auto& s : index.getAllSources())
    {
        if (! listed.insert (s.key).second)
            continue;
        InstrumentRow row;
        row.source = s;
        row.always = SoundIndexService::isAlwaysMeasured (s);
        row.measured = row.always || included.contains (s.key);
        if (const auto found = measured.instruments.find (s.key); found != measured.instruments.end())
        {
            describe (row, found->second);
            learning (row, &found->second);
        }
        else
            row.state = row.measured ? "Not measured yet: click Update sound index"
                                     : "Not measured: tick it to measure it at the next update";
        instruments.push_back (std::move (row));
    }
    for (const auto& [key, i] : measured.instruments)
        if (! listed.contains (key))
        {
            InstrumentRow row;
            row.source = i.source;
            row.installed = false;
            describe (row, i);
            row.state = "No longer installed (leaves the index at the next update): " + row.state;
            instruments.push_back (std::move (row));
        }
    std::stable_sort (instruments.begin(), instruments.end(),
                      [] (const InstrumentRow& a, const InstrumentRow& b)
                      {
                          if (a.measured != b.measured)
                              return a.measured;
                          return a.source.name.compareNatural (b.source.name) < 0;
                      });

    folders.clear();
    for (const auto& f : index.getSampleFolders ? index.getSampleFolders() : juce::Array<juce::File>())
    {
        FolderRow row;
        row.folder = f;
        const auto prefix = f.getFullPathName() + juce::File::getSeparatorString();
        for (const auto& s : measured.samples)
            if (s.path.startsWith (prefix))
            {
                ++row.samples;
                row.problems += s.info.hasProperty ("problem") ? 1 : 0;
            }
        folders.push_back (row);
    }

    juce::Array<juce::File> sampleFolders;
    for (const auto& f : folders)
        sampleFolders.add (f.folder);
    sounds->setSampleFolders (sampleFolders);
    instrumentList->refresh();
    folderList->refresh();
    sounds->reload();
    updateButtons();
}

void SoundIndexPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Read again when nothing runs (an update ended, a tick, a forget); while one runs only the status changes.
    if (! index.isUpdating())
        reload();
    updateButtons();
}

void SoundIndexPage::updateButtons()
{
    const bool busy = index.isUpdating();
    status.setText (index.getStatusText(), juce::dontSendNotification);
    updateButton.setEnabled (! busy);
    stopButton.setVisible (busy);
    againButton.setEnabled (! busy);
    deleteButton.setEnabled (! busy);
    instrumentList->updateButtons();
    resized();
}

void SoundIndexPage::toggle (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)instruments.size()))
        return;
    const auto& r = instruments[(size_t)row];
    if (r.always || ! r.installed)
        return;
    index.setIncluded (r.source.key, ! r.measured);
}

void SoundIndexPage::measureAgain (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)instruments.size()) || ! instruments[(size_t)row].installed)
        return;
    index.refreshInstrument (instruments[(size_t)row].source.key);
}

void SoundIndexPage::learn (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)instruments.size()) || ! instruments[(size_t)row].installed)
        return;
    const auto& r = instruments[(size_t)row];
    LearnInstrumentWindow::Listening listening;
    listening.helper = index.getOptions();
    listening.source = r.source;
    LearnInstrumentWindow::open (
        index.getFolder(), r.source.key, r.source.name, this,
        [safe = juce::Component::SafePointer<SoundIndexPage> (this)] (std::optional<PlayableKeys>)
        {
            if (safe != nullptr && ! safe->index.isUpdating())
                safe->reload();
        },
        std::move (listening), aiService);
}

void SoundIndexPage::forget (int row)
{
    if (juce::isPositiveAndBelow (row, (int)instruments.size()))
        index.forget (instruments[(size_t)row].source.key);
}

void SoundIndexPage::showSoundsOf (int row)
{
    if (! juce::isPositiveAndBelow (row, (int)instruments.size()))
        return;
    tabs.setCurrentTabIndex (1);
    sounds->setSearchText ({});
    sounds->setFilter (SoundIndexView::Show::everything, instruments[(size_t)row].source.name);
}

void SoundIndexPage::showSamplesOf (int row, bool problemsOnly)
{
    if (! juce::isPositiveAndBelow (row, (int)folders.size()))
        return;
    tabs.setCurrentTabIndex (1);
    sounds->setSearchText ({});
    sounds->setFilter (problemsOnly ? SoundIndexView::Show::problems : SoundIndexView::Show::samples,
                       folders[(size_t)row].folder.getFullPathName());
}

void SoundIndexPage::measureFolderAgain (int row)
{
    if (juce::isPositiveAndBelow (row, (int)folders.size()))
        index.refreshSamples ({ folders[(size_t)row].folder });
}

bool SoundIndexPage::deleteIndex()
{
    const bool deleted = index.deleteIndex();
    reload();
    return deleted;
}

void SoundIndexPage::confirmDelete()
{
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::QuestionIcon, "Delete the sound index?",
            "Everything measured is deleted; the next Update sound index measures it all again (the first time "
            "takes a while). Your saved sounds, the roles you gave and your preset folders stay.",
            "Delete", "Cancel", this),
        [safe = juce::Component::SafePointer (this)] (int result)
        {
            if (safe != nullptr && result == 1)
                safe->deleteIndex();
        });
}

void SoundIndexPage::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    status.setColour (juce::Label::textColourId, theme.get ("text"));
}

void SoundIndexPage::resized()
{
    auto area = getLocalBounds().reduced (10);
    status.setBounds (area.removeFromTop (40));
    auto buttons = area.removeFromTop (28);
    updateButton.setBounds (buttons.removeFromLeft (150).reduced (0, 1));
    buttons.removeFromLeft (6);
    if (stopButton.isVisible())
    {
        stopButton.setBounds (buttons.removeFromLeft (70).reduced (0, 1));
        buttons.removeFromLeft (6);
    }
    againButton.setBounds (buttons.removeFromLeft (280).reduced (0, 1));
    deleteButton.setBounds (buttons.removeFromRight (140).reduced (0, 1));
    area.removeFromTop (8);
    tabs.setBounds (area);
}

//==============================================================================
namespace
{
SoundIndexWindow* openWindow = nullptr;
}

SoundIndexWindow::SoundIndexWindow (SoundIndexService& service)
    : juce::DocumentWindow ("Sound index", juce::Colours::black, juce::DocumentWindow::closeButton, false)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new SoundIndexPage (service), true);
    setResizable (true, false);
    setResizeLimits (560, 380, 4000, 4000);
    addToDesktop();
    openWindow = this;
}

SoundIndexWindow::~SoundIndexWindow()
{
    if (openWindow == this)
        openWindow = nullptr;
}

void SoundIndexWindow::show (SoundIndexService& service, juce::Component* near, ai::AiService* aiService)
{
    if (openWindow != nullptr)
    {
        openWindow->toFront (true);
        return;
    }
    auto* window = new SoundIndexWindow (service);
    if (auto* page = getOpenPage())
        page->setAiService (aiService);
    if (near != nullptr)
    {
        window->setLookAndFeel (&near->getLookAndFeel());
        window->centreAroundComponent (near->getTopLevelComponent(), window->getWidth(), window->getHeight());
        if (near->isOnDesktop() || near->getTopLevelComponent()->isOnDesktop())
            keepAboveOwner (*window, *near->getTopLevelComponent());
    }
    window->setVisible (true);
}

SoundIndexPage* SoundIndexWindow::getOpenPage()
{
    return openWindow != nullptr ? dynamic_cast<SoundIndexPage*> (openWindow->getContentComponent()) : nullptr;
}

void SoundIndexWindow::close() { delete openWindow; }

void SoundIndexWindow::closeButtonPressed()
{
    // Out of the button's own click first.
    setVisible (false);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SoundIndexWindow> (this)]
                                     { delete safe.getComponent(); });
}

} // namespace spinus::ui
