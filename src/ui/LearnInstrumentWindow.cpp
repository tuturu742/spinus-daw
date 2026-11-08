#include "LearnInstrumentWindow.h"

#include "Theme.h"
#include "ai/AiService.h"
#include "ai/AnalysisJobs.h"
#include "session/KeyProbe.h"
#include "session/PartSounds.h"
#include "WindowStacking.h"

namespace spinus::ui
{

namespace
{
using Job = InstrumentProfiles::Job;
using Voices = InstrumentProfiles::Voices;

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

bool isBlack (int note)
{
    const auto n = note % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

juce::String jobWords (Job job)
{
    switch (job)
    {
    case Job::plays:
        return "plays";
    case Job::switches:
        return "a switch key";
    case Job::special:
        return "a special key";
    case Job::unused:
        return "does nothing";
    case Job::unknown:
        break;
    }
    return "not known";
}

juce::String sourceWords (InstrumentProfiles::Source source)
{
    switch (source)
    {
    case InstrumentProfiles::Source::user:
        return "set by you";
    case InstrumentProfiles::Source::probing:
        return "heard by Spinus";
    case InstrumentProfiles::Source::plugin:
        return "said by the instrument";
    case InstrumentProfiles::Source::manual:
        return "from its manual";
    case InstrumentProfiles::Source::bundled:
        return "comes with Spinus";
    case InstrumentProfiles::Source::imported:
        return "from an imported profile";
    }
    return {};
}

juce::Colour jobColour (const Theme& theme, Job job, bool black)
{
    switch (job)
    {
    case Job::plays:
        return theme.get ("accent").withMultipliedBrightness (black ? 0.7f : 1.0f);
    case Job::switches:
        return theme.get ("danger").withMultipliedBrightness (black ? 0.7f : 1.0f);
    case Job::special:
        return theme.get ("accentAlt").withMultipliedBrightness (black ? 0.7f : 1.0f);
    case Job::unused:
        return black ? juce::Colour (0xff303030) : juce::Colour (0xff707070);
    case Job::unknown:
        break;
    }
    return black ? juce::Colour (0xff181818) : juce::Colour (0xffe8e8e8);
}
} // namespace

/** 128 keys as a piano: drag to select, hover to read what a key does. */
class LearnInstrumentPanel::Keyboard final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Keyboard (LearnInstrumentPanel& p) : panel (p) {}

    static constexpr int whiteKeys = 75; // keys 0 to 127

    int noteAt (juce::Point<float> p) const
    {
        const auto w = whiteWidth();
        if (p.y < (float)getHeight() * 0.62f)
            for (int note = 0; note < 128; ++note)
                if (isBlack (note) && blackBounds (note).contains (p))
                    return note;
        const auto white = juce::jlimit (0, whiteKeys - 1, (int)(p.x / w));
        // The white key's note: 7 an octave, C D E F G A B.
        static const int steps[] = { 0, 2, 4, 5, 7, 9, 11 };
        return juce::jmin (127, white / 7 * 12 + steps[white % 7]);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto& keys = panel.current().keys;
        const auto selected = [this] (int note)
        {
            return panel.selectedFrom >= 0 && note >= juce::jmin (panel.selectedFrom, panel.selectedTo) &&
                   note <= juce::jmax (panel.selectedFrom, panel.selectedTo);
        };
        const auto w = whiteWidth();
        const auto labels = (float)getHeight() - 14.0f;
        int white = 0;
        for (int note = 0; note < 128; ++note)
        {
            if (isBlack (note))
                continue;
            const juce::Rectangle<float> r ((float)white * w, 0.0f, w, labels);
            g.setColour (jobColour (theme, keys[(size_t)note].job, false));
            g.fillRect (r);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawRect (r, 0.5f);
            if (selected (note))
            {
                g.setColour (theme.get ("text").withAlpha (0.35f));
                g.fillRect (r.withTrimmedTop (labels * 0.62f));
            }
            paintHeard (g, theme, note, r.withTrimmedTop (labels * 0.8f).reduced (2.0f, 3.0f));
            if (note % 12 == 0)
            {
                g.setColour (theme.get ("textDim"));
                g.setFont (11.0f);
                g.drawText (keyName (note), juce::Rectangle<float> ((float)white * w, labels, w * 3.0f, 14.0f),
                            juce::Justification::centredLeft);
            }
            ++white;
        }
        for (int note = 0; note < 128; ++note)
            if (isBlack (note))
            {
                const auto r = blackBounds (note);
                g.setColour (jobColour (theme, keys[(size_t)note].job, true));
                g.fillRect (r);
                g.setColour (juce::Colours::black);
                g.drawRect (r, 0.5f);
                if (selected (note))
                {
                    g.setColour (theme.get ("text").withAlpha (0.45f));
                    g.fillRect (r.withTrimmedTop (r.getHeight() * 0.5f));
                }
                paintHeard (g, theme, note, r.withTrimmedTop (r.getHeight() * 0.7f).reduced (1.0f, 2.0f));
            }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        anchor = noteAt (e.position);
        panel.selectKeys (anchor, anchor);
    }
    void mouseDrag (const juce::MouseEvent& e) override { panel.selectKeys (anchor, noteAt (e.position)); }
    void mouseMove (const juce::MouseEvent& e) override { setTooltip (panel.describeKey (noteAt (e.position))); }

private:
    /** A key Apply would change: a dot of the colour it would take. */
    void paintHeard (juce::Graphics& g, const Theme& theme, int note, juce::Rectangle<float> r) const
    {
        InstrumentProfiles::Mode mode;
        if (panel.reading.has_value())
        {
            auto applied = panel.profile;
            manualreading::apply (applied, *panel.reading);
            mode = applied.modes[(size_t)panel.mode];
        }
        else if (panel.heard.has_value() && panel.heardMode == panel.mode)
        {
            mode = panel.current();
            keyprobe::merge (mode, *panel.heard);
        }
        else
            return;
        if (mode.keys[(size_t)note] == panel.current().keys[(size_t)note])
            return;
        const auto size = juce::jmin (r.getWidth(), r.getHeight());
        const auto dot = r.withSizeKeepingCentre (size, size);
        g.setColour (jobColour (theme, mode.keys[(size_t)note].job, false));
        g.fillEllipse (dot);
        g.setColour (juce::Colours::black);
        g.drawEllipse (dot, 1.0f);
    }

    float whiteWidth() const { return (float)getWidth() / (float)whiteKeys; }
    juce::Rectangle<float> blackBounds (int note) const
    {
        // Between the white keys either side of it.
        int whitesBefore = 0;
        for (int n = 0; n < note; ++n)
            whitesBefore += isBlack (n) ? 0 : 1;
        const auto w = whiteWidth();
        return { (float)whitesBefore * w - w * 0.32f, 0.0f, w * 0.64f, ((float)getHeight() - 14.0f) * 0.62f };
    }

    LearnInstrumentPanel& panel;
    int anchor = 0;
};

LearnInstrumentPanel::LearnInstrumentPanel (const juce::String& instrumentName, const std::optional<Profile>& start)
    : name (instrumentName), profile (start.value_or (Profile { { InstrumentProfiles::Mode {} } }))
{
    keyboard = std::make_unique<Keyboard> (*this);
    intro.setText ("What each key of " + name +
                       " does, mode by mode. Spinus writes notes only on the keys that play, so a song never hits a "
                       "key that switches the instrument. Drag across keys to select them, then mark them.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    intro.setMinimumHorizontalScale (1.0f);
    modeLabel.setText ("Mode:", juce::dontSendNotification);
    enterLabel.setText ("How to enter it:", juce::dontSendNotification);
    goodForLabel.setText ("Good for:", juce::dontSendNotification);
    modeBox.setTooltip ("The instrument's playing modes: most have one; a sampled guitar may have a strummer mode");
    modeBox.onChange = [this]
    {
        if (modeBox.getSelectedItemIndex() >= 0)
            showMode (modeBox.getSelectedItemIndex());
    };
    addModeButton.setTooltip ("Another playing mode of this instrument (a strummer, a riff player)");
    addModeButton.onClick = [this] { addMode ("Mode " + juce::String ((int)profile.modes.size() + 1)); };
    removeModeButton.setTooltip ("Remove this mode");
    removeModeButton.onClick = [this] { removeMode(); };
    modeName.setTooltip ("The mode's name, as its manual calls it");
    enterEditor.setTextToShowWhenEmpty ("as the instrument opens", juce::Colours::grey);
    enterEditor.setTooltip ("How to get to this mode: a button in the instrument's window, a preset, or a key");
    enterKeyBox.setTooltip ("A key that switches the instrument to this mode, if its manual names one");
    enterKeyBox.addItem ("No key", 1);
    for (int note = 0; note < 128; ++note)
        enterKeyBox.addItem ("Key " + keyName (note) + " (MIDI " + juce::String (note) + ")", note + 2);
    goodForEditor.setTextToShowWhenEmpty ("which parts it suits, e.g. strummed rhythm, country and folk",
                                          juce::Colours::grey);
    voicesBox.addItemList ({ "Chords or one note: not known", "Plays chords", "Plays one note at a time" }, 1);
    for (auto* e : { &modeName, &enterEditor, &goodForEditor })
        e->onTextChange = [this] { fieldsChanged(); };
    enterKeyBox.onChange = [this] { fieldsChanged(); };
    voicesBox.onChange = [this] { fieldsChanged(); };

    whatEditor.setTextToShowWhenEmpty ("what the keys do, e.g. palm mute", juce::Colours::grey);
    whatEditor.setTooltip ("For switch and special keys: what they do, in a few words");
    playsButton.setTooltip ("These keys sound as notes: Spinus writes on them");
    switchButton.setTooltip ("These keys change the articulation or the mode instead of sounding (palm mute, slide)");
    specialButton.setTooltip ("These keys have another job in this mode (chord keys, strum patterns, mutes)");
    unusedButton.setTooltip ("These keys do nothing");
    unknownButton.setTooltip ("Spinus does not know what these keys do");
    playsButton.onClick = [this] { markSelected (Job::plays); };
    switchButton.onClick = [this] { markSelected (Job::switches, whatEditor.getText()); };
    specialButton.onClick = [this] { markSelected (Job::special, whatEditor.getText()); };
    unusedButton.onClick = [this] { markSelected (Job::unused); };
    unknownButton.onClick = [this] { markSelected (Job::unknown); };
    forgetButton.setTooltip ("Remove everything Spinus knows of this instrument (kept when you Save)");
    forgetButton.onClick = [this] { forget(); };
    exportButton.setTooltip ("Save what Spinus knows of this instrument as a file to share with others");
    exportButton.onClick = [this]
    {
        if (onExport)
            onExport();
    };
    importButton.setTooltip ("Read a profile someone exported, as a proposal to Apply");
    importButton.onClick = [this]
    {
        if (onImport)
            onImport();
    };
    cancelButton.onClick = [this]
    {
        if (onCancel)
            onCancel();
    };
    saveButton.setTooltip ("Keep this; Listen, Save this sound's Play and the songs keep to the keys that play");
    saveButton.onClick = [this] { save(); };
    listenButton.setTooltip ("Spinus plays every key of this mode and listens: which play, which switch, which do "
                             "something else. A few minutes; what it hears is shown for you to Apply");
    listenButton.onClick = [this]
    {
        if (listening)
        {
            if (onStopListening)
                onStopListening();
        }
        else if (onListen)
        {
            heardMode = mode;
            onListen (current().enterKey);
        }
    };
    askButton.setTooltip ("Read the names the instrument gives its keys (a drum map's \"Kick\", a key switch's \"Palm "
                          "mute\"). Few instruments name them; what it says is shown for you to Apply");
    askButton.onClick = [this]
    {
        if (! listening && onAsk)
        {
            heardMode = mode;
            onAsk();
        }
    };
    applyButton.setTooltip ("Take what was found (the keys you set stay yours)");
    applyButton.onClick = [this] { applyHeard(); };
    discardButton.setTooltip ("Drop what was found");
    discardButton.onClick = [this] { discardHeard(); };
    heardLabel.setJustificationType (juce::Justification::centredLeft);
    manualsLabel.setText ("Manuals:", juce::dontSendNotification);
    manualsBox.setTooltip ("The instrument's manuals, for Read the manual: PDF files or web pages, official or not");
    removeManualButton.setTooltip ("Remove the manual shown");
    removeManualButton.onClick = [this] { removeManual (manualsBox.getSelectedItemIndex()); };
    manualEditor.setTextToShowWhenEmpty ("a web page: https://...", juce::Colours::grey);
    manualEditor.onReturnKey = [this] { addManual (manualEditor.getText()); };
    addManualButton.setTooltip ("Add the web page or file typed on the left");
    addManualButton.onClick = [this] { addManual (manualEditor.getText()); };
    chooseManualButton.setTooltip ("A manual on this computer: a PDF, or a web page saved as a file");
    chooseManualButton.onClick = [this]
    {
        if (onChooseManual)
            onChooseManual();
    };
    readButton.onClick = [this]
    {
        if (onRead && ! readingNow && readWhyNot.isEmpty() && ! profile.manuals.empty())
            onRead();
    };
    for (auto* c : std::initializer_list<juce::Component*> {
             &intro,         &modeLabel,          &modeBox,         &addModeButton,  &removeModeButton,
             &modeName,      &enterLabel,         &enterEditor,     &enterKeyBox,    &goodForLabel,
             &goodForEditor, &voicesBox,          keyboard.get(),   &selectionLabel, &whatEditor,
             &playsButton,   &switchButton,       &specialButton,   &unusedButton,   &unknownButton,
             &summary,       &forgetButton,       &cancelButton,    &saveButton,     &listenButton,
             &askButton,     &heardLabel,         &manualsLabel,    &manualsBox,     &removeManualButton,
             &manualEditor,  &chooseManualButton, &addManualButton, &readButton,     &exportButton,
             &importButton })
        addAndMakeVisible (c);
    addChildComponent (applyButton);
    addChildComponent (discardButton);
    setSize (980, 504);
    updateManuals();
    showMode (0);
}

LearnInstrumentPanel::~LearnInstrumentPanel() = default;

InstrumentProfiles::Mode& LearnInstrumentPanel::current()
{
    if (profile.modes.empty())
        profile.modes.emplace_back();
    return profile.modes[(size_t)juce::jlimit (0, (int)profile.modes.size() - 1, mode)];
}

std::optional<InstrumentProfiles::Profile> LearnInstrumentPanel::getProfile() const
{
    if (forgotten)
        return std::nullopt;
    return profile;
}

void LearnInstrumentPanel::showMode (int index)
{
    mode = juce::jlimit (0, juce::jmax (0, (int)profile.modes.size() - 1), index);
    updateFields();
}

void LearnInstrumentPanel::setListening (const juce::String& progress)
{
    listening = progress.isNotEmpty();
    listenButton.setButtonText (listening ? "Stop listening" : "Listen to every key");
    if (listening)
        heardLabel.setText (progress, juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &modeBox, &addModeButton, &removeModeButton, &askButton })
        c->setEnabled (! listening);
    if (! listening)
        removeModeButton.setEnabled (profile.modes.size() > 1);
}

void LearnInstrumentPanel::setHeard (std::optional<std::array<InstrumentProfiles::Key, 128>> keys,
                                     const juce::String& problem, From from)
{
    setListening ({});
    reading.reset();
    heard = std::move (keys);
    heardFrom = from;
    const auto plugin = from == From::plugin;
    if (heard.has_value() && std::all_of (heard->begin(), heard->end(),
                                          [] (const InstrumentProfiles::Key& k) { return k.job == Job::unknown; }))
        heard.reset(); // nothing to propose
    if (! heard.has_value())
        heardLabel.setText (plugin
                                ? (problem.isNotEmpty() ? "No answer: " + problem
                                                        : juce::String ("The instrument names none of its keys (most "
                                                                        "don't). Listen to every key instead."))
                                : "Nothing heard: " + problem,
                            juce::dontSendNotification);
    else
    {
        int counts[5] = {};
        for (const auto& key : *heard)
            ++counts[(int)key.job];
        auto merged = profile.modes[(size_t)juce::jlimit (0, (int)profile.modes.size() - 1, heardMode)];
        const auto changes = keyprobe::merge (merged, *heard);
        juce::StringArray said;
        if (plugin)
        {
            said.add (juce::String (counts[(int)Job::plays] + counts[(int)Job::switches]) + " keys named");
            if (counts[(int)Job::switches] > 0)
                said.add (juce::String (counts[(int)Job::switches]) + " of them switches");
        }
        else
        {
            said.add (juce::String (counts[(int)Job::plays]) + " keys play");
            if (counts[(int)Job::switches] > 0)
                said.add (juce::String (counts[(int)Job::switches]) + " switch");
            if (counts[(int)Job::special] > 0)
                said.add (juce::String (counts[(int)Job::special]) + " do something else");
            if (counts[(int)Job::unused] > 0)
                said.add (juce::String (counts[(int)Job::unused]) + " are silent");
        }
        heardLabel.setText (juce::String (plugin ? "The instrument says: " : "Heard: ") + said.joinIntoString (", ") +
                                ". Apply changes " + juce::String (changes) + (changes == 1 ? " key" : " keys") +
                                " (marked with a dot).",
                            juce::dontSendNotification);
        if (heardMode != mode)
            showMode (heardMode);
    }
    applyButton.setVisible (heard.has_value());
    discardButton.setVisible (heard.has_value());
    keyboard->repaint();
}

void LearnInstrumentPanel::applyHeard()
{
    if (reading.has_value())
    {
        const auto changes = manualreading::apply (profile, *reading);
        const auto imported = reading->file.isNotEmpty();
        if (! imported) // the manuals were read for this version
            for (auto& manual : profile.manuals)
                manual.version = instrumentVersion;
        forgotten = false;
        discardHeard();
        updateManuals();
        heardLabel.setText ("Applied what " + juce::String (imported ? "the file" : "the manual") + " says (" +
                                juce::String (changes) + (changes == 1 ? " change" : " changes") + "). Save keeps it.",
                            juce::dontSendNotification);
        return;
    }
    if (! heard.has_value())
        return;
    showMode (heardMode);
    keyprobe::merge (current(), *heard);
    forgotten = false;
    discardHeard();
    heardLabel.setText (heardFrom == From::plugin ? "Applied what the instrument says. Save keeps it."
                                                  : "Applied what was heard. Save keeps it.",
                        juce::dontSendNotification);
}

void LearnInstrumentPanel::discardHeard()
{
    heard.reset();
    reading.reset();
    applyButton.setVisible (false);
    discardButton.setVisible (false);
    heardLabel.setText ({}, juce::dontSendNotification);
    updateFields();
}

void LearnInstrumentPanel::addManual (const juce::String& typed)
{
    auto source = typed.trim().unquoted();
    if (source.isEmpty())
        return;
    if (! source.startsWith ("https://") && ! source.startsWith ("http://"))
    {
        if (source.startsWith ("www."))
            source = "https://" + source;
        else if (! juce::File::isAbsolutePath (source) || ! juce::File (source).existsAsFile())
        {
            heardLabel.setText ("A manual is a web page's address (https://...) or a file on this computer: use "
                                "Choose file....",
                                juce::dontSendNotification);
            return;
        }
    }
    if (std::none_of (profile.manuals.begin(), profile.manuals.end(),
                      [&] (const InstrumentProfiles::Manual& m) { return m.source == source; }))
        profile.manuals.push_back ({ source, {} });
    manualEditor.clear();
    forgotten = false;
    updateManuals();
    manualsBox.setSelectedItemIndex ((int)profile.manuals.size() - 1, juce::dontSendNotification);
}

void LearnInstrumentPanel::removeManual (int index)
{
    if (index < 0 || index >= (int)profile.manuals.size())
        return;
    profile.manuals.erase (profile.manuals.begin() + index);
    updateManuals();
}

void LearnInstrumentPanel::setInstrumentVersion (const juce::String& version)
{
    instrumentVersion = version;
    updateManuals();
}

void LearnInstrumentPanel::setCanRead (const juce::String& whyNot)
{
    readWhyNot = whyNot;
    updateManuals();
}

juce::var LearnInstrumentPanel::getShared() const { return InstrumentProfiles::toShared (profile, name); }

bool LearnInstrumentPanel::importShared (const juce::var& shared, const juce::String& fileName)
{
    juce::String exportedAs, problem;
    const auto imported = InstrumentProfiles::fromShared (shared, exportedAs, problem);
    if (! imported.has_value())
    {
        heardLabel.setText (fileName + " was not imported: " + problem, juce::dontSendNotification);
        return false;
    }
    manualreading::Reading read;
    read.modes = imported->modes;
    read.file = fileName +
                (exportedAs.isNotEmpty() && exportedAs != name ? " (exported for " + exportedAs + ")" : juce::String());
    setReading (std::move (read));
    return true;
}

void LearnInstrumentPanel::setReadingProgress (const juce::String& progress)
{
    readingNow = progress.isNotEmpty();
    if (readingNow)
        heardLabel.setText (progress, juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &listenButton, &askButton })
        c->setEnabled (! readingNow);
    updateManuals();
}

void LearnInstrumentPanel::setReading (std::optional<manualreading::Reading> read, const juce::String& problem)
{
    setReadingProgress ({});
    heard.reset();
    reading = std::move (read);
    if (! reading.has_value())
        heardLabel.setText ("The manual was not read: " + problem, juce::dontSendNotification);
    else
    {
        manualreading::fitOctaves (*reading, profile);
        auto applied = profile;
        const auto modesBefore = applied.modes.size();
        const auto changes = manualreading::apply (applied, *reading);
        const auto added = (int)(applied.modes.size() - modesBefore);
        juce::String text =
            (reading->file.isNotEmpty() ? reading->file + " says: " : juce::String ("The manual says: ")) +
            manualreading::describe (*reading) + ". Apply changes " + juce::String (changes) +
            (changes == 1 ? " thing" : " things") +
            (added > 0 ? " (" + juce::String (added) + (added == 1 ? " new mode" : " new modes") + ")"
                       : juce::String()) +
            "; keys marked with a dot.";
        if (! reading->middleCSaid)
            text << (reading->octavesMoved != 0
                         ? " It doesn't say how it numbers octaves: its keys were moved " +
                               juce::String (std::abs (reading->octavesMoved)) +
                               (std::abs (reading->octavesMoved) == 1 ? " octave " : " octaves ") +
                               (reading->octavesMoved > 0 ? "up" : "down") + " to fit the keys Spinus knows."
                         : " It doesn't say how it numbers octaves (C3 taken as middle C): check the keys.");
        if (! reading->problems.isEmpty())
            text << " Not read: " << reading->problems.joinIntoString ("; ") << ".";
        heardLabel.setText (text, juce::dontSendNotification);
    }
    applyButton.setVisible (reading.has_value());
    discardButton.setVisible (reading.has_value());
    keyboard->repaint();
}

juce::String LearnInstrumentPanel::getManualsText() const
{
    if (profile.manuals.empty())
        return "No manual yet";
    juce::StringArray names;
    juce::String stale;
    for (const auto& manual : profile.manuals)
    {
        names.add (manual.isWebPage() ? manual.source.fromFirstOccurrenceOf ("://", false, false)
                                      : juce::File (manual.source).getFileName());
        if (manual.version.isNotEmpty() && instrumentVersion.isNotEmpty() && manual.version != instrumentVersion)
            stale = " (read for " + manual.version + "; the instrument is " + instrumentVersion +
                    " now: it may be out of date, read it again)";
    }
    return juce::String ((int)names.size()) + (names.size() == 1 ? " manual: " : " manuals: ") +
           names.joinIntoString (", ") + stale;
}

void LearnInstrumentPanel::updateManuals()
{
    const auto shown = juce::jmax (0, manualsBox.getSelectedItemIndex());
    manualsBox.clear (juce::dontSendNotification);
    for (size_t i = 0; i < profile.manuals.size(); ++i)
    {
        const auto& manual = profile.manuals[i];
        auto label = manual.isWebPage() ? manual.source.fromFirstOccurrenceOf ("://", false, false)
                                        : juce::File (manual.source).getFileName();
        if (manual.version.isNotEmpty() && instrumentVersion.isNotEmpty() && manual.version != instrumentVersion)
            label << " (read for " << manual.version << ")";
        manualsBox.addItem (label, (int)i + 1);
    }
    manualsBox.setTextWhenNothingSelected (getManualsText());
    if (! profile.manuals.empty())
        manualsBox.setSelectedItemIndex (juce::jmin (shown, (int)profile.manuals.size() - 1),
                                         juce::dontSendNotification);
    removeManualButton.setEnabled (! profile.manuals.empty());
    readButton.setEnabled (readWhyNot.isEmpty() && ! profile.manuals.empty() && ! readingNow);
    readButton.setTooltip (readWhyNot.isNotEmpty() ? readWhyNot
                           : profile.manuals.empty()
                               ? juce::String ("Add a manual first: a PDF on this computer or a web page")
                               : juce::String ("The AI reads the manuals and proposes the modes they name, how to "
                                               "enter each and what their keys do; shown for you to Apply"));
}

void LearnInstrumentPanel::addMode (const juce::String& modeName_)
{
    InstrumentProfiles::Mode added;
    added.name = modeName_;
    profile.modes.push_back (added);
    forgotten = false;
    showMode ((int)profile.modes.size() - 1);
}

void LearnInstrumentPanel::removeMode()
{
    if (profile.modes.size() <= 1)
        return;
    profile.modes.erase (profile.modes.begin() + mode);
    showMode (mode - 1);
}

void LearnInstrumentPanel::selectKeys (int from, int to)
{
    selectedFrom = juce::jlimit (0, 127, from);
    selectedTo = juce::jlimit (0, 127, to);
    const auto low = juce::jmin (selectedFrom, selectedTo), high = juce::jmax (selectedFrom, selectedTo);
    selectionLabel.setText ("Selected: " + (low == high ? keyName (low) : keyName (low) + " to " + keyName (high)) +
                                ". Mark as:",
                            juce::dontSendNotification);
    keyboard->repaint();
}

void LearnInstrumentPanel::markSelected (Job job, const juce::String& what)
{
    if (selectedFrom < 0)
        return;
    const auto words = job == Job::switches || job == Job::special ? what.trim() : juce::String();
    current().setKeys (selectedFrom, selectedTo, { job, words, InstrumentProfiles::Source::user });
    forgotten = false;
    updateFields();
}

void LearnInstrumentPanel::forget()
{
    forgotten = true;
    profile = Profile { { InstrumentProfiles::Mode {} } };
    showMode (0);
}

void LearnInstrumentPanel::save()
{
    if (onSave)
        onSave (getProfile());
}

juce::String LearnInstrumentPanel::describeKey (int note) const
{
    if (! juce::isPositiveAndBelow (note, 128) || profile.modes.empty())
        return {};
    const auto& key = profile.modes[(size_t)juce::jlimit (0, (int)profile.modes.size() - 1, mode)].keys[(size_t)note];
    return keyName (note) + " (MIDI " + juce::String (note) + "): " + jobWords (key.job) +
           (key.what.isNotEmpty() ? ", " + key.what : juce::String()) +
           (key.job != Job::unknown ? " (" + sourceWords (key.source) + ")" : juce::String());
}

juce::String LearnInstrumentPanel::getSummaryText() const
{
    if (forgotten)
        return "Forgotten: Save removes what Spinus knew of " + name + ", and it plays any note again.";
    const auto& m = profile.modes[(size_t)juce::jlimit (0, (int)profile.modes.size() - 1, mode)];
    int counts[5] = {};
    for (const auto& key : m.keys)
        ++counts[(int)key.job];
    juce::StringArray said;
    if (const auto playable = m.playable())
        said.add ("Plays " + playable->describe());
    else
        said.add ("Which keys play is not known: it plays any note");
    const auto count = [] (int n, const char* one, const char* many)
    { return juce::String (n) + " " + (n == 1 ? one : many); };
    if (counts[(int)Job::switches] > 0)
        said.add (count (counts[(int)Job::switches], "switch key", "switch keys"));
    if (counts[(int)Job::special] > 0)
        said.add (count (counts[(int)Job::special], "special key", "special keys"));
    if (counts[(int)Job::unused] > 0)
        said.add (count (counts[(int)Job::unused], "key does nothing", "keys do nothing"));
    return said.joinIntoString ("; ") + ".";
}

void LearnInstrumentPanel::updateFields()
{
    modeBox.clear (juce::dontSendNotification);
    for (size_t i = 0; i < profile.modes.size(); ++i)
        modeBox.addItem (profile.modes[i].name, (int)i + 1);
    modeBox.setSelectedItemIndex (mode, juce::dontSendNotification);
    const auto& m = current();
    modeName.setText (m.name, false);
    enterEditor.setText (m.enter, false);
    enterKeyBox.setSelectedId (m.enterKey + 2, juce::dontSendNotification);
    goodForEditor.setText (m.goodFor, false);
    voicesBox.setSelectedItemIndex ((int)m.voices, juce::dontSendNotification);
    removeModeButton.setEnabled (profile.modes.size() > 1);
    summary.setText (getSummaryText(), juce::dontSendNotification);
    keyboard->repaint();
}

void LearnInstrumentPanel::fieldsChanged()
{
    auto& m = current();
    m.name = modeName.getText().trim().isNotEmpty() ? modeName.getText().trim() : juce::String ("Normal");
    m.enter = enterEditor.getText().trim();
    m.enterKey = enterKeyBox.getSelectedId() - 2;
    m.goodFor = goodForEditor.getText().trim();
    m.voices = (Voices)juce::jmax (0, voicesBox.getSelectedItemIndex());
    modeBox.changeItemText (mode + 1, m.name);
    forgotten = false;
    summary.setText (getSummaryText(), juce::dontSendNotification);
}

void LearnInstrumentPanel::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    heardLabel.setColour (juce::Label::textColourId, theme.get ("textDim"));
    for (auto* l : { &intro, &modeLabel, &enterLabel, &goodForLabel, &selectionLabel, &manualsLabel })
        l->setColour (juce::Label::textColourId, theme.get ("text"));
    summary.setColour (juce::Label::textColourId, theme.get ("accent"));
    // Each button the colour of the keys it marks: the keyboard's legend.
    const std::pair<juce::TextButton*, Job> marks[] = { { &playsButton, Job::plays },
                                                        { &switchButton, Job::switches },
                                                        { &specialButton, Job::special },
                                                        { &unusedButton, Job::unused },
                                                        { &unknownButton, Job::unknown } };
    for (const auto& [button, job] : marks)
    {
        const auto colour = jobColour (theme, job, false);
        button->setColour (juce::TextButton::buttonColourId, colour);
        button->setColour (juce::TextButton::textColourOffId, colour.contrasting (0.8f));
    }
}

void LearnInstrumentPanel::resized()
{
    auto area = getLocalBounds().reduced (12);
    intro.setBounds (area.removeFromTop (40));
    area.removeFromTop (6);
    auto row = [&area] (int height = 26)
    {
        auto r = area.removeFromTop (height);
        area.removeFromTop (6);
        return r;
    };
    {
        auto r = row();
        modeLabel.setBounds (r.removeFromLeft (50));
        modeBox.setBounds (r.removeFromLeft (200));
        r.removeFromLeft (8);
        modeName.setBounds (r.removeFromLeft (200));
        r.removeFromLeft (8);
        addModeButton.setBounds (r.removeFromLeft (100));
        r.removeFromLeft (8);
        removeModeButton.setBounds (r.removeFromLeft (120));
    }
    {
        auto r = row();
        enterLabel.setBounds (r.removeFromLeft (110));
        enterKeyBox.setBounds (r.removeFromRight (220));
        r.removeFromRight (8);
        enterEditor.setBounds (r);
    }
    {
        auto r = row();
        goodForLabel.setBounds (r.removeFromLeft (110));
        voicesBox.setBounds (r.removeFromRight (220));
        r.removeFromRight (8);
        goodForEditor.setBounds (r);
    }
    {
        auto r = row();
        manualsLabel.setBounds (r.removeFromLeft (110));
        manualsBox.setBounds (r.removeFromLeft (200));
        r.removeFromLeft (6);
        removeManualButton.setBounds (r.removeFromLeft (76));
        r.removeFromLeft (8);
        readButton.setBounds (r.removeFromRight (140));
        r.removeFromRight (8);
        addManualButton.setBounds (r.removeFromRight (56));
        r.removeFromRight (6);
        chooseManualButton.setBounds (r.removeFromRight (110));
        r.removeFromRight (6);
        manualEditor.setBounds (r);
    }
    {
        auto r = area.removeFromBottom (28);
        saveButton.setBounds (r.removeFromRight (90));
        r.removeFromRight (8);
        cancelButton.setBounds (r.removeFromRight (90));
        forgetButton.setBounds (r.removeFromLeft (180));
        r.removeFromLeft (8);
        exportButton.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (8);
        importButton.setBounds (r.removeFromLeft (90));
        area.removeFromBottom (6);
    }
    summary.setBounds (area.removeFromBottom (22));
    area.removeFromBottom (4);
    {
        auto r = area.removeFromBottom (26);
        area.removeFromBottom (6);
        listenButton.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (8);
        askButton.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (8);
        discardButton.setBounds (r.removeFromRight (90));
        r.removeFromRight (8);
        applyButton.setBounds (r.removeFromRight (90));
        r.removeFromRight (8);
        heardLabel.setBounds (r);
    }
    {
        auto r = area.removeFromBottom (26);
        area.removeFromBottom (6);
        selectionLabel.setBounds (r.removeFromLeft (220));
        for (auto* b : { &playsButton, &switchButton, &specialButton, &unusedButton, &unknownButton })
        {
            b->setBounds (r.removeFromLeft (84));
            r.removeFromLeft (6);
        }
        whatEditor.setBounds (r);
    }
    keyboard->setBounds (area);
}

// --- The window ----------------------------------------------------------------------------------------------------

namespace
{
std::vector<LearnInstrumentWindow*>& openWindows()
{
    static std::vector<LearnInstrumentWindow*> windows;
    return windows;
}
} // namespace

void LearnInstrumentWindow::open (const juce::File& indexFolder, const juce::String& instrument,
                                  const juce::String& name, juce::Component* near, Saved saved,
                                  std::optional<Listening> listening, ai::AiService* service)
{
    for (auto* w : openWindows())
        if (w->instrument == instrument)
        {
            w->toFront (true);
            return;
        }
    auto* window =
        new LearnInstrumentWindow (indexFolder, instrument, name, std::move (saved), std::move (listening), service);
    if (near != nullptr && near->isOnDesktop())
        keepAboveOwner (*window, *near);
    window->setVisible (true);
}

LearnInstrumentWindow::LearnInstrumentWindow (const juce::File& folder, const juce::String& key,
                                              const juce::String& instrumentName, Saved s, std::optional<Listening> l,
                                              ai::AiService* aiService)
    : juce::DocumentWindow ("Learn " + instrumentName, juce::Colours::black, juce::DocumentWindow::closeButton, false),
      indexFolder (folder), instrument (key), saved (std::move (s)), listening (std::move (l)), service (aiService),
      name (instrumentName)
{
    InstrumentProfiles profiles (indexFolder);
    profiles.load();
    auto* panel = new LearnInstrumentPanel (name, profiles.get (instrument));
    panel->onCancel = [this] { finish(); };
    panel->onSave = [this] (std::optional<InstrumentProfiles::Profile> profile)
    {
        InstrumentProfiles all (indexFolder);
        all.load();
        all.set (instrument, std::move (profile));
        all.save();
        if (saved)
            saved (all.playable (instrument));
        finish();
    };
    panel->onListen = [this] (int enterKey) { listen (enterKey); };
    panel->onStopListening = [this] { stopping->store (true); };
    panel->onAsk = [this] { ask(); };
    panel->onRead = [this] { readManuals(); };
    panel->onChooseManual = [this] { chooseManual(); };
    panel->onExport = [this] { exportProfile(); };
    panel->onImport = [this] { importProfile(); };
    if (listening.has_value()) // the plugin's version, the first part of its fingerprint
        panel->setInstrumentVersion (listening->source.fingerprint.upToFirstOccurrenceOf ("|", false, false));
    panel->setCanRead (service == nullptr || ! service->isEnabled()
                           ? "Reading a manual needs the AI: turn it on in Options > AI and add a connection"
                           : juce::String());
    setUsingNativeTitleBar (true);
    setContentOwned (panel, true);
    setResizable (true, false);
    addToDesktop();
    openWindows().push_back (this);
}

LearnInstrumentWindow::~LearnInstrumentWindow()
{
    std::erase (openWindows(), this);
    *alive = false;
    stopping->store (true);
    task = nullptr; // stops the helper and waits
}

void LearnInstrumentWindow::listen (int enterKey)
{
    auto* panel = dynamic_cast<LearnInstrumentPanel*> (getContentComponent());
    if (panel == nullptr || task != nullptr)
        return;
    if (! listening.has_value())
    {
        panel->setHeard (std::nullopt, "choose this instrument for a part first (Sounds for each part), then open "
                                       "Learn this instrument from its menu there: Spinus listens to it with that "
                                       "sound.");
        return;
    }
    stopping->store (false);
    panel->setListening ("Listening: loading the instrument...");
    auto found = std::make_shared<std::optional<std::array<InstrumentProfiles::Key, 128>>>();
    auto problem = std::make_shared<juce::String>();
    task = std::make_unique<BackgroundTask> (
        "Spinus listens to every key",
        [l = *listening, enterKey, found, problem, token = stopping,
         safe = juce::Component::SafePointer<LearnInstrumentPanel> (panel)] (const std::function<bool()>& shouldStop)
        {
            *found = parts::probeInHelper (
                l.helper, l.source, l.preset, l.state, enterKey, [&] { return shouldStop() || token->load(); },
                [safe] (int done, int total)
                {
                    juce::MessageManager::callAsync (
                        [safe, done, total]
                        {
                            if (safe != nullptr)
                                safe->setListening ("Listening: " + juce::String (done) + " of " +
                                                    juce::String (total) + " keys...");
                        });
                },
                *problem);
            return juce::Result::ok();
        },
        [this, a = alive, found, problem] (const juce::Result&)
        {
            juce::MessageManager::callAsync (
                [this, a, found, problem]
                {
                    if (! *a)
                        return;
                    task = nullptr;
                    if (auto* p = dynamic_cast<LearnInstrumentPanel*> (getContentComponent()))
                        p->setHeard (*found, *problem);
                });
        });
}

void LearnInstrumentWindow::ask()
{
    auto* panel = dynamic_cast<LearnInstrumentPanel*> (getContentComponent());
    if (panel == nullptr || task != nullptr)
        return;
    if (! listening.has_value())
    {
        panel->setHeard (std::nullopt,
                         "choose this instrument for a part first (Sounds for each part), then open Learn this "
                         "instrument from its menu there.",
                         LearnInstrumentPanel::From::plugin);
        return;
    }
    stopping->store (false);
    panel->setListening ("Asking the instrument...");
    auto found = std::make_shared<std::optional<std::array<InstrumentProfiles::Key, 128>>>();
    auto problem = std::make_shared<juce::String>();
    task = std::make_unique<BackgroundTask> (
        "Spinus asks the instrument for its keys' names",
        [l = *listening, found, problem, token = stopping] (const std::function<bool()>& shouldStop)
        {
            *found = parts::keyNamesInHelper (
                l.helper, l.source, l.preset, l.state, [&] { return shouldStop() || token->load(); }, *problem);
            return juce::Result::ok();
        },
        [this, a = alive, found, problem] (const juce::Result&)
        {
            juce::MessageManager::callAsync (
                [this, a, found, problem]
                {
                    if (! *a)
                        return;
                    task = nullptr;
                    if (auto* p = dynamic_cast<LearnInstrumentPanel*> (getContentComponent()))
                        p->setHeard (*found, *problem, LearnInstrumentPanel::From::plugin);
                });
        });
}

void LearnInstrumentWindow::chooseManual()
{
    chooser = std::make_unique<core::FileDialog> ("A manual for " + name, juce::File(), "*.pdf;*.html;*.htm;*.txt;*.md",
                                                  this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, a = alive] (const core::FileDialog& fc)
                          {
                              if (! *a || fc.getResult() == juce::File())
                                  return;
                              if (auto* p = dynamic_cast<LearnInstrumentPanel*> (getContentComponent()))
                                  p->addManual (fc.getResult().getFullPathName());
                          });
}

void LearnInstrumentWindow::exportProfile()
{
    auto* panel = dynamic_cast<LearnInstrumentPanel*> (getContentComponent());
    if (panel == nullptr)
        return;
    const auto shared = panel->getShared();
    const auto file = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                          .getChildFile (juce::File::createLegalFileName (name + " profile.json"));
    chooser = std::make_unique<juce::FileChooser> ("Export the profile of " + name, file, "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [shared, a = alive] (const juce::FileChooser& fc)
                          {
                              if (*a && fc.getResult() != juce::File())
                                  fc.getResult().replaceWithText (juce::JSON::toString (shared));
                          });
}

void LearnInstrumentWindow::importProfile()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a profile for " + name, juce::File(), "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, a = alive] (const juce::FileChooser& fc)
                          {
                              if (! *a || fc.getResult() == juce::File())
                                  return;
                              if (auto* p = dynamic_cast<LearnInstrumentPanel*> (getContentComponent()))
                                  p->importShared (juce::JSON::parse (fc.getResult()), fc.getResult().getFileName());
                          });
}

void LearnInstrumentWindow::readManuals()
{
    auto* panel = dynamic_cast<LearnInstrumentPanel*> (getContentComponent());
    if (panel == nullptr || service == nullptr)
        return;
    juce::Array<juce::var> manuals;
    for (const auto& manual : panel->getManuals())
    {
        auto* m = new juce::DynamicObject();
        m->setProperty (manual.isWebPage() ? "url" : "file", manual.source);
        manuals.add (juce::var (m));
    }
    auto* params = new juce::DynamicObject();
    params->setProperty ("instrument", name);
    params->setProperty ("manuals", manuals);
    panel->setReadingProgress ("The AI is reading the manual...");
    ai::runJob (
        *service, "ai.v1.instrument.readManual", juce::var (params), alive, {},
        [this, a = alive] (const juce::var& result, const juce::String& error)
        {
            if (! *a)
                return;
            if (auto* p = dynamic_cast<LearnInstrumentPanel*> (getContentComponent()))
            {
                auto read = error.isEmpty() ? manualreading::fromVar (result) : std::nullopt;
                p->setReading (std::move (read), error.isNotEmpty() ? error : juce::String ("it named no mode."));
            }
        },
        [this, a = alive] (double, double, const juce::String& step)
        {
            if (! *a || step.isEmpty())
                return;
            if (auto* p = dynamic_cast<LearnInstrumentPanel*> (getContentComponent()))
                p->setReadingProgress (step);
        });
}

void LearnInstrumentWindow::closeButtonPressed() { finish(); }

void LearnInstrumentWindow::finish()
{
    // Out of the button's own click first.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<LearnInstrumentWindow> (this)]
                                     { delete safe.getComponent(); });
    setVisible (false);
}

int LearnInstrumentWindow::getNumOpen() { return (int)openWindows().size(); }

void LearnInstrumentWindow::closeAll()
{
    while (! openWindows().empty())
        delete openWindows().back();
}

LearnInstrumentPanel* LearnInstrumentWindow::lastOpened()
{
    return openWindows().empty() ? nullptr
                                 : dynamic_cast<LearnInstrumentPanel*> (openWindows().back()->getContentComponent());
}

} // namespace spinus::ui
