#include "SoundIndexView.h"

#include "Theme.h"
#include "session/PartSounds.h"

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

SoundIndexView::SoundIndexView (const juce::File& folder) : marked (folder)
{
    SoundIndex index (folder);
    index.load();
    all = index.rows();
    marked.load();
    for (const auto& [key, instrument] : index.instruments)
        for (const auto& sound : instrument.sounds)
            ids[instrument.source.name + "\n" + sound["name"].toString()] = { key, sound["id"].toString() };
    intro.setText ("What Spinus measured of your instruments' presets and your samples, and the role it guesses "
                   "for each (from its name when the name says, otherwise from how it sounds). Search by name, "
                   "instrument, role or anything shown; right-click a preset to give it a role of your own.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    count.setJustificationType (juce::Justification::centredLeft);
    searchBox.setTextToShowWhenEmpty ("Search (for example: bass, pad, guitar, loop, kick)", juce::Colours::grey);
    searchBox.onTextChange = [this] { search (searchBox.getText()); };
    list.setModel (this);
    list.setRowHeight (22);
    for (auto* c : std::initializer_list<juce::Component*> { &intro, &count, &searchBox, &list })
        addAndMakeVisible (c);
    setSize (760, 520);
    search ({});
}

SoundIndexView::~SoundIndexView() { list.setModel (nullptr); }

void SoundIndexView::show (const juce::File& folder, juce::Component* near)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new SoundIndexView (folder));
    options.dialogTitle = "Sound index";
    options.componentToCentreAround = near;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (near != nullptr)
        if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&near->getLookAndFeel()))
        {
            options.dialogBackgroundColour = lf->getTheme().get ("panel");
            options.content->setLookAndFeel (lf);
        }
    options.launchAsync();
}

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
        const auto haystack =
            (row.name + " " + row.source + " " + row.role + " " + row.detail + (row.sample ? " sample" : " preset"))
                .toLowerCase();
        if (std::all_of (words.begin(), words.end(),
                         [&] (const juce::String& w) { return w.isEmpty() || haystack.contains (w); }))
            shown.push_back (row);
    }
    count.setText (all.empty() ? juce::String ("The sound index is empty: Update sound index in the Track creator.")
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
    count.setBounds (row.removeFromRight (260));
    searchBox.setBounds (row.reduced (0, 1));
    area.removeFromTop (6);
    list.setBounds (area);
}

} // namespace spinus::ui
