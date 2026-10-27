#pragma once

#include <juce_core/juce_core.h>

#include <map>

namespace spinus
{

/** The roles the user gave the sound bank's instruments and sounds ("this plugin's default sound is a rhythm
    guitar"), by the sound index's instrument key and a preset's id (empty: the instrument as a whole). Kept
    beside the index's folder (`sound-roles.json` next to it), which nothing of the index writes or deletes, so they
    survive a new scan and deleting the index; the Track creator's choosing (parts::choose), its parts from an
   instrument and its notes use them before any role guessed from a name or a sound. Roles are the song's
   (parts::songRoles()). Plain data: any thread but the audio thread, one at a time. */
class UserRoles
{
public:
    explicit UserRoles (const juce::File& indexFolder);
    static juce::File fileIn (const juce::File& indexFolder) { return indexFolder.getSiblingFile ("sound-roles.json"); }

    void load();
    bool save() const;

    /** The role the user gave the preset of the instrument, else the instrument; empty when none. */
    juce::String get (const juce::String& instrument, const juce::String& preset = {}) const;
    /** Gives a role (empty: forgets it). Not saved until save(). */
    void set (const juce::String& instrument, const juce::String& preset, const juce::String& role);
    bool isEmpty() const noexcept { return roles.empty(); }

private:
    juce::File file;
    std::map<juce::String, std::map<juce::String, juce::String>> roles; // instrument -> preset ("": all) -> role
};

} // namespace spinus
