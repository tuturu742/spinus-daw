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
                   "hear. Spinus reads it on this computer (no AI connection needed), then Sounds for each part "
                   "and Write a song work from it with the sounds you have.",
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
    aiButton.setTooltip ("Ask your AI connection (Options > AI) to read the description; Spinus checks its reading "
                         "and writes every note itself. Off: Spinus's own rules, nothing is sent anywhere");
    readButton.setTooltip ("Read the description into a style to write a song in");
    readButton.onClick = [this] { read(); };
    for (auto* l : { &understood, &status })
    {
        l->setJustificationType (juce::Justification::topLeft);
        l->setMinimumHorizontalScale (1.0f);
    }
    details.setMultiLine (true, true);
    details.setReadOnly (true);
    details.setScrollbarsShown (true);
    details.setCaretVisible (false);
    for (auto* c : std::initializer_list<juce::Component*> { &intro, &textBox, &aiButton, &readButton, &understood,
                                                             &status, &details })
        addAndMakeVisible (c);
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
    busy = true;
    const bool useAi = aiButton.getToggleState();
    status.setText (useAi ? "The AI is reading the description..." : "Reading the description...",
                    juce::dontSendNotification);
    updateButtons();
    ai::runJob (
        *service, "ai.v1.song.describe", object ({ { "text", text }, { "ai", useAi } }), alive,
        [this] (const juce::String& started) { job = started; },
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
    status.setText ((described["note"].toString().isNotEmpty() ? described["note"].toString() + " "
                     : by.isNotEmpty()                         ? "Read by the AI (" + by + "). "
                                                               : juce::String()) +
                        "Next: Sounds for each part, or Write a song.",
                    juce::dontSendNotification);
}

void DescribeSongPanel::updateButtons()
{
    readButton.setEnabled (! busy && textBox.getText().trim().isNotEmpty());
    aiButton.setEnabled (! busy);
    textBox.setReadOnly (busy);
}

void DescribeSongPanel::paint (juce::Graphics&)
{
    const auto& theme = themeOf (*this);
    intro.setColour (juce::Label::textColourId, theme.get ("text"));
    understood.setColour (juce::Label::textColourId, theme.get ("accent"));
    status.setColour (juce::Label::textColourId, theme.get ("textDim"));
}

void DescribeSongPanel::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (56));
    area.removeFromTop (4);
    textBox.setBounds (area.removeFromTop (juce::jlimit (60, 140, area.getHeight() / 4)));
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        readButton.setBounds (r.removeFromLeft (80).reduced (0, 1));
        r.removeFromLeft (10);
        aiButton.setBounds (r);
    }
    area.removeFromTop (4);
    understood.setBounds (area.removeFromTop (40));
    status.setBounds (area.removeFromTop (40));
    area.removeFromTop (4);
    details.setBounds (area);
}

} // namespace spinus::ui
