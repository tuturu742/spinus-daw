#include "TypingKeyboard.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace spinus::ui
{

namespace
{
// Semitones above the lower row's C for each key; the upper row adds an octave.
constexpr std::pair<juce::juce_wchar, int> lowerRow[] = {
    { 'z', 0 }, { 's', 1 },  { 'x', 2 },  { 'd', 3 },  { 'c', 4 },  { 'v', 5 },  { 'g', 6 },  { 'b', 7 }, { 'h', 8 },
    { 'n', 9 }, { 'j', 10 }, { 'm', 11 }, { ',', 12 }, { 'l', 13 }, { '.', 14 }, { ';', 15 }, { '/', 16 }
};
constexpr std::pair<juce::juce_wchar, int> upperRow[] = {
    { 'q', 0 }, { '2', 1 },  { 'w', 2 },  { '3', 3 },  { 'e', 4 },  { 'r', 5 },  { '5', 6 },  { 't', 7 }, { '6', 8 },
    { 'y', 9 }, { '7', 10 }, { 'u', 11 }, { 'i', 12 }, { '9', 13 }, { 'o', 14 }, { '0', 15 }, { 'p', 16 }
};
constexpr int lowestBase = 12, highestBase = 96;

/** As the Piano Roll names notes (FL: MIDI 60 is C5). */
juce::String noteName (int note) { return juce::MidiMessage::getMidiNoteName (note, true, true, 5); }
} // namespace

TypingKeyboard::TypingKeyboard (std::function<void (int, bool)> p) : play (std::move (p)) {}

TypingKeyboard::~TypingKeyboard() { releaseAll(); }

void TypingKeyboard::setOn (bool shouldBeOn)
{
    if (! shouldBeOn)
        releaseAll();
    on = shouldBeOn;
}

void TypingKeyboard::setBaseNote (int note)
{
    releaseAll();
    baseNote = juce::jlimit (lowestBase, highestBase, note);
}

int TypingKeyboard::noteFor (juce::juce_wchar character) const
{
    for (auto [key, semitones] : lowerRow)
        if (key == character)
            return baseNote + semitones;
    for (auto [key, semitones] : upperRow)
        if (key == character)
            return baseNote + 12 + semitones;
    return -1;
}

bool TypingKeyboard::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (! on || mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown())
        return false;

    const auto code = key.getKeyCode();
    const auto character = juce::CharacterFunctions::toLowerCase (key.getTextCharacter() != 0 ? key.getTextCharacter()
                                                                                              : (juce::juce_wchar)code);

    if (character == '-' || character == '=')
    {
        setBaseNote (baseNote + (character == '-' ? -12 : 12));
        return true;
    }

    const auto note = noteFor (character);
    if (note < 0 || note > 127)
        return false;
    if (! held.contains (code)) // keys repeat while held: play once
    {
        held[code] = note;
        if (play)
            play (note, true);
        startTimer (100);
    }
    return true;
}

bool TypingKeyboard::keyStateChanged()
{
    bool released = false;
    for (auto it = held.begin(); it != held.end();)
    {
        const bool down = isKeyDown ? isKeyDown (it->first) : juce::KeyPress::isKeyCurrentlyDown (it->first);
        if (down)
        {
            ++it;
            continue;
        }
        if (play)
            play (it->second, false);
        it = held.erase (it);
        released = true;
    }
    if (held.empty())
        stopTimer();
    return released;
}

void TypingKeyboard::releaseAll()
{
    for (auto& [code, note] : held)
        if (play)
            play (note, false);
    held.clear();
    stopTimer();
}

std::vector<int> TypingKeyboard::getHeldNotes() const
{
    std::vector<int> notes;
    notes.reserve (held.size());
    for (auto& [code, note] : held)
        notes.push_back (note);
    return notes;
}

juce::String TypingKeyboard::describe() const
{
    return "Typing keyboard: Z-M and Q-U play " + noteName (baseNote) + "-" + noteName (baseNote + 23) +
           " (- and = change octave)";
}

void TypingKeyboard::timerCallback() { keyStateChanged(); }

} // namespace spinus::ui
