#include "Logo.h"

#include <SpinusArt.h>

namespace spinus::ui
{

std::unique_ptr<juce::Drawable> createLogo()
{
    return juce::Drawable::createFromImageData (SpinusArt::spinus_svg, SpinusArt::spinus_svgSize);
}

juce::Image logoImage (int size)
{
    juce::Image image (juce::Image::ARGB, size, size, true);
    if (auto logo = createLogo())
    {
        juce::Graphics g (image);
        logo->drawWithin (g, juce::Rectangle<int> (size, size).toFloat(), juce::RectanglePlacement::centred, 1.0f);
    }
    return image;
}

WindowIcons::WindowIcons() : icon (logoImage (256))
{
    juce::Desktop::getInstance().addFocusChangeListener (this);
    startTimer (500); // windows that open without taking the focus
    apply();
}

WindowIcons::~WindowIcons() { juce::Desktop::getInstance().removeFocusChangeListener (this); }

void WindowIcons::apply()
{
    std::set<juce::ComponentPeer*> current;
    for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
    {
        auto* peer = juce::ComponentPeer::getPeer (i);
        current.insert (peer);
        if (! done.contains (peer))
            peer->setIcon (icon);
    }
    done = std::move (current); // closed windows drop out, so a new window at the same address gets it too
}

} // namespace spinus::ui
