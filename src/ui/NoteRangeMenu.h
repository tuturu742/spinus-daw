#pragma once

#include "session/NoteRanges.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace spinus::ui
{

/** "Notes it plays": the lowest and the highest note an instrument may play (NoteRanges), an octave a submenu, as
    the Piano roll names them (with the MIDI number, since instruments' manuals name octaves differently), and
    "Any note again". `set` gets the new range (nullopt: any). */
inline juce::PopupMenu makeNoteRangeMenu (std::optional<NoteRanges::Range> now,
                                          const std::function<void (std::optional<NoteRanges::Range>)>& set)
{
    auto ends = [now, set] (bool lowest)
    {
        juce::PopupMenu octaves;
        for (int octave = 0; octave <= 10; ++octave)
        {
            juce::PopupMenu notes;
            for (int note = octave * 12; note < juce::jmin (128, octave * 12 + 12); ++note)
            {
                const auto current = now.has_value() && (lowest ? now->low : now->high) == note;
                auto range = now.value_or (NoteRanges::Range {});
                (lowest ? range.low : range.high) = note;
                notes.addItem (NoteRanges::noteName (note) + "  (MIDI " + juce::String (note) + ")",
                               range.low <= range.high, current, [set, range] { set (range); });
            }
            octaves.addSubMenu (NoteRanges::noteName (octave * 12) + " ...", notes);
        }
        return octaves;
    };
    juce::PopupMenu menu;
    menu.addSectionHeader ("For an instrument whose other keys switch what it plays");
    menu.addSubMenu ("Lowest note" +
                         (now.has_value() ? " (now " + NoteRanges::noteName (now->low) + ")" : juce::String()),
                     ends (true));
    menu.addSubMenu ("Highest note" +
                         (now.has_value() ? " (now " + NoteRanges::noteName (now->high) + ")" : juce::String()),
                     ends (false));
    if (now.has_value())
        menu.addItem ("Any note again", [set] { set (std::nullopt); });
    return menu;
}

} // namespace spinus::ui
