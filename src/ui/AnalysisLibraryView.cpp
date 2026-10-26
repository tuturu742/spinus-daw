#include "AnalysisLibraryView.h"

#include "Theme.h"
#include "session/SoundIndex.h"

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

AnalysisLibraryView::AnalysisLibraryView (AnalysisLibrary l, juce::File sounds)
    : library (std::move (l)), soundsFolder (std::move (sounds))
{
    intro.setText ("What Spinus learnt about the songs you analyzed (tempo, key, notes, style) and their stems, "
                   "kept so they are not analyzed again. Deleting removes only these, never your own audio files; "
                   "a song is simply analyzed again when you use it.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    total.setJustificationType (juce::Justification::centredLeft);
    list.setModel (this);
    list.setRowHeight (24);
    stemsButton.setTooltip ("Delete the selected song's stems: what was measured stays");
    stemsButton.onClick = [this] { deleteStems (list.getSelectedRow()); };
    songButton.setTooltip ("Delete everything kept of the selected song");
    songButton.onClick = [this] { deleteSong (list.getSelectedRow()); };
    allStemsButton.setTooltip ("Delete every song's stems (most of the space): what was measured stays");
    allStemsButton.onClick = [this] { deleteAllStems(); };
    everythingButton.setTooltip ("Delete everything kept of every song");
    everythingButton.onClick = [this] { deleteEverything(); };
    for (auto* c : std::initializer_list<juce::Component*> { &intro, &total, &list, &stemsButton, &songButton,
                                                             &allStemsButton, &everythingButton })
        addAndMakeVisible (c);
    soundsButton.setTooltip ("Delete what the Track creator measured of your instruments and samples (never the sounds "
                             "themselves); Update sound index measures them again");
    soundsButton.onClick = [this] { deleteSoundIndex(); };
    soundsText.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (soundsText);
    addChildComponent (soundsButton);
    soundsText.setVisible (soundsFolder != juce::File());
    soundsButton.setVisible (soundsFolder != juce::File());
    setSize (640, 450);
    refresh();
}

AnalysisLibraryView::~AnalysisLibraryView() { list.setModel (nullptr); }

void AnalysisLibraryView::show (const AnalysisLibrary& library, juce::Component* near,
                                const juce::File& soundIndexFolder, std::function<void()> onSoundIndexDeleted)
{
    juce::DialogWindow::LaunchOptions options;
    auto* view = new AnalysisLibraryView (library, soundIndexFolder);
    view->onSoundIndexDeleted = std::move (onSoundIndexDeleted);
    options.content.setOwned (view);
    options.dialogTitle = "Saved analyses";
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

juce::String AnalysisLibraryView::sizeText (juce::int64 bytes)
{
    if (bytes >= 1000000000)
        return juce::String ((double)bytes / 1e9, 1) + " GB";
    if (bytes >= 1000000)
        return juce::String ((double)bytes / 1e6, 1) + " MB";
    return juce::String (juce::jmax<juce::int64> (bytes > 0 ? 1 : 0, (bytes + 999) / 1000)) + " KB";
}

juce::String AnalysisLibraryView::describe (const AnalysisLibrary::Entry& entry)
{
    auto text = entry.name + "  -  " + sizeText (entry.bytes);
    if (entry.stemBytes > 0)
        text += " (stems: " + sizeText (entry.stemBytes) + ")";
    if (! entry.current)
        text += "  -  made by another version of Spinus, not used";
    return text;
}

void AnalysisLibraryView::refresh()
{
    entries = library.list();
    juce::int64 bytes = 0, stems = 0;
    for (const auto& e : entries)
    {
        bytes += e.bytes;
        stems += e.stemBytes;
    }
    total.setText (entries.empty()
                       ? juce::String ("No songs are kept.")
                       : juce::String ((int)entries.size()) + (entries.size() == 1 ? " song, " : " songs, ") +
                             sizeText (bytes) + " in all, of which stems " + sizeText (stems) + ".",
                   juce::dontSendNotification);
    if (soundsFolder != juce::File())
    {
        const auto used = SoundIndex (soundsFolder).getDiskUse();
        soundsText.setText (used > 0 ? "The Track creator's sound index (how your instruments and samples sound): " +
                                           sizeText (used) + "."
                                     : juce::String ("The Track creator's sound index: none yet."),
                            juce::dontSendNotification);
    }
    list.updateContent();
    list.repaint();
    updateButtons();
}

bool AnalysisLibraryView::deleteSoundIndex()
{
    if (soundsFolder == juce::File() || SoundIndexer::isAnyRunning())
        return false;
    SoundIndex index (soundsFolder);
    const bool done = index.removeAll();
    refresh();
    if (onSoundIndexDeleted)
        onSoundIndexDeleted();
    return done;
}

bool AnalysisLibraryView::deleteStems (int row)
{
    if (row < 0 || row >= (int)entries.size())
        return false;
    const bool done = library.removeStems (entries[(size_t)row].key);
    refresh();
    return done;
}

bool AnalysisLibraryView::deleteSong (int row)
{
    if (row < 0 || row >= (int)entries.size())
        return false;
    const bool done = library.removeSong (entries[(size_t)row].key);
    list.deselectAllRows();
    refresh();
    return done;
}

bool AnalysisLibraryView::deleteAllStems()
{
    const bool done = library.removeAllStems();
    refresh();
    return done;
}

bool AnalysisLibraryView::deleteEverything()
{
    const bool done = library.removeAll();
    list.deselectAllRows();
    refresh();
    return done;
}

void AnalysisLibraryView::updateButtons()
{
    const auto row = list.getSelectedRow();
    const bool selected = row >= 0 && row < (int)entries.size();
    songButton.setEnabled (selected);
    stemsButton.setEnabled (selected && entries[(size_t)row].stemBytes > 0);
    allStemsButton.setEnabled (std::any_of (entries.begin(), entries.end(), [] (auto& e) { return e.stemBytes > 0; }));
    everythingButton.setEnabled (! entries.empty());
    soundsButton.setEnabled (soundsFolder != juce::File() && soundsFolder.exists() && ! SoundIndexer::isAnyRunning());
}

void AnalysisLibraryView::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int)entries.size())
        return;
    const auto& theme = themeOf (*this);
    if (selected)
        g.fillAll (theme.get ("accent").withAlpha (0.25f));
    g.setColour (entries[(size_t)row].current ? theme.get ("text") : theme.get ("textDim"));
    g.setFont (juce::FontOptions (14.0f));
    g.drawFittedText (describe (entries[(size_t)row]), 6, 0, width - 12, height, juce::Justification::centredLeft, 1);
}

void AnalysisLibraryView::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    intro.setColour (juce::Label::textColourId, theme.get ("textDim"));
    total.setColour (juce::Label::textColourId, theme.get ("text"));
    soundsText.setColour (juce::Label::textColourId, theme.get ("text"));
    list.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
}

void AnalysisLibraryView::resized()
{
    auto area = getLocalBounds().reduced (10);
    intro.setBounds (area.removeFromTop (54));
    total.setBounds (area.removeFromTop (26));
    auto buttons = area.removeFromBottom (28);
    for (auto* b : { &stemsButton, &songButton })
    {
        b->setBounds (buttons.removeFromLeft (b == &songButton ? 80 : 130).reduced (0, 1));
        buttons.removeFromLeft (6);
    }
    everythingButton.setBounds (buttons.removeFromRight (140).reduced (0, 1));
    buttons.removeFromRight (6);
    allStemsButton.setBounds (buttons.removeFromRight (130).reduced (0, 1));
    area.removeFromBottom (6);
    if (soundsFolder != juce::File())
    {
        auto row = area.removeFromBottom (28);
        soundsButton.setBounds (row.removeFromRight (170).reduced (0, 1));
        soundsText.setBounds (row);
        area.removeFromBottom (6);
    }
    list.setBounds (area);
}

} // namespace spinus::ui
