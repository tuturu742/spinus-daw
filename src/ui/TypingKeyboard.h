#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

namespace spinus::ui
{

/** The computer keyboard as a piano (FL: "Typing keyboard to piano"), for playing and recording without a
    MIDI keyboard. Two rows, like FL:

        lower octave   Z S X D C V G B H N J M , L . ; /     (white keys on Z X C V B N M , . /)
        upper octave   Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P     (white keys on Q W E R T Y U I O P)

    The lower row starts at C5 (MIDI 60, named as in the Piano Roll) and the upper one an octave higher; - and = move
   both an octave down or up. While on, these keys play notes instead of their shortcuts (R: record, L: pattern/song),
    and shortcuts with Ctrl or Alt work as usual.

    The owner of the keys (the main view) passes keyPressed and keyStateChanged on; a timer releases notes
    whose key went up while the window had no focus. Message thread only. */
class TypingKeyboard final : private juce::Timer
{
public:
    /** Called to play (down) or release a note. */
    explicit TypingKeyboard (std::function<void (int note, bool down)> play);
    ~TypingKeyboard() override;

    bool isOn() const noexcept { return on; }
    void setOn (bool); // turning off releases every note

    /** The note of the lower row's first key (Z), 60 at first; - and = change it by an octave. */
    int getBaseNote() const noexcept { return baseNote; }
    void setBaseNote (int);

    /** The note a key plays (`character` lower case), or -1 when it plays none. */
    int noteFor (juce::juce_wchar character) const;

    /** True when the key was used (a note, or an octave change). */
    bool keyPressed (const juce::KeyPress&);
    /** Releases the notes whose keys are up. True when some were. */
    bool keyStateChanged();
    void releaseAll();

    /** Notes held now (for tests). */
    std::vector<int> getHeldNotes() const;

    /** What the keys do, for the hint bar: "Typing keyboard: Z-M and Q-U play C5-B6 (- and = change
        octave)". */
    juce::String describe() const;

    /** Tests: whether a key is down, instead of asking the system. */
    std::function<bool (int keyCode)> isKeyDown;

private:
    void timerCallback() override;

    std::function<void (int, bool)> play;
    bool on = false;
    int baseNote = 60;
    std::map<int, int> held; // key code -> the note it plays
};

} // namespace spinus::ui
