#include "DescribeSongPanel.h"

#include "Theme.h"
#include "ai/AnalysisJobs.h"

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

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}
} // namespace

DescribeSongPanel::DescribeSongPanel (std::function<ai::AiService*()> service) : getService (std::move (service))
{
    intro.setText ("Describe the song you want in a few words: its genre, mood, tempo, key, the instruments you "
                   "hear. Spinus reads it on this computer (no AI connection needed), then Parts and Write a song "
                   "work from it with the sounds you have.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    intro.setMinimumHorizontalScale (1.0f);
    textBox.setMultiLine (true, true);
    textBox.setReturnKeyStartsNewLine (true);
    textBox.setScrollbarsShown (true);
    textBox.setTextToShowWhenEmpty ("e.g. A sunny, upbeat indie pop song at 118 BPM in G major, with acoustic "
                                    "guitar, handclaps and a catchy whistled lead",
                                    juce::Colours::grey);
    textBox.onTextChange = [this] { updateButtons(); };
    lyricsLabel.setText ("Lyrics (optional): the song's words, a section tag on its own line before each part",
                         juce::dontSendNotification);
    lyricsBox.setMultiLine (true, true);
    lyricsBox.setReturnKeyStartsNewLine (true);
    lyricsBox.setScrollbarsShown (true);
    lyricsBox.setTextToShowWhenEmpty ("[Verse 1]\nThe city lights are fading out\n...\n\n[Chorus]\nHold on to the "
                                      "night\n\n[Guitar Solo]",
                                      juce::Colours::grey);
    lyricsBox.setTooltip ("Spinus sizes the song's sections to the words and writes a vocal line for them, played by "
                          "an instrument (it sings nothing). A tag with no lines is an instrumental section; a tag "
                          "used again with no lines repeats its words. Lyrics without tags are not read: add them, or "
                          "let the AI tag them for you to check");
    noVocalsButton.setTooltip ("An instrumental song: the lyrics stay in the box but are not used");
    noVocalsButton.onClick = [this] { updateButtons(); };
    aiButton.setTooltip ("Ask your AI connection (Options > AI) to read the description; Spinus checks its reading "
                         "and writes every note itself. Off: Spinus's own rules, nothing is sent anywhere");
    readButton.setTooltip ("Read the description into a style to write a song in");
    readButton.onClick = [this] { read(); };
    againButton.setTooltip ("Put back the description of your reference songs (your changes go)");
    againButton.onClick = [this]
    {
        textBox.setText (generated, false);
        read();
    };
    for (auto* l : { &understood, &status })
    {
        l->setJustificationType (juce::Justification::topLeft);
        l->setMinimumHorizontalScale (1.0f);
    }
    proposalLabel.setText ("The AI's tags for your lyrics (your lines as you wrote them): check them, then",
                           juce::dontSendNotification);
    for (auto* box : { &proposalBox, &details })
    {
        box->setMultiLine (true, true);
        box->setReadOnly (true);
        box->setScrollbarsShown (true);
        box->setCaretVisible (false);
    }
    useTagsButton.setTooltip ("Put the AI's tagged lyrics in the lyrics box (Ctrl+Z there takes them back) and read "
                              "again");
    useTagsButton.onClick = [this] { useProposal(); };
    keepMineButton.setTooltip ("Leave your lyrics as they are; without tags they are not read");
    keepMineButton.onClick = [this] { keepMine(); };
    for (auto* c :
         std::initializer_list<juce::Component*> { &proposalLabel, &proposalBox, &useTagsButton, &keepMineButton })
        addChildComponent (c);

    for (auto* c :
         std::initializer_list<juce::Component*> { &intro, &textBox, &lyricsLabel, &lyricsBox, &noVocalsButton,
                                                   &aiButton, &readButton, &understood, &status, &details })
        addAndMakeVisible (c);
    addChildComponent (againButton);
    updateButtons();
}

DescribeSongPanel::~DescribeSongPanel()
{
    *alive = false;
    if (auto* service = getService(); service != nullptr && job.isNotEmpty())
        service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

bool DescribeSongPanel::read()
{
    if (busy)
        return false;
    const auto text = textBox.getText().trim();
    if (text.isEmpty())
    {
        status.setText ("Describe the song first, in a few words.", juce::dontSendNotification);
        return false;
    }
    auto* service = getService();
    if (service == nullptr || ! service->isEnabled())
    {
        status.setText ("Reading a description needs Spinus's AI service, which runs on this computer (no AI "
                        "connection is needed): turn AI on in Options > AI.",
                        juce::dontSendNotification);
        return false;
    }
    setProposal ({});
    busy = true;
    const bool useAi = aiButton.getToggleState();
    status.setText (useAi ? "The AI is reading the description..." : "Reading the description...",
                    juce::dontSendNotification);
    updateButtons();
    auto params = object ({ { "text", text }, { "ai", useAi } });
    if (const auto words = lyricsBox.getText().trim(); words.isNotEmpty() && ! noVocalsButton.getToggleState())
        params.getDynamicObject()->setProperty ("lyrics", words);
    if (references.isObject())
    {
        params.getDynamicObject()->setProperty ("summary", references);
        params.getDynamicObject()->setProperty ("generated", generated);
    }
    ai::runJob (
        *service, "ai.v1.song.describe", params, alive, [this] (const juce::String& started) { job = started; },
        [this] (const juce::var& result, const juce::String& error)
        {
            job.clear();
            busy = false;
            if (error.isNotEmpty())
                status.setText ("The description could not be read: " + error, juce::dontSendNotification);
            else
            {
                show (result);
                if (onRead)
                    onRead (result);
            }
            updateButtons();
        });
    return true;
}

void DescribeSongPanel::show (const juce::var& read)
{
    summary = read;
    const auto& described = read["described"];
    understood.setText (described["understood"].toString() + ".", juce::dontSendNotification);
    juce::StringArray lines;
    if (const auto* plain = read["plain"].getArray())
        for (const auto& line : *plain)
            if (line.toString() != understood.getText()) // said above
                lines.add (line.toString());
    details.setText (lines.joinIntoString ("\n"), false);
    const auto by = described["readBy"].toString();
    setProposal (read["lyrics"]["proposal"].toString());
    const auto lyricsNote = read["lyrics"]["note"].toString();
    status.setText ((lyricsNote.isNotEmpty() ? lyricsNote + " " : juce::String()) +
                        (described["note"].toString().isNotEmpty() ? described["note"].toString() + " "
                         : by.isNotEmpty()                         ? "Read by the AI (" + by + "). "
                                                                   : juce::String()) +
                        "Next: 3. Parts, then 4. Write a song.",
                    juce::dontSendNotification);
}

void DescribeSongPanel::setProposal (const juce::String& text)
{
    proposal = text;
    proposalBox.setText (text, false);
    for (auto* c :
         std::initializer_list<juce::Component*> { &proposalLabel, &proposalBox, &useTagsButton, &keepMineButton })
        c->setVisible (text.isNotEmpty());
    updateButtons();
    resized();
}

void DescribeSongPanel::useProposal()
{
    if (proposal.isEmpty() || busy)
        return;
    const auto tagged = proposal;
    setProposal ({});
    noVocalsButton.setToggleState (false, juce::dontSendNotification);
    updateButtons();
    lyricsBox.selectAll(); // replaced as one edit, so Ctrl+Z in the box puts the user's text back
    lyricsBox.insertTextAtCaret (tagged);
    read();
}

void DescribeSongPanel::keepMine() { setProposal ({}); }

void DescribeSongPanel::setReferences (const juce::var& songs, const juce::String& text)
{
    references = songs;
    generated = text;
    intro.setText (references.isObject()
                       ? "Your reference songs, in words. Change what you want (\"make it 140 BPM\", \"in D minor\", "
                         "\"add a guitar\"): what you change wins, and their measured style stays for the rest."
                       : "Describe the song you want in a few words: its genre, mood, tempo, key, the instruments you "
                         "hear. Spinus reads it on this computer (no AI connection needed), then Parts and Write a "
                         "song work from it with the sounds you have.",
                   juce::dontSendNotification);
    updateButtons();
    resized();
}

void DescribeSongPanel::setNoVocals (bool on)
{
    noVocalsButton.setToggleState (on, juce::dontSendNotification);
    updateButtons();
}

void DescribeSongPanel::updateButtons()
{
    againButton.setVisible (references.isObject() && generated.isNotEmpty());
    againButton.setEnabled (! busy);
    readButton.setEnabled (! busy && textBox.getText().trim().isNotEmpty());
    aiButton.setEnabled (! busy);
    textBox.setReadOnly (busy);
    noVocalsButton.setEnabled (! busy);
    useTagsButton.setEnabled (! busy);
    keepMineButton.setEnabled (! busy);
    lyricsBox.setReadOnly (busy || noVocalsButton.getToggleState());
    lyricsBox.setAlpha (noVocalsButton.getToggleState() ? 0.5f : 1.0f);
}

void DescribeSongPanel::paint (juce::Graphics&)
{
    const auto& theme = themeOf (*this);
    intro.setColour (juce::Label::textColourId, theme.get ("text"));
    lyricsLabel.setColour (juce::Label::textColourId, theme.get ("textDim"));
    proposalLabel.setColour (juce::Label::textColourId, theme.get ("text"));
    understood.setColour (juce::Label::textColourId, theme.get ("accent"));
    status.setColour (juce::Label::textColourId, theme.get ("textDim"));
}

void DescribeSongPanel::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (56));
    area.removeFromTop (4);
    const auto boxHeight = juce::jlimit (60, 140, area.getHeight() / 5);
    textBox.setBounds (area.removeFromTop (boxHeight));
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (22);
        noVocalsButton.setBounds (r.removeFromRight (190));
        lyricsLabel.setBounds (r);
    }
    lyricsBox.setBounds (area.removeFromTop (boxHeight));
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        readButton.setBounds (r.removeFromLeft (80).reduced (0, 1));
        r.removeFromLeft (10);
        if (againButton.isVisible())
        {
            againButton.setBounds (r.removeFromLeft (140).reduced (0, 1));
            r.removeFromLeft (10);
        }
        aiButton.setBounds (r);
    }
    area.removeFromTop (4);
    understood.setBounds (area.removeFromTop (40));
    status.setBounds (area.removeFromTop (40));
    area.removeFromTop (4);
    if (proposalBox.isVisible())
    {
        {
            auto r = area.removeFromTop (28);
            keepMineButton.setBounds (r.removeFromRight (100).reduced (0, 1));
            r.removeFromRight (10);
            useTagsButton.setBounds (r.removeFromRight (130).reduced (0, 1));
            proposalLabel.setBounds (r);
        }
        proposalBox.setBounds (area.removeFromTop (juce::jmax (60, area.getHeight() / 2)));
        area.removeFromTop (4);
    }
    details.setBounds (area);
}

} // namespace spinus::ui
