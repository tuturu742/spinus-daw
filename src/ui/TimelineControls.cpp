#include "TimelineControls.h"

namespace spinus::ui
{

namespace timeline
{
double zoomAround (double& pixelsPerBeat, double scrollTicks, double x, double factor, juce::Range<double> limits,
                   double ticksPerBeat, double earliest)
{
    const auto tick = x / pixelsPerBeat * ticksPerBeat + scrollTicks;
    pixelsPerBeat = limits.clipValue (pixelsPerBeat * factor);
    return juce::jmax (earliest, tick - x / pixelsPerBeat * ticksPerBeat);
}

std::optional<double> followPlayhead (double playhead, double scroll, double visible)
{
    if (playhead < 0.0 || visible <= 0.0)
        return std::nullopt;
    // The page turns a little before the edge, so the playhead is never hidden under it.
    if (playhead >= scroll && playhead <= scroll + visible * 0.97)
        return std::nullopt;
    return juce::jmax (0.0, playhead - visible * 0.03);
}

ZoomKey zoomKeyOf (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    // Page Up and Page Down on their own, as in FL Studio.
    if (! mods.isAnyModifierKeyDown() && key.getKeyCode() == juce::KeyPress::pageUpKey)
        return ZoomKey::in;
    if (! mods.isAnyModifierKeyDown() && key.getKeyCode() == juce::KeyPress::pageDownKey)
        return ZoomKey::out;
    if (! mods.isCommandDown() || mods.isAltDown())
        return ZoomKey::none;
    const auto code = key.getKeyCode();
    const auto character = key.getTextCharacter();
    if (code == '=' || code == '+' || character == '=' || character == '+' || code == juce::KeyPress::numberPadAdd)
        return ZoomKey::in;
    if (code == '-' || character == '-' || character == '_' || code == juce::KeyPress::numberPadSubtract)
        return ZoomKey::out;
    if (code == '0' || code == juce::KeyPress::numberPad0)
        return ZoomKey::fit;
    return ZoomKey::none;
}
} // namespace timeline

TimelineControls::TimelineControls (juce::PropertiesFile* s, juce::String setting)
    : settings (s), followSetting (std::move (setting))
{
    zoomOutButton.setHelpText ("Zoom out (Ctrl+-, or Ctrl+wheel)");
    zoomInButton.setHelpText ("Zoom in (Ctrl+=, or Ctrl+wheel)");
    fitButton.setHelpText ("Fit: show it all (Ctrl+0)");
    zoomOutButton.onClick = [this]
    {
        if (onZoom)
            onZoom (1.0 / timeline::zoomStep);
    };
    zoomInButton.onClick = [this]
    {
        if (onZoom)
            onZoom (timeline::zoomStep);
    };
    fitButton.onClick = [this]
    {
        if (onFit)
            onFit();
    };
    for (auto* b : { &zoomOutButton, &zoomInButton, &fitButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    zoomOutButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    zoomInButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    fitButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    if (followSetting.isNotEmpty())
    {
        followButton.setHelpText ("Follow: the view turns the page to keep the play position in sight while playing");
        followButton.setClickingTogglesState (true);
        followButton.setWantsKeyboardFocus (false);
        followButton.setToggleState (settings == nullptr || settings->getBoolValue (followSetting, true),
                                     juce::dontSendNotification);
        followButton.onClick = [this]
        {
            if (settings != nullptr)
            {
                settings->setValue (followSetting, followButton.getToggleState());
                settings->saveIfNeeded();
            }
        };
        addAndMakeVisible (followButton);
    }
}

void TimelineControls::setFollowing (bool follow)
{
    if (followButton.isVisible() && followButton.getToggleState() != follow)
        followButton.setToggleState (follow, juce::sendNotificationSync);
}

int TimelineControls::getPreferredWidth() const { return 26 + 26 + 36 + (followButton.isVisible() ? 6 + 58 : 0); }

void TimelineControls::resized()
{
    auto area = getLocalBounds();
    if (followButton.isVisible())
    {
        followButton.setBounds (area.removeFromRight (58));
        area.removeFromRight (6);
    }
    zoomOutButton.setBounds (area.removeFromLeft (26));
    zoomInButton.setBounds (area.removeFromLeft (26));
    fitButton.setBounds (area);
}

} // namespace spinus::ui
