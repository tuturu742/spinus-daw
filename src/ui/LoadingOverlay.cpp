#include "LoadingOverlay.h"

#include "PaintNow.h"
#include "Theme.h"

namespace spinus::ui
{

LoadingOverlay::LoadingOverlay()
{
    // Opaque, so painting it now paints nothing under it: the panels may be half way through the new project. What
    // shows around the card is a picture of them from before.
    setOpaque (true);
    setVisible (false);
}

void LoadingOverlay::start (const juce::String& name)
{
    title = "Opening " + name;
    detail = "Reading the project";
    progress = -1.0; // no count yet: the bar moves on its own
    if (auto* parent = getParentComponent())
    {
        underneath = parent->createComponentSnapshot (parent->getLocalBounds(), true,
                                                      juce::Component::getApproximateScaleFactorForComponent (parent));
        setBounds (parent->getLocalBounds());
    }
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

void LoadingOverlay::finish()
{
    setVisible (false);
    underneath = {};
}

// The message loop is busy opening the project, so a repaint would wait for the end: paint now.
void LoadingOverlay::paintNow() { ui::paintNow (*this); }

void LoadingOverlay::resized()
{
    // Bottom right, clear of the hint bar, so the window stays in view.
    const auto width = juce::jmin (340, getWidth() - 32);
    card = { getWidth() - 16 - width, getHeight() - 40 - 84, width, 84 };
    bar = card.reduced (14).removeFromBottom (10);
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

    if (underneath.isValid())
        g.drawImage (underneath, getLocalBounds().toFloat());
    else
        g.fillAll (colour ("background", juce::Colours::black));
    juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 12, {}).drawForRectangle (g, card);
    g.setColour (colour ("panel", juce::Colours::darkgrey));
    g.fillRoundedRectangle (card.toFloat(), 6.0f);
    g.setColour (colour ("border", juce::Colours::grey));
    g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 6.0f, 1.0f);

    auto area = card.reduced (14);
    g.setColour (colour ("text", juce::Colours::white));
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText (title, area.removeFromTop (20), juce::Justification::centredLeft, true);
    g.setColour (colour ("textDim", juce::Colours::grey));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (detail, area.removeFromTop (20), juce::Justification::centredLeft, true);

    // Drawn here rather than by a juce::ProgressBar, which only takes a new value on its timer: not while the loop
    // is busy.
    g.setColour (colour ("background", juce::Colours::black));
    g.fillRoundedRectangle (bar.toFloat(), 3.0f);
    g.setColour (colour ("accent", juce::Colours::orange));
    g.fillRoundedRectangle (getFilledPart().toFloat(), 3.0f);
}

} // namespace spinus::ui
