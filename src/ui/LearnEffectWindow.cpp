#include "LearnEffectWindow.h"

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
} // namespace

//==============================================================================
/** A text cell edited in place (a click starts editing). */
class LearnEffectPanel::TextCell final : public juce::Label
{
public:
    explicit TextCell (LearnEffectPanel& p) : panel (p)
    {
        setEditable (true, true, false);
        onTextChange = [this] { panel.edited (row, column, getText()); };
    }
    void show (int r, int c, const juce::String& text, const juce::String& hint)
    {
        row = r;
        column = c;
        setText (text, juce::dontSendNotification);
        setTooltip (hint);
        const auto& theme = themeOf (panel);
        setColour (textColourId, theme.get ("text"));
        setColour (textWhenEditingColourId, theme.get ("text"));
        setColour (outlineColourId, theme.get ("textDim").withAlpha (0.25f));
    }
    void paint (juce::Graphics& g) override
    {
        juce::Label::paint (g);
        if (getText().isEmpty() && ! isBeingEdited())
        {
            g.setColour (themeOf (panel).get ("textDim").withAlpha (0.6f));
            g.setFont (getFont());
            g.drawFittedText (getTooltip(), getLocalBounds().reduced (4, 0), juce::Justification::centredLeft, 1);
        }
    }

private:
    LearnEffectPanel& panel;
    int row = 0, column = 0;
};

//==============================================================================
LearnEffectPanel::LearnEffectPanel (const juce::String& effectName, std::vector<Session::ParameterText> settings,
                                    std::optional<EffectProfiles::Profile> profile)
    : name (effectName)
{
    for (auto& s : settings)
    {
        Row row { std::move (s), {} };
        row.learned.id = row.plugin.id;
        row.learned.name = row.plugin.name;
        if (profile.has_value())
            if (const auto* known = profile->find (row.plugin.id))
            {
                row.learned = *known;
                row.learned.name = row.plugin.name; // as the plugin names it now
            }
        rows.push_back (std::move (row));
    }
    // Learned settings the plugin no longer reports stay (an older version had them), at the end.
    if (profile.has_value())
        for (const auto& s : profile->settings)
            if (std::none_of (rows.begin(), rows.end(), [&] (const Row& r) { return r.plugin.id == s.id; }))
                rows.push_back ({ { s.id, s.name, {}, {}, {} }, s });

    intro.setText ("What Spinus knows of " + name +
                       "'s settings, for the assistant and coding agents. The range is what the plugin reports; "
                       "click a cell to say what a setting does, a safe range (as the plugin shows values, \"-30 "
                       "dB\"), and tick the few that matter most. Nothing is kept until Save.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    goodForLabel.setText ("Good for", juce::dontSendNotification);
    goodFor.setTextToShowWhenEmpty ("what this effect suits (\"plate reverb, best for vocals and snares\")",
                                    juce::Colours::grey);
    goodFor.setText (profile.has_value() ? profile->goodFor : juce::String(), false);
    searchBox.setTextToShowWhenEmpty ("Search the settings", juce::Colours::grey);
    searchBox.onTextChange = [this] { search (searchBox.getText()); };
    message.setJustificationType (juce::Justification::centredLeft);

    auto& header = table.getHeader();
    header.addColumn ("Setting", settingColumn, 150, 80);
    header.addColumn ("Range (the plugin's)", rangeColumn, 170, 80);
    header.addColumn ("What it does", whatColumn, 260, 100);
    header.addColumn ("Safe from", lowColumn, 90, 60);
    header.addColumn ("Safe to", highColumn, 90, 60);
    header.addColumn ("Matters", mattersColumn, 64, 50, 80);
    table.setModel (this);
    table.setRowHeight (24);

    saveButton.onClick = [this]
    {
        if (onSave)
            onSave (getProfile());
    };
    cancelButton.onClick = [this]
    {
        if (onCancel)
            onCancel();
    };
    forgetButton.setTooltip ("Spinus forgets everything it knew of this effect");
    forgetButton.onClick = [this]
    {
        if (onSave)
            onSave (std::nullopt);
    };
    exportButton.setTooltip ("Save what Spinus knows of this effect as a file to share (facts only)");
    exportButton.onClick = [this]
    {
        if (onExport)
            onExport();
    };
    importButton.setTooltip ("Read a profile file someone exported: it fills the settings you have not set");
    importButton.onClick = [this]
    {
        if (onImport)
            onImport();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &intro, &goodForLabel, &goodFor, &searchBox, &table,
                                                             &message, &saveButton, &cancelButton, &forgetButton,
                                                             &exportButton, &importButton })
        addAndMakeVisible (c);
    setSize (900, 600);
    search ({});
}

LearnEffectPanel::~LearnEffectPanel() { table.setModel (nullptr); }

void LearnEffectPanel::search (const juce::String& text)
{
    const auto words = juce::StringArray::fromTokens (text.toLowerCase(), " ", "");
    shown.clear();
    for (int i = 0; i < (int)rows.size(); ++i)
    {
        const auto& r = rows[(size_t)i];
        const auto haystack = (r.plugin.name + " " + r.learned.what).toLowerCase();
        if (std::all_of (words.begin(), words.end(),
                         [&] (const juce::String& w) { return w.isEmpty() || haystack.contains (w); }))
            shown.push_back (i);
    }
    table.updateContent();
    table.repaint();
    message.setText (juce::String ((int)shown.size()) + " of " + juce::String ((int)rows.size()) + " settings",
                     juce::dontSendNotification);
}

LearnEffectPanel::Row* LearnEffectPanel::rowAt (int shownRow)
{
    return juce::isPositiveAndBelow (shownRow, (int)shown.size()) ? &rows[(size_t)shown[(size_t)shownRow]] : nullptr;
}

void LearnEffectPanel::edited (int shownRow, int column, const juce::String& text)
{
    auto* r = rowAt (shownRow);
    if (r == nullptr)
        return;
    const auto t = text.trim();
    if (column == whatColumn)
        r->learned.what = t;
    else if (column == lowColumn)
        r->learned.low = t;
    else if (column == highColumn)
        r->learned.high = t;
    r->learned.source = EffectProfiles::Source::user;
}

void LearnEffectPanel::setWhat (int row, const juce::String& what) { edited (row, whatColumn, what); }

void LearnEffectPanel::setRange (int row, const juce::String& low, const juce::String& high)
{
    edited (row, lowColumn, low);
    edited (row, highColumn, high);
}

void LearnEffectPanel::setMatters (int row, bool matters)
{
    if (auto* r = rowAt (row))
    {
        r->learned.matters = matters;
        r->learned.source = EffectProfiles::Source::user;
        table.repaintRow (row);
    }
}

juce::String LearnEffectPanel::getRowText (int row) const
{
    if (! juce::isPositiveAndBelow (row, (int)shown.size()))
        return {};
    const auto& r = rows[(size_t)shown[(size_t)row]];
    return r.plugin.name + " | " + r.plugin.low + " to " + r.plugin.high + " | " + r.learned.what + " | " +
           r.learned.low + " to " + r.learned.high + (r.learned.matters ? " | matters" : "");
}

EffectProfiles::Profile LearnEffectPanel::getProfile() const
{
    EffectProfiles::Profile profile;
    profile.goodFor = goodFor.getText().trim();
    for (const auto& r : rows)
        if (! r.learned.isEmpty())
            profile.settings.push_back (r.learned);
    return profile;
}

juce::String LearnEffectPanel::importShared (const juce::var& file, const juce::String& fileName)
{
    juce::String from, problem;
    const auto imported = EffectProfiles::fromShared (file, from, problem);
    juce::String said;
    if (! imported.has_value())
        said = fileName + " could not be read: " + problem;
    else
    {
        int filled = 0;
        for (const auto& s : imported->settings)
        {
            auto found = std::find_if (rows.begin(), rows.end(), [&] (const Row& r) { return r.plugin.id == s.id; });
            if (found == rows.end())
                continue; // a setting this version of the effect does not have
            if (! found->learned.isEmpty() && found->learned.source == EffectProfiles::Source::user)
                continue; // the user's word wins
            found->learned = s;
            found->learned.name = found->plugin.name;
            ++filled;
        }
        if (goodFor.getText().trim().isEmpty())
            goodFor.setText (imported->goodFor, false);
        said = fileName + ": " + juce::String (filled) + (filled == 1 ? " setting" : " settings") + " filled from " +
               (from.isNotEmpty() ? from : juce::String ("the file")) + "'s profile. Save keeps them.";
        table.updateContent();
        table.repaint();
    }
    message.setText (said, juce::dontSendNotification);
    return said;
}

void LearnEffectPanel::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    const auto& theme = themeOf (*this);
    if (selected)
        g.fillAll (theme.get ("accent").withAlpha (0.25f));
    else if (row % 2 == 1)
        g.fillAll (theme.get ("text").withAlpha (0.03f));
}

void LearnEffectPanel::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    auto* r = rowAt (row);
    if (r == nullptr)
        return;
    const auto& theme = themeOf (*this);
    const auto area = juce::Rectangle<int> (0, 0, width, height).reduced (4, 0);
    g.setFont (juce::FontOptions (13.0f));
    if (column == settingColumn)
    {
        g.setColour (theme.get ("text"));
        g.drawFittedText (r->plugin.name, area, juce::Justification::centredLeft, 1);
    }
    else if (column == rangeColumn)
    {
        g.setColour (theme.get ("textDim"));
        g.drawFittedText (r->plugin.low.isNotEmpty() ? r->plugin.low + " to " + r->plugin.high
                                                     : juce::String ("not in this version"),
                          area, juce::Justification::centredLeft, 1);
    }
    else if (column == mattersColumn)
    {
        auto box = juce::Rectangle<int> (0, 0, width, height).withSizeKeepingCentre (14, 14).toFloat();
        g.setColour (theme.get ("text"));
        g.drawRoundedRectangle (box, 2.0f, 1.0f);
        if (r->learned.matters)
        {
            juce::Path p;
            p.startNewSubPath (box.getX() + 3.0f, box.getCentreY());
            p.lineTo (box.getX() + 6.0f, box.getBottom() - 3.0f);
            p.lineTo (box.getRight() - 3.0f, box.getY() + 3.0f);
            g.strokePath (p, juce::PathStrokeType (1.8f));
        }
    }
}

juce::Component* LearnEffectPanel::refreshComponentForCell (int row, int column, bool, juce::Component* existing)
{
    if (column != whatColumn && column != lowColumn && column != highColumn)
    {
        delete existing;
        return nullptr;
    }
    auto* cell = dynamic_cast<TextCell*> (existing);
    if (cell == nullptr)
    {
        delete existing;
        cell = new TextCell (*this);
    }
    const auto* r = rowAt (row);
    if (r == nullptr)
        cell->show (row, column, {}, {});
    else if (column == whatColumn)
        cell->show (row, column, r->learned.what, "what it does");
    else if (column == lowColumn)
        cell->show (row, column, r->learned.low, r->plugin.low);
    else
        cell->show (row, column, r->learned.high, r->plugin.high);
    return cell;
}

juce::String LearnEffectPanel::getCellTooltip (int row, int column)
{
    const auto* r = rowAt (row);
    if (r == nullptr)
        return {};
    if (column == mattersColumn)
        return "Tick the few settings that shape the sound most: agents try those first";
    if (column == rangeColumn)
        return r->plugin.text.isNotEmpty() ? "Now: " + r->plugin.text : juce::String();
    return {};
}

void LearnEffectPanel::cellClicked (int row, int column, const juce::MouseEvent&)
{
    if (const auto* r = rowAt (row); r != nullptr && column == mattersColumn)
        setMatters (row, ! r->learned.matters);
}

void LearnEffectPanel::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    for (auto* l : { &intro, &message })
        l->setColour (juce::Label::textColourId, theme.get ("textDim"));
    goodForLabel.setColour (juce::Label::textColourId, theme.get ("text"));
    table.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
}

void LearnEffectPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    intro.setBounds (area.removeFromTop (40));
    auto line = area.removeFromTop (28);
    goodForLabel.setBounds (line.removeFromLeft (70));
    goodFor.setBounds (line.reduced (0, 2));
    area.removeFromTop (6);
    searchBox.setBounds (area.removeFromTop (26).removeFromLeft (300));
    area.removeFromTop (6);
    auto buttons = area.removeFromBottom (28);
    saveButton.setBounds (buttons.removeFromRight (90).reduced (0, 1));
    buttons.removeFromRight (6);
    cancelButton.setBounds (buttons.removeFromRight (90).reduced (0, 1));
    forgetButton.setBounds (buttons.removeFromLeft (140).reduced (0, 1));
    buttons.removeFromLeft (6);
    exportButton.setBounds (buttons.removeFromLeft (90).reduced (0, 1));
    buttons.removeFromLeft (6);
    importButton.setBounds (buttons.removeFromLeft (90).reduced (0, 1));
    area.removeFromBottom (4);
    message.setBounds (area.removeFromBottom (24));
    table.setBounds (area);
}

//==============================================================================
namespace
{
std::vector<LearnEffectWindow*>& openWindows()
{
    static std::vector<LearnEffectWindow*> windows;
    return windows;
}
} // namespace

void LearnEffectWindow::open (const juce::File& indexFolder, const juce::String& effect, const juce::String& name,
                              std::vector<Session::ParameterText> settings, juce::Component* near)
{
    for (auto* w : openWindows())
        if (w->effect == effect)
        {
            w->toFront (true);
            return;
        }
    auto* window = new LearnEffectWindow (indexFolder, effect, name, std::move (settings));
    if (near != nullptr)
    {
        window->centreAroundComponent (near->getTopLevelComponent(), window->getWidth(), window->getHeight());
        if (near->getTopLevelComponent()->isOnDesktop())
            keepAboveOwner (*window, *near->getTopLevelComponent());
    }
    window->setVisible (true);
}

LearnEffectWindow::LearnEffectWindow (const juce::File& folder, const juce::String& key, const juce::String& effectName,
                                      std::vector<Session::ParameterText> settings)
    : juce::DocumentWindow ("Learn " + effectName, juce::Colours::black, juce::DocumentWindow::closeButton, false),
      indexFolder (folder), effect (key), name (effectName)
{
    EffectProfiles profiles (indexFolder);
    profiles.load();
    auto* panel = new LearnEffectPanel (name, std::move (settings), profiles.get (effect));
    panel->onCancel = [this] { finish(); };
    panel->onSave = [this] (std::optional<EffectProfiles::Profile> profile)
    {
        EffectProfiles all (indexFolder);
        all.load();
        all.set (effect, std::move (profile));
        all.save();
        finish();
    };
    panel->onExport = [this] { exportProfile(); };
    panel->onImport = [this] { importProfile(); };
    setUsingNativeTitleBar (true);
    setContentOwned (panel, true);
    setResizable (true, false);
    addToDesktop();
    openWindows().push_back (this);
}

LearnEffectWindow::~LearnEffectWindow()
{
    std::erase (openWindows(), this);
    *alive = false;
}

void LearnEffectWindow::exportProfile()
{
    auto* panel = dynamic_cast<LearnEffectPanel*> (getContentComponent());
    if (panel == nullptr)
        return;
    const auto shared = panel->getShared();
    const auto file = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                          .getChildFile (juce::File::createLegalFileName (name + " effect profile.json"));
    chooser = std::make_unique<core::FileDialog> ("Export the profile of " + name, file, "*.json", this);
    chooser->launchAsync ((int)((unsigned)juce::FileBrowserComponent::saveMode |
                                (unsigned)juce::FileBrowserComponent::canSelectFiles |
                                (unsigned)juce::FileBrowserComponent::warnAboutOverwriting),
                          [shared, a = alive] (const core::FileDialog& fc)
                          {
                              if (*a && fc.getResult() != juce::File())
                                  fc.getResult().replaceWithText (juce::JSON::toString (shared));
                          });
}

void LearnEffectWindow::importProfile()
{
    chooser = std::make_unique<core::FileDialog> ("Import a profile for " + name, juce::File(), "*.json", this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, a = alive] (const core::FileDialog& fc)
                          {
                              if (! *a || fc.getResult() == juce::File())
                                  return;
                              if (auto* p = dynamic_cast<LearnEffectPanel*> (getContentComponent()))
                                  p->importShared (juce::JSON::parse (fc.getResult()), fc.getResult().getFileName());
                          });
}

void LearnEffectWindow::closeButtonPressed() { finish(); }

void LearnEffectWindow::finish()
{
    // Out of the button's own click first.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<LearnEffectWindow> (this)]
                                     { delete safe.getComponent(); });
    setVisible (false);
}

int LearnEffectWindow::getNumOpen() { return (int)openWindows().size(); }

void LearnEffectWindow::closeAll()
{
    while (! openWindows().empty())
        delete openWindows().back();
}

LearnEffectPanel* LearnEffectWindow::lastOpened()
{
    return openWindows().empty() ? nullptr
                                 : dynamic_cast<LearnEffectPanel*> (openWindows().back()->getContentComponent());
}

} // namespace spinus::ui
