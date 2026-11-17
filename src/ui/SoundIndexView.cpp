#include "SoundIndexView.h"

#include "Theme.h"
#include "session/PartSounds.h"

#include <set>

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
} // namespace

SoundIndexView::SoundIndexView (const juce::File& f) : folder (f), marked (f)
{
    intro.setText (
        "What Spinus measured of your instruments' presets and your samples, and the role it guesses "
        "for each (from its name when the name says, otherwise from how it sounds). Search by name, "
        "instrument, role or anything shown; Show Problems lists what went wrong, and why. Right-click a preset to "
        "give it a role of your own.",
        juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    count.setJustificationType (juce::Justification::centredLeft);
    searchBox.setTextToShowWhenEmpty ("Search (for example: bass, pad, guitar, loop, kick)", juce::Colours::grey);
    searchBox.onTextChange = [this] { search (searchBox.getText()); };
    showBox.addItem ("All sounds", 1 + (int)Show::everything);
    showBox.addItem ("Presets", 1 + (int)Show::presets);
    showBox.addItem ("Samples", 1 + (int)Show::samples);
    showBox.addItem ("Problems", 1 + (int)Show::problems);
    showBox.addSeparator();
    showBox.addItem ("Left out", 1 + (int)Show::leftOut);
    showBox.setSelectedId (1, juce::dontSendNotification);
    showBox.setTooltip ("Show every sound, only presets, only samples, or only what went wrong (and why)");
    showBox.onChange = [this] { setFilter ((Show)(showBox.getSelectedId() - 1), from); };
    fromBox.setTooltip ("One instrument's presets, or one sample folder's samples");
    roleBox.setTooltip ("Only sounds of one role (bass, pad, lead...), or samples of one kind (kick, loop...)");
    roleBox.onChange = [this]
    { setRole (roleBox.getSelectedId() > 1 ? roleBox.getItemText (roleBox.getSelectedItemIndex()) : juce::String()); };
    list.setMultipleSelectionEnabled (true);
    listenButton.setTooltip ("Play the selected sound (a preset is played by its instrument the first time, which "
                             "takes a moment)");
    listenButton.onClick = [this]
    {
        if (const auto chosen = getSelected(); ! chosen.empty() && onListen)
            onListen (chosen.front());
    };
    actButton.onClick = [this] { actOnSelected(); };
    allButton.onClick = [this] { actOnAllShown(); };
    message.setJustificationType (juce::Justification::centredLeft);
    fromBox.onChange = [this]
    {
        const auto i = fromBox.getSelectedId() - 2;
        setFilter (show, juce::isPositiveAndBelow (i, fromItems.size()) ? fromItems[i] : juce::String());
    };
    list.setModel (this);
    list.setRowHeight (22);
    for (auto* c : std::initializer_list<juce::Component*> { &intro, &count, &searchBox, &showBox, &fromBox, &roleBox,
                                                             &list, &listenButton, &actButton, &allButton, &message })
        addAndMakeVisible (c);
    setSize (760, 520);
    reload();
}

void SoundIndexView::reload()
{
    SoundIndex index (folder);
    index.load();
    all = index.rows();
    marked = UserRoles (folder);
    marked.load();
    ids.clear();
    for (const auto& [key, instrument] : index.instruments)
        for (const auto& sound : instrument.sounds)
            ids[instrument.source.name + "\n" + sound["name"].toString()] = { key, sound["id"].toString() };
    leftOutRows.clear();
    LeftOutSounds leftOut (folder);
    leftOut.load();
    for (const auto& e : leftOut.getEntries())
    {
        SoundIndex::Row row;
        row.name = e.name.isNotEmpty() ? e.name : (e.isSample() ? juce::File (e.sample).getFileName() : e.preset);
        row.source = e.source;
        row.role = "left out";
        row.detail = e.isSample() ? e.sample : juce::String ("a preset; Put back measures it again");
        row.sample = e.isSample();
        row.path = e.sample;
        row.instrument = e.instrument;
        row.id = e.preset;
        leftOutRows.push_back (row);
    }
    fillFromMenu();
    fillRoleMenu();
    search (searchBox.getText());
}

void SoundIndexView::fillRoleMenu()
{
    std::set<juce::String> roles;
    for (const auto& row : all)
        if (row.role.isNotEmpty() && row.role != "problem")
            roles.insert (row.role);
    roleBox.clear (juce::dontSendNotification);
    roleBox.addItem ("Any role", 1);
    int id = 2, chosen = 1;
    for (const auto& r : roles)
    {
        if (r == roleFilter)
            chosen = id;
        roleBox.addItem (r, id++);
    }
    if (chosen == 1)
        roleFilter.clear();
    roleBox.setSelectedId (chosen, juce::dontSendNotification);
}

void SoundIndexView::setRole (const juce::String& r)
{
    roleFilter = r;
    fillRoleMenu();
    search (searchBox.getText());
}

std::vector<SoundIndex::Row> SoundIndexView::getSelected() const
{
    std::vector<SoundIndex::Row> rows;
    const auto selected = list.getSelectedRows();
    for (int i = 0; i < selected.size(); ++i)
        if (juce::isPositiveAndBelow (selected[i], (int)shown.size()))
            rows.push_back (shown[(size_t)selected[i]]);
    return rows;
}

void SoundIndexView::actOnSelected() { act (getSelected()); }

void SoundIndexView::actOnAllShown() { act (shown); }

void SoundIndexView::act (const std::vector<SoundIndex::Row>& rows)
{
    std::vector<SoundIndex::Row> usable;
    for (const auto& r : rows)
        if (! r.whole)
            usable.push_back (r);
    if (usable.empty())
        return;
    const bool back = show == Show::leftOut;
    const auto run = [this, back, usable]
    {
        if (back ? onPutBack : onLeaveOut)
            (back ? onPutBack : onLeaveOut) (usable);
    };
    if (usable.size() == 1)
        return run();
    const auto n = juce::String ((int)usable.size());
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::QuestionIcon, back ? "Put back " + n + " sounds?" : "Leave out " + n + " sounds?",
            back ? "They are measured again by the next update, which starts now."
                 : "They leave the sound index, and no update measures them again, so Spinus never suggests them. "
                   "Show \"Left out\" lists them to put back.",
            back ? "Put back" : "Leave out", "Cancel", this),
        [safe = juce::Component::SafePointer (this), run] (int result)
        {
            if (safe != nullptr && result == 1)
                run();
        });
}

void SoundIndexView::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, (int)shown.size()) && show != Show::leftOut && onListen)
        onListen (shown[(size_t)row]);
}

void SoundIndexView::updateButtons()
{
    const bool back = show == Show::leftOut;
    const auto selected = getSelected();
    actButton.setButtonText (back ? "Put back" : "Leave out");
    allButton.setButtonText ((back ? "Put back all shown (" : "Leave out all shown (") +
                             juce::String ((int)shown.size()) + ")");
    actButton.setEnabled (std::any_of (selected.begin(), selected.end(), [] (const auto& r) { return ! r.whole; }));
    allButton.setEnabled (! shown.empty());
    listenButton.setVisible (! back);
    listenButton.setEnabled (selected.size() == 1 && selected.front().role != "problem");
}

void SoundIndexView::setSampleFolders (const juce::Array<juce::File>& folders)
{
    sampleFolders = folders;
    fillFromMenu();
}

void SoundIndexView::fillFromMenu()
{
    std::set<juce::String> instruments;
    for (const auto& row : all)
        if (! row.sample)
            instruments.insert (row.source);
    fromItems.clear();
    fromBox.clear (juce::dontSendNotification);
    fromBox.addItem ("Every instrument and folder", 1);
    for (const auto& name : instruments)
    {
        fromItems.add (name);
        fromBox.addItem (name, fromItems.size() + 1);
    }
    if (! sampleFolders.isEmpty())
        fromBox.addSeparator();
    for (const auto& f : sampleFolders)
    {
        fromItems.add (f.getFullPathName());
        fromBox.addItem ("Folder: " + f.getFileName(), fromItems.size() + 1);
    }
    const auto i = fromItems.indexOf (from);
    if (i < 0)
        from.clear();
    fromBox.setSelectedId (i < 0 ? 1 : i + 2, juce::dontSendNotification);
}

void SoundIndexView::setFilter (Show s, const juce::String& source)
{
    show = s;
    from = source;
    showBox.setSelectedId (1 + (int)show, juce::dontSendNotification);
    const auto i = fromItems.indexOf (from);
    fromBox.setSelectedId (i < 0 ? 1 : i + 2, juce::dontSendNotification);
    search (searchBox.getText());
}

bool SoundIndexView::passes (const SoundIndex::Row& row) const
{
    const bool problem = row.role == "problem";
    if ((show == Show::presets && row.sample) || (show == Show::samples && ! row.sample) ||
        (show == Show::problems && ! problem))
        return false;
    if (roleFilter.isNotEmpty() && show != Show::leftOut && row.role != roleFilter)
        return false;
    if (from.isEmpty())
        return true;
    if (row.sample)
        return row.path.startsWith (from + juce::File::getSeparatorString());
    return row.source == from;
}

SoundIndexView::~SoundIndexView() { list.setModel (nullptr); }

juce::String SoundIndexView::describe (const SoundIndex::Row& row)
{
    return row.name + "  (" + row.source + ")  -  " + row.role + (row.detail.isNotEmpty() ? ": " + row.detail : "");
}

void SoundIndexView::search (const juce::String& text)
{
    const auto words = juce::StringArray::fromTokens (text.toLowerCase(), " ", "\"");
    shown.clear();
    for (const auto& row : show == Show::leftOut ? leftOutRows : all)
    {
        if (! passes (row))
            continue;
        const auto haystack =
            (row.name + " " + row.source + " " + row.role + " " + row.detail + (row.sample ? " sample" : " preset"))
                .toLowerCase();
        if (std::all_of (words.begin(), words.end(), [&] (const juce::String& w)
                         { return w.unquoted().isEmpty() || haystack.contains (w.unquoted()); }))
            shown.push_back (row);
    }
    if (show == Show::leftOut)
        count.setText (juce::String ((int)shown.size()) + " of " + juce::String ((int)leftOutRows.size()) + " left out",
                       juce::dontSendNotification);
    else
        count.setText (all.empty()
                           ? juce::String ("The sound index is empty: click Update sound index.")
                           : juce::String ((int)shown.size()) + " of " + juce::String ((int)all.size()) + " sounds",
                       juce::dontSendNotification);
    list.deselectAllRows();
    list.updateContent();
    list.repaint();
    updateButtons();
}

void SoundIndexView::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int)shown.size())
        return;
    const auto& theme = themeOf (*this);
    const auto& r = shown[(size_t)row];
    if (selected)
        g.fillAll (theme.get ("accent").withAlpha (0.25f));
    auto area = juce::Rectangle<int> (0, 0, width, height).reduced (6, 0);
    g.setFont (juce::FontOptions (13.0f));
    g.setColour (theme.get ("text"));
    g.drawFittedText (r.name, area.removeFromLeft (area.getWidth() * 30 / 100), juce::Justification::centredLeft, 1);
    g.setColour (theme.get ("textDim"));
    g.drawFittedText (r.source, area.removeFromLeft (area.getWidth() * 25 / 100), juce::Justification::centredLeft, 1);
    const auto mine = markOf (row);
    g.setColour (r.role == "problem" || mine.isNotEmpty() ? theme.get ("accent") : theme.get ("text"));
    g.drawFittedText (mine.isNotEmpty() ? parts::roleName (mine).toLowerCase() : r.role,
                      area.removeFromLeft (mine.isNotEmpty() ? 110 : 70), juce::Justification::centredLeft, 1);
    g.setColour (theme.get ("textDim"));
    g.drawFittedText (r.detail, area, juce::Justification::centredLeft, 1);
}

std::pair<juce::String, juce::String> SoundIndexView::idsOf (const SoundIndex::Row& row) const
{
    if (row.sample)
        return {};
    const auto found = ids.find (row.source + "\n" + row.name);
    return found != ids.end() ? found->second : std::pair<juce::String, juce::String>();
}

juce::String SoundIndexView::markOf (int row) const
{
    if (row < 0 || row >= (int)shown.size())
        return {};
    const auto [instrument, preset] = idsOf (shown[(size_t)row]);
    return instrument.isEmpty() ? juce::String() : marked.get (instrument, preset);
}

bool SoundIndexView::mark (int row, const juce::String& role)
{
    if (row < 0 || row >= (int)shown.size())
        return false;
    const auto [instrument, preset] = idsOf (shown[(size_t)row]);
    if (instrument.isEmpty())
        return false;
    marked.set (instrument, preset, role);
    list.repaintRow (row);
    return marked.save();
}

void SoundIndexView::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() || ! juce::isPositiveAndBelow (row, (int)shown.size()))
        return;
    if (! list.isRowSelected (row))
        list.selectRow (row);
    const auto r = shown[(size_t)row];
    juce::PopupMenu menu;
    if (show == Show::leftOut)
    {
        menu.addItem ("Put back", [this] { actOnSelected(); });
        menu.showMenuAsync ({});
        return;
    }
    menu.addItem ("Listen", r.role != "problem" && onListen != nullptr, false, [this, r] { onListen (r); });
    menu.addItem (getSelected().size() > 1 ? "Leave the selected sounds out of the index" : "Leave out of the index",
                  ! r.whole, false, [this] { actOnSelected(); });
    if (! idsOf (r).first.isEmpty())
    {
        const auto mine = markOf (row);
        juce::PopupMenu roles;
        for (const auto& songRole : parts::songRoles())
            roles.addItem (parts::roleName (songRole), true, mine == songRole,
                           [this, row, songRole] { mark (row, songRole); });
        menu.addSeparator();
        menu.addSubMenu ("Use as", roles);
        if (mine.isNotEmpty())
            menu.addItem ("Spinus's guess again", [this, row] { mark (row, {}); });
    }
    menu.showMenuAsync ({});
}

juce::String SoundIndexView::getTooltipForRow (int row)
{
    return row >= 0 && row < (int)shown.size() ? describe (shown[(size_t)row]) : juce::String();
}

void SoundIndexView::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    intro.setColour (juce::Label::textColourId, theme.get ("textDim"));
    count.setColour (juce::Label::textColourId, theme.get ("text"));
    message.setColour (juce::Label::textColourId, theme.get ("textDim"));
    list.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
}

void SoundIndexView::resized()
{
    auto area = getLocalBounds().reduced (10);
    intro.setBounds (area.removeFromTop (40));
    auto row = area.removeFromTop (28);
    count.setBounds (row.removeFromRight (130));
    roleBox.setBounds (row.removeFromRight (110).reduced (4, 1));
    fromBox.setBounds (row.removeFromRight (180).reduced (4, 1));
    showBox.setBounds (row.removeFromRight (110).reduced (4, 1));
    searchBox.setBounds (row.reduced (0, 1));
    area.removeFromTop (6);
    auto buttons = area.removeFromBottom (28);
    listenButton.setBounds (buttons.removeFromLeft (80).reduced (0, 1));
    buttons.removeFromLeft (6);
    actButton.setBounds (buttons.removeFromLeft (100).reduced (0, 1));
    buttons.removeFromLeft (6);
    allButton.setBounds (buttons.removeFromLeft (190).reduced (0, 1));
    buttons.removeFromLeft (10);
    message.setBounds (buttons);
    area.removeFromBottom (6);
    list.setBounds (area);
}

} // namespace spinus::ui
