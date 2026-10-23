#include "HintBar.h"

#include "Theme.h"

namespace spinus::ui
{

HintBar::HintBar() { juce::Desktop::getInstance().addGlobalMouseListener (&globalMouse); }

HintBar::~HintBar() { juce::Desktop::getInstance().removeGlobalMouseListener (&globalMouse); }

juce::String HintBar::hintFor (const juce::Component* c)
{
    for (; c != nullptr; c = c->getParentComponent())
        if (auto hint = c->getHelpText(); hint.isNotEmpty())
            return hint;
    return {};
}

void HintBar::mouseMovedAnywhere (const juce::MouseEvent& e)
{
    if (juce::Time::getMillisecondCounter() < messageUntil)
        return;

    setText (hintFor (juce::Desktop::getInstance().findComponentAt (e.getScreenPosition())));
}

void HintBar::showMessage (const juce::String& message)
{
    messageUntil = juce::Time::getMillisecondCounter() + 4000;
    setText (message);
}

void HintBar::setText (const juce::String& t)
{
    if (t != text)
    {
        text = t;
        repaint();
    }
}

void HintBar::paint (juce::Graphics& g)
{
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&getLookAndFeel());
    g.fillAll (lf != nullptr ? lf->getTheme().get ("hintBar") : juce::Colours::black);
    g.setColour (lf != nullptr ? lf->getTheme().get ("textDim") : juce::Colours::grey);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (text, getLocalBounds().reduced (8, 0), juce::Justification::centredLeft, true);
}

} // namespace spinus::ui
