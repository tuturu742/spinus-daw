#include "About.h"

#include "Logo.h"
#include "Theme.h"
#include "WindowStacking.h"
#include "session/ProgramFolder.h"
#include "session/SystemInfo.h"

#include <SpinusArt.h>
#include <SpinusVersion.h>

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

constexpr int topHeight = 236;
} // namespace

juce::String AboutSpinus::getVersionInfo()
{
    juce::StringArray lines;
    lines.add (juce::String (productName) + " " + versionString);
    lines.add ("Package: " + session::getPackageKind());
    lines.add ("System: " + session::getSystemName());
    lines.add ("Built with " + juce::SystemStats::getJUCEVersion() + " and Tracktion Engine");
    return lines.joinIntoString ("\n");
}

juce::String AboutSpinus::getThirdPartyText()
{
    return juce::String::fromUTF8 (SpinusArt::THIRD_PARTY_md, SpinusArt::THIRD_PARTY_mdSize);
}

AboutSpinus::AboutSpinus() : logo (createLogo())
{
    name.setText (productName, juce::dontSendNotification);
    name.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    version.setText ("Version " + juce::String (versionString), juce::dontSendNotification);
    version.setFont (juce::FontOptions (14.5f));
    description.setText ("A free, open-source digital audio workstation for Linux and Windows, with an FL Studio-style "
                         "workflow and optional AI help that only ever proposes changes.",
                         juce::dontSendNotification);
    description.setFont (juce::FontOptions (13.5f));
    description.setJustificationType (juce::Justification::topLeft);
    licenses.setText (
        juce::String::fromUTF8 ("License: GNU Affero General Public License, version 3 or later (AGPL-3.0-or-later).\n"
                                "The demo song \xe2\x80\x9cSiskin\xe2\x80\x9d: CC BY 4.0, by Spinus DAW contributors.\n"
                                "Made with JUCE and Tracktion Engine."),
        juce::dontSendNotification);
    licenses.setFont (juce::FontOptions (12.5f));
    licenses.setJustificationType (juce::Justification::topLeft);
    for (auto* label : { &name, &version, &description, &licenses })
    {
        label->setBorderSize ({});
        label->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (*label);
    }

    github.setButtonText ("github.com/tuturu742/spinus-daw");
    github.setURL (juce::URL (projectUrl));
    github.setFont (juce::FontOptions (13.5f), false, juce::Justification::centredLeft);
    github.setTooltip (projectUrl);
    addAndMakeVisible (github);

    copyButton.setHelpText ("Copy Spinus's version and your system, to paste into a bug report");
    copyButton.onClick = [this] { copyVersionInfo(); };
    addAndMakeVisible (copyButton);

    creditsTitle.setText ("Third-party components", juce::dontSendNotification);
    creditsTitle.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    creditsTitle.setBorderSize ({});
    addAndMakeVisible (creditsTitle);

    // THIRD_PARTY.md under the title above, with what it is for.
    auto text = getThirdPartyText();
    if (text.startsWith ("# "))
        text = text.fromFirstOccurrenceOf ("\n", false, false);
    text =
        text.replace ("When you add, upgrade or remove a dependency, update this file in the same pull request.", {});
    credits.setContent (markdown::parse ("Spinus is built on the work of these projects. The same list is "
                                         "`THIRD_PARTY.md`, in the package and on [GitHub](" +
                                         juce::String (projectUrl) + "/blob/main/THIRD_PARTY.md).\n\n" + text),
                        {});
    credits.onLink = [] (const juce::String& target)
    {
        if (target.startsWith ("http"))
            juce::URL (target).launchInDefaultBrowser();
        else if (! juce::File::isAbsolutePath (target))
            juce::URL (juce::String (projectUrl) + "/blob/main/" + target).launchInDefaultBrowser();
    };
    viewport.setViewedComponent (&credits, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    viewport.getVerticalScrollBar().setSingleStepSize (40.0);
    addAndMakeVisible (viewport);

    setSize (780, 720);
}

AboutSpinus::~AboutSpinus() = default;

juce::String AboutSpinus::getText() const
{
    return juce::StringArray { name.getText(), version.getText(), description.getText(), licenses.getText(),
                               projectUrl }
        .joinIntoString ("\n");
}

void AboutSpinus::copyVersionInfo()
{
    juce::SystemClipboard::copyTextToClipboard (getVersionInfo());
    copyButton.setButtonText ("Copied");
    juce::Timer::callAfterDelay (1500,
                                 [safe = juce::Component::SafePointer<AboutSpinus> (this)]
                                 {
                                     if (safe != nullptr)
                                         safe->copyButton.setButtonText ("Copy version info");
                                 });
}

void AboutSpinus::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    g.setColour (theme.get ("panelHeader"));
    g.fillRect (getLocalBounds().removeFromTop (topHeight));
    g.setColour (theme.get ("border"));
    g.fillRect (0, topHeight, getWidth(), 1);
    if (logo != nullptr)
        logo->drawWithin (g, logoArea.toFloat(), juce::RectanglePlacement::centred, 1.0f);

    name.setColour (juce::Label::textColourId, theme.get ("text"));
    version.setColour (juce::Label::textColourId, theme.get ("accent"));
    description.setColour (juce::Label::textColourId, theme.get ("text"));
    licenses.setColour (juce::Label::textColourId, theme.get ("textDim"));
    creditsTitle.setColour (juce::Label::textColourId, theme.get ("text"));
    github.setColour (juce::HyperlinkButton::textColourId, theme.get ("accentAlt").brighter (0.25f));
}

void AboutSpinus::resized()
{
    auto top = getLocalBounds().removeFromTop (topHeight).reduced (24, 20);
    logoArea = top.removeFromLeft (132).removeFromTop (132);
    top.removeFromLeft (20);
    name.setBounds (top.removeFromTop (36));
    version.setBounds (top.removeFromTop (22));
    top.removeFromTop (8);
    description.setBounds (top.removeFromTop (40));
    licenses.setBounds (top.removeFromTop (56));
    auto row = top.removeFromTop (28);
    github.setBounds (row.removeFromLeft (250));
    copyButton.setBounds (row.removeFromRight (150).reduced (0, 1));

    auto rest = getLocalBounds().withTrimmedTop (topHeight + 1);
    creditsTitle.setBounds (rest.removeFromTop (36).reduced (24, 0).withTrimmedTop (10));
    viewport.setBounds (rest);
    credits.setWidth (viewport.getMaximumVisibleWidth());
}

//==============================================================================
namespace
{
juce::Component::SafePointer<juce::DocumentWindow>& openWindow()
{
    static juce::Component::SafePointer<juce::DocumentWindow> window;
    return window;
}

class Window final : public juce::DocumentWindow
{
public:
    Window (AboutSpinus* content, juce::Component* owner)
        : DocumentWindow ("About Spinus", juce::Colour (0xff1e2227), DocumentWindow::closeButton)
    {
        if (owner != nullptr)
            if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&owner->getLookAndFeel()))
            {
                setBackgroundColour (lf->getTheme().get ("panel"));
                content->setLookAndFeel (lf);
            }
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (620, 480, 2000, 2000);
        if (owner != nullptr)
            centreAroundComponent (owner->getTopLevelComponent(), getWidth(), getHeight());
        else
            centreWithSize (getWidth(), getHeight());
        setVisible (true);
        if (owner != nullptr)
            keepAboveOwner (*this, *owner->getTopLevelComponent());
    }

    ~Window() override
    {
        if (auto* content = getContentComponent())
            content->setLookAndFeel (nullptr);
        clearContentComponent();
    }

    void closeButtonPressed() override { delete this; }
};
} // namespace

void AboutSpinus::show (juce::Component* centreAround)
{
    if (openWindow() == nullptr)
        openWindow() = new Window (new AboutSpinus(), centreAround);
    openWindow()->toFront (true);
}

AboutSpinus* AboutSpinus::getOpen()
{
    return openWindow() != nullptr ? dynamic_cast<AboutSpinus*> (openWindow()->getContentComponent()) : nullptr;
}

void AboutSpinus::closeWindow()
{
    if (auto* window = openWindow().getComponent())
        delete window;
}

} // namespace spinus::ui
