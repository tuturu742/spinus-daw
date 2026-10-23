#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

namespace spinus::ui
{

/** The colours of the Spinus UI. Every colour comes from a theme, so themes are
    user-editable JSON files:

        { "name": "Dark", "colours": { "background": "#ff1e2227", "accent": "#ffe8c547", ... } }

    Colours a theme leaves out fall back to the built-in dark theme.
*/
class Theme
{
public:
    /** The colour roles every theme defines. */
    static const juce::StringArray& roles();

    static Theme dark();
    static Theme light();

    /** Parses a theme file. Unknown roles and malformed colours are reported in `errors` and ignored. */
    static Theme fromJson (const juce::var&, juce::StringArray& errors);
    juce::var toJson() const;

    juce::String getName() const { return name; }
    juce::Colour get (const juce::String& role) const;

    /** Applies the theme to a LookAndFeel's standard JUCE colour IDs. */
    void applyTo (juce::LookAndFeel_V4&) const;

private:
    juce::String name;
    std::map<juce::String, juce::Colour> colours;
};

/** The Spinus look: LookAndFeel_V4 with theme colours. */
class SpinusLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit SpinusLookAndFeel (Theme);

    const Theme& getTheme() const noexcept { return theme; }
    void setTheme (Theme);

    /** Message boxes: a small icon in the left margin, beside the title (LookAndFeel_V4 draws a large,
        faded one that reaches under the text). */
    void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea,
                       juce::TextLayout&) override;
    /** Message boxes draw with this look (they are windows of their own, so they would use the default). */
    juce::AlertWindow* createAlertWindow (const juce::String& title, const juce::String& message,
                                          const juce::String& button1, const juce::String& button2,
                                          const juce::String& button3, juce::MessageBoxIconType, int numButtons,
                                          juce::Component* associatedComponent) override;

private:
    Theme theme;
};

/** The Freeze symbol (a snowflake), drawn in the area. */
void drawFrozenIcon (juce::Graphics&, juce::Rectangle<float>, juce::Colour);

} // namespace spinus::ui
