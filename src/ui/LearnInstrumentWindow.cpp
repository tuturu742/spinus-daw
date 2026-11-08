#include "LearnInstrumentWindow.h"

#include "Theme.h"
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
        if (! panel.heard.has_value() || panel.heardMode != panel.mode)
            return;
        const auto& heard = *panel.heard;
        auto mode = panel.current();
        keyprobe::merge (mode, heard);
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
    for (auto* c : std::initializer_list<juce::Component*> {
             &intro,         &modeLabel,      &modeBox,     &addModeButton, &removeModeButton, &modeName,
             &enterLabel,    &enterEditor,    &enterKeyBox, &goodForLabel,  &goodForEditor,    &voicesBox,
             keyboard.get(), &selectionLabel, &whatEditor,  &playsButton,   &switchButton,     &specialButton,
             &unusedButton,  &unknownButton,  &summary,     &forgetButton,  &cancelButton,     &saveButton,
             &listenButton,  &askButton,      &heardLabel })
        addAndMakeVisible (c);
    addChildComponent (applyButton);
    addChildComponent (discardButton);
    setSize (980, 470);
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
    if (! heard.has_value())
        return;
    const auto keys = *heard; // showMode may change what is shown
    showMode (heardMode);
    keyprobe::merge (current(), keys);
    forgotten = false;
    discardHeard();
    heardLabel.setText (heardFrom == From::plugin ? "Applied what the instrument says. Save keeps it."
                                                  : "Applied what was heard. Save keeps it.",
                        juce::dontSendNotification);
}

void LearnInstrumentPanel::discardHeard()
{
    heard.reset();
    applyButton.setVisible (false);
    discardButton.setVisible (false);
    heardLabel.setText ({}, juce::dontSendNotification);
    updateFields();
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
    for (auto* l : { &intro, &modeLabel, &enterLabel, &goodForLabel, &selectionLabel })
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
        auto r = area.removeFromBottom (28);
        saveButton.setBounds (r.removeFromRight (90));
        r.removeFromRight (8);
        cancelButton.setBounds (r.removeFromRight (90));
        forgetButton.setBounds (r.removeFromLeft (180));
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
                                  std::optional<Listening> listening)
{
    for (auto* w : openWindows())
        if (w->instrument == instrument)
        {
            w->toFront (true);
            return;
        }
    auto* window = new LearnInstrumentWindow (indexFolder, instrument, name, std::move (saved), std::move (listening));
    if (near != nullptr && near->isOnDesktop())
        keepAboveOwner (*window, *near);
    window->setVisible (true);
}

LearnInstrumentWindow::LearnInstrumentWindow (const juce::File& folder, const juce::String& key,
                                              const juce::String& name, Saved s, std::optional<Listening> l)
    : juce::DocumentWindow ("Learn " + name, juce::Colours::black, juce::DocumentWindow::closeButton, false),
      indexFolder (folder), instrument (key), saved (std::move (s)), listening (std::move (l))
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
