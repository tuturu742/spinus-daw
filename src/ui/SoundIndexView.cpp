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
    showBox.setSelectedId (1, juce::dontSendNotification);
    showBox.setTooltip ("Show every sound, only presets, only samples, or only what went wrong (and why)");
    showBox.onChange = [this] { setFilter ((Show)(showBox.getSelectedId() - 1), from); };
    fromBox.setTooltip ("One instrument's presets, or one sample folder's samples");
    fromBox.onChange = [this]
    {
        const auto i = fromBox.getSelectedId() - 2;
        setFilter (show, juce::isPositiveAndBelow (i, fromItems.size()) ? fromItems[i] : juce::String());
    };
    list.setModel (this);
    list.setRowHeight (22);
    for (auto* c : std::initializer_list<juce::Component*> { &intro, &count, &searchBox, &showBox, &fromBox, &list })
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
    fillFromMenu();
    search (searchBox.getText());
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
    for (const auto& row : all)
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
    count.setText (all.empty() ? juce::String ("The sound index is empty: click Update sound index.")
                               : juce::String ((int)shown.size()) + " of " + juce::String ((int)all.size()) + " sounds",
                   juce::dontSendNotification);
    list.updateContent();
    list.repaint();
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
    if (! e.mods.isPopupMenu() || idsOf (shown[(size_t)juce::jlimit (0, (int)shown.size() - 1, row)]).first.isEmpty())
        return;
    juce::PopupMenu menu;
    const auto mine = markOf (row);
    juce::PopupMenu roles;
    for (const auto& role : parts::songRoles())
        roles.addItem (parts::roleName (role), true, mine == role, [this, row, role] { mark (row, role); });
    menu.addSubMenu ("Use as", roles);
    if (mine.isNotEmpty())
        menu.addItem ("Spinus's guess again", [this, row] { mark (row, {}); });
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
    list.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
}

void SoundIndexView::resized()
{
    auto area = getLocalBounds().reduced (10);
    intro.setBounds (area.removeFromTop (40));
    auto row = area.removeFromTop (28);
    count.setBounds (row.removeFromRight (150));
    fromBox.setBounds (row.removeFromRight (200).reduced (4, 1));
    showBox.setBounds (row.removeFromRight (120).reduced (4, 1));
    searchBox.setBounds (row.reduced (0, 1));
    area.removeFromTop (6);
    list.setBounds (area);
}

} // namespace spinus::ui
