#include "LoadingOverlay.h"

#include "PaintNow.h"
#include "Theme.h"

namespace spinus::ui
{

LoadingOverlay::LoadingOverlay()
{
    // Opaque, so painting it now paints nothing under it: the panels may be half way through the new project.
    setOpaque (true);
    setVisible (false);
}

void LoadingOverlay::start (const juce::String& name)
{
    title = "Opening " + name;
    detail = "Reading the project";
    progress = -1.0; // no count yet: the bar moves on its own
    if (auto* parent = getParentComponent())
        setBounds (parent->getLocalBounds());
    setVisible (true);
    toFront (false);
    paintNow();
}

void LoadingOverlay::setProgress (int done, int total, const juce::String& what)
{
    if (! isVisible())
        return;
    detail = "Loading " + what + " (" + juce::String (done + 1) + " of " + juce::String (total) + ")";
    progress = total > 0 ? (double)done / total : -1.0;
    paintNow();
}

void LoadingOverlay::finish() { setVisible (false); }

// The message loop is busy opening the project, so a repaint would wait for the end: paint now.
void LoadingOverlay::paintNow() { ui::paintNow (*this); }

void LoadingOverlay::resized()
{
    card = getLocalBounds().withSizeKeepingCentre (juce::jmin (440, getWidth() - 32), 110);
    bar = card.reduced (20).removeFromBottom (14);
}

juce::Rectangle<int> LoadingOverlay::getFilledPart() const
{
    if (progress >= 0.0)
        return bar.withWidth (juce::roundToInt (bar.getWidth() * juce::jlimit (0.0, 1.0, progress)));
    // No count yet: a block that moves along a little at each paint (not a timer, the loop is busy).
    const auto width = bar.getWidth() / 4;
    const auto travel = juce::jmax (1, bar.getWidth() - width);
    const auto x = (int)((juce::Time::getMillisecondCounter() / 4) % (juce::uint32)(2 * travel));
    return bar.withWidth (width).translated (x < travel ? x : 2 * travel - x, 0);
}

void LoadingOverlay::paint (juce::Graphics& g)
{
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&getLookAndFeel());
    auto colour = [lf] (const char* name, juce::Colour fallback)
    { return lf != nullptr ? lf->getTheme().get (name) : fallback; };

    g.fillAll (colour ("background", juce::Colours::black));
    g.setColour (colour ("panel", juce::Colours::darkgrey));
    g.fillRoundedRectangle (card.toFloat(), 6.0f);
    g.setColour (colour ("border", juce::Colours::grey));
    g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 6.0f, 1.0f);

    auto area = card.reduced (20);
    g.setColour (colour ("text", juce::Colours::white));
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText (title, area.removeFromTop (24), juce::Justification::centredLeft, true);
    g.setColour (colour ("textDim", juce::Colours::grey));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (detail, area.removeFromTop (22), juce::Justification::centredLeft, true);

    // Drawn here rather than by a juce::ProgressBar, which only takes a new value on its timer: not while the loop
    // is busy.
    g.setColour (colour ("background", juce::Colours::black));
    g.fillRoundedRectangle (bar.toFloat(), 3.0f);
    g.setColour (colour ("accent", juce::Colours::orange));
    g.fillRoundedRectangle (getFilledPart().toFloat(), 3.0f);
}

} // namespace spinus::ui
