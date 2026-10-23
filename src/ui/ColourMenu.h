#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The colours Spinus offers for channels, patterns, playlist tracks and mixer inserts: a "Colour" submenu
    of named swatches, each drawn in its colour, the current one ticked. */
struct ColourMenu
{
    struct Choice
    {
        const char* name;
        juce::uint32 argb;
    };

    static const std::vector<Choice>& getChoices()
    {
        static const std::vector<Choice> choices {
            { "Red", 0xffd65c5c },    { "Orange", 0xffe0894a }, { "Yellow", 0xffd8c04a }, { "Lime", 0xff9ccc4e },
            { "Green", 0xff6aa84f },  { "Teal", 0xff4fb3a0 },   { "Cyan", 0xff4fb8d6 },   { "Blue", 0xff5c8dd6 },
            { "Indigo", 0xff6f6fd6 }, { "Purple", 0xff9a7ad6 }, { "Pink", 0xffd67ab8 },   { "Brown", 0xffa0785a },
            { "Grey", 0xff8a8f99 },   { "Slate", 0xff3d4450 },
        };
        return choices;
    }

    /** A surface in an item's colour (a channel's name, a track's header, a strip): `amount` of the colour
        over the panel behind it. */
    static juce::Colour tint (juce::Colour panel, juce::Colour colour, float amount = 0.6f)
    {
        return panel.interpolatedWith (colour.withAlpha (1.0f), amount);
    }

    /** Text that reads on a coloured surface: near-black on light colours, near-white on dark ones. */
    static juce::Colour textOn (juce::Colour surface)
    {
        return surface.getPerceivedBrightness() > 0.62f ? juce::Colour (0xff15171b) : juce::Colour (0xfff3f4f6);
    }

    /** "#ffd65c5c", as commands take colours. */
    static juce::String toText (juce::uint32 argb)
    {
        return "#" + juce::String::toHexString ((juce::int64)argb).paddedLeft ('0', 8);
    }

    /** Adds the "Colour" submenu; `chosen` gets the picked colour. */
    static void addTo (juce::PopupMenu& menu, juce::uint32 current, const std::function<void (juce::uint32)>& chosen)
    {
        juce::PopupMenu colours;
        for (const auto& c : getChoices())
        {
            juce::PopupMenu::Item item (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xa0  ")) + c.name);
            item.setColour (juce::Colour (c.argb)).setTicked (c.argb == current);
            item.setAction ([chosen, argb = c.argb] { chosen (argb); });
            colours.addItem (std::move (item));
        }
        menu.addSubMenu ("Colour", colours);
    }
};

} // namespace spinus::ui
