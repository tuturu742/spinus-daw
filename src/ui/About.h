#pragma once

#include "Markdown.h"

namespace spinus::ui
{

/** Help › About Spinus: the logo, name and version, what Spinus is, its license (AGPL-3.0-or-later), the demo
    song's (CC BY 4.0), the project's GitHub page, and the third-party components with their licenses
    (THIRD_PARTY.md, built into Spinus so it is there in every package). "Copy version info" puts the version,
    the system and the package on the clipboard, for a bug report. */
class AboutSpinus final : public juce::Component
{
public:
    AboutSpinus();
    ~AboutSpinus() override;

    static void show (juce::Component* centreAround);
    static AboutSpinus* getOpen();
    static void closeWindow();

    static constexpr const char* projectUrl = "https://github.com/tuturu742/spinus-daw";

    /** "Spinus DAW 0.1.0-alpha.1", the package kind, the system and JUCE's version, a line each. */
    static juce::String getVersionInfo();
    /** THIRD_PARTY.md as built into Spinus. */
    static juce::String getThirdPartyText();
    /** The words above the credits (tests): name, version, description, licenses. */
    juce::String getText() const;
    void copyVersionInfo();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::unique_ptr<juce::Drawable> logo;
    juce::Label name, version, description, licenses;
    juce::HyperlinkButton github;
    juce::TextButton copyButton { "Copy version info" };
    juce::Label creditsTitle;
    juce::Viewport viewport;
    markdown::View credits;
    juce::Rectangle<int> logoArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutSpinus)
};

} // namespace spinus::ui
