#include <catch2/catch_test_macros.hpp>

#include "ui/TypingKeyboard.h"

#include <set>

using spinus::ui::TypingKeyboard;

TEST_CASE ("Typing keyboard: two rows like a piano, off by default, repeats and releases", "[ui]")
{
    std::vector<std::pair<int, bool>> played;
    TypingKeyboard keys ([&] (int note, bool down) { played.push_back ({ note, down }); });
    std::set<int> down;
    keys.isKeyDown = [&] (int code) { return down.contains (code); };
    auto press = [&] (juce::juce_wchar c, juce::ModifierKeys mods = {})
    {
        down.insert ((int)c);
        return keys.keyPressed (juce::KeyPress ((int)c, mods, c));
    };
    auto release = [&] (juce::juce_wchar c)
    {
        down.erase ((int)c);
        keys.keyStateChanged();
    };

    // Off: keys stay shortcuts.
    CHECK (! keys.isOn());
    CHECK (! press ('r'));
    CHECK (played.empty());

    keys.setOn (true);
    CHECK (keys.noteFor ('z') == 60);
    CHECK (keys.noteFor ('s') == 61);
    CHECK (keys.noteFor ('m') == 71);
    CHECK (keys.noteFor ('q') == 72);
    CHECK (keys.noteFor ('r') == 77); // F6: R plays a note while on
    CHECK (keys.noteFor ('u') == 83);
    CHECK (keys.noteFor ('a') == -1);
    CHECK (! press ('a')); // not a note key: left to the shortcuts

    CHECK (press ('z'));
    CHECK (press ('z')); // the key repeats while held: one note
    CHECK (press ('e'));
    CHECK (played == std::vector<std::pair<int, bool>> { { 60, true }, { 76, true } });
    CHECK (keys.getHeldNotes().size() == 2);
    release ('z');
    CHECK (played.back() == std::pair { 60, false });
    CHECK (keys.getHeldNotes() == std::vector<int> { 76 });

    // Shortcuts with Ctrl still work (Ctrl+Z: undo).
    CHECK (! press ('z', juce::ModifierKeys::ctrlModifier));

    // - and = change octave (releasing what was held).
    CHECK (press ('='));
    CHECK (played.back() == std::pair { 76, false });
    CHECK (keys.noteFor ('z') == 72);
    CHECK (keys.describe() == "Typing keyboard: Z-M and Q-U play C6-B7 (- and = change octave)");
    for (int i = 0; i < 10; ++i)
        press ('-');
    CHECK (keys.getBaseNote() == 12); // limited

    // Turning off releases held notes.
    keys.setBaseNote (60);
    press ('x');
    keys.setOn (false);
    CHECK (played.back() == std::pair { 62, false });
    CHECK (keys.getHeldNotes().empty());
}
