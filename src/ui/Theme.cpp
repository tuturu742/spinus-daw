#include "Theme.h"

namespace spinus::ui
{

namespace
{
// Role, dark, light.
const std::tuple<const char*, juce::uint32, juce::uint32> builtIn[] = {
    { "background", 0xff1e2227, 0xffeceef1 },  // main window behind the panels
    { "panel", 0xff2a2f36, 0xfff8f9fa },       // panel content
    { "panelHeader", 0xff343a42, 0xffdde1e6 }, // panel title bars, top bar
    { "border", 0xff14171b, 0xffb8bec6 },      // separators and outlines
    { "text", 0xffe6e9ec, 0xff1d2125 },        // normal text
    { "textDim", 0xff9aa3ad, 0xff5d6670 },     // secondary text
    { "accent", 0xffe8c547, 0xffc79a00 },      // the Spinus yellow: selection, active state
    { "accentAlt", 0xff5c8dd6, 0xff2f6bc4 },   // second highlight (e.g. patterns)
    { "danger", 0xffe05252, 0xffc62828 },      // errors, record
    { "frozen", 0xff7cc6e6, 0xff1f7fa8 },      // frozen channels and inserts (Freeze)
    { "hintBar", 0xff16191d, 0xffd3d8de },     // the bottom hint bar
};
} // namespace

const juce::StringArray& Theme::roles()
{
    static const juce::StringArray list = []
    {
        juce::StringArray r;
        for (auto& [role, darkColour, lightColour] : builtIn)
            r.add (role);
        return r;
    }();
    return list;
}

Theme Theme::dark()
{
    Theme t;
    t.name = "Dark";
    for (auto& [role, darkColour, lightColour] : builtIn)
        t.colours[role] = juce::Colour (darkColour);
    return t;
}

Theme Theme::light()
{
    Theme t;
    t.name = "Light";
    for (auto& [role, darkColour, lightColour] : builtIn)
        t.colours[role] = juce::Colour (lightColour);
    return t;
}

Theme Theme::fromJson (const juce::var& json, juce::StringArray& errors)
{
    auto t = dark();
    t.name = json.getProperty ("name", "Custom").toString();

    auto* colours = json.getProperty ("colours", {}).getDynamicObject();
    if (colours == nullptr)
    {
        errors.add ("a theme needs a \"colours\" object");
        return t;
    }

    for (auto& property : colours->getProperties())
    {
        const auto role = property.name.toString();
        const auto text = property.value.toString();

        if (! roles().contains (role))
            errors.add ("unknown colour role '" + role + "'");
        else if (! text.startsWith ("#") || text.length() != 9 ||
                 ! text.substring (1).containsOnly ("0123456789abcdefABCDEF"))
            errors.add ("'" + role + "' must be a colour like #ff1e2227, not '" + text + "'");
        else
            t.colours[role] = juce::Colour ((juce::uint32)text.substring (1).getHexValue64());
    }

    return t;
}

juce::var Theme::toJson() const
{
    auto* c = new juce::DynamicObject();
    for (auto& role : roles())
        c->setProperty (role, "#" + get (role).toDisplayString (true).toLowerCase());

    auto* o = new juce::DynamicObject();
    o->setProperty ("name", name);
    o->setProperty ("colours", juce::var (c));
    return juce::var (o);
}

juce::Colour Theme::get (const juce::String& role) const
{
    auto it = colours.find (role);
    jassert (it != colours.end()); // every role has a value (dark() fills them all)
    return it != colours.end() ? it->second : juce::Colours::magenta;
}

void Theme::applyTo (juce::LookAndFeel_V4& lf) const
{
    const auto bg = get ("background"), panel = get ("panel"), header = get ("panelHeader"), text = get ("text"),
               dim = get ("textDim"), accent = get ("accent"), border = get ("border");

    lf.setColourScheme ({ panel, bg, header, border, text, accent, text, dim, accent });

    lf.setColour (juce::ResizableWindow::backgroundColourId, bg);
    lf.setColour (juce::DocumentWindow::textColourId, text);
    lf.setColour (juce::Label::textColourId, text);
    lf.setColour (juce::TextButton::buttonColourId, header);
    lf.setColour (juce::TextButton::buttonOnColourId, accent);
    lf.setColour (juce::TextButton::textColourOffId, text);
    lf.setColour (juce::TextButton::textColourOnId, bg);
    lf.setColour (juce::TooltipWindow::backgroundColourId, header);
    lf.setColour (juce::TooltipWindow::textColourId, text);
}

//==============================================================================
SpinusLookAndFeel::SpinusLookAndFeel (Theme t) { setTheme (std::move (t)); }

void SpinusLookAndFeel::setTheme (Theme t)
{
    theme = std::move (t);
    theme.applyTo (*this);
}

juce::AlertWindow* SpinusLookAndFeel::createAlertWindow (const juce::String& title, const juce::String& message,
                                                         const juce::String& button1, const juce::String& button2,
                                                         const juce::String& button3, juce::MessageBoxIconType icon,
                                                         int numButtons, juce::Component* associatedComponent)
{
    auto* window = LookAndFeel_V4::createAlertWindow (title, message, button1, button2, button3, icon, numButtons,
                                                      associatedComponent);
    window->setLookAndFeel (this);
    return window;
}

void SpinusLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>&,
                                      juce::TextLayout& textLayout)
{
    constexpr float cornerSize = 4.0f;
    constexpr int iconColumn = 80; // as LookAndFeel_V4, which AlertWindow's layout expects
    const auto bounds = alert.getLocalBounds().reduced (1);
    g.setColour (alert.findColour (juce::AlertWindow::backgroundColourId));
    g.fillRoundedRectangle (bounds.toFloat(), cornerSize);
    g.setColour (alert.findColour (juce::AlertWindow::outlineColourId));
    g.drawRoundedRectangle (alert.getLocalBounds().toFloat().reduced (0.5f), cornerSize, 1.0f);

    const auto type = alert.getAlertType();
    auto iconSpaceUsed = 0;
    if (type != juce::MessageBoxIconType::NoIcon)
    {
        const juce::Rectangle<float> icon ((float)iconColumn - 56.0f, 26.0f, 36.0f, 36.0f);
        const bool warning = type == juce::MessageBoxIconType::WarningIcon;
        g.setColour (theme.get (warning ? "danger" : "accentAlt"));
        if (warning)
        {
            juce::Path triangle;
            triangle.addTriangle (icon.getCentreX(), icon.getY(), icon.getRight(), icon.getBottom(), icon.getX(),
                                  icon.getBottom());
            g.fillPath (triangle.createPathWithRoundedCorners (4.0f));
        }
        else
            g.fillEllipse (icon);
        g.setColour (theme.get ("panel"));
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText (warning                                      ? "!"
                    : type == juce::MessageBoxIconType::InfoIcon ? "i"
                                                                 : "?",
                    icon.withTrimmedTop (warning ? 8.0f : 0.0f), juce::Justification::centred);
        iconSpaceUsed = iconColumn;
    }

    g.setColour (alert.findColour (juce::AlertWindow::textColourId));
    const juce::Rectangle<int> text (bounds.getX() + iconSpaceUsed, 30, bounds.getWidth(),
                                     bounds.getHeight() - getAlertWindowButtonHeight() - 20);
    textLayout.draw (g, text.toFloat());
}

void drawFrozenIcon (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    // A snowflake: three crossing lines with a small fork near each end.
    const auto centre = area.getCentre();
    const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    const auto thickness = juce::jmax (1.0f, radius * 0.16f);
    g.setColour (colour);
    for (int arm = 0; arm < 6; ++arm)
    {
        const auto angle = juce::MathConstants<float>::pi / 3.0f * (float)arm;
        const auto tip = centre.getPointOnCircumference (radius, angle);
        g.drawLine ({ centre, tip }, thickness);
        const auto fork = centre.getPointOnCircumference (radius * 0.6f, angle);
        g.drawLine ({ fork, fork.getPointOnCircumference (radius * 0.3f, angle - 0.8f) }, thickness);
        g.drawLine ({ fork, fork.getPointOnCircumference (radius * 0.3f, angle + 0.8f) }, thickness);
    }
}

} // namespace spinus::ui
