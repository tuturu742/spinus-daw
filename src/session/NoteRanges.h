#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <optional>

namespace spinus
{

/** The notes the user lets an instrument play ("from E2 to G6"), by the sound index's instrument key. Many sampled
    instruments use keys outside their playing range as switches (a low C changes a guitar to palm mutes or slides),
    so a phrase or a song part that reaches them changes the instrument instead of playing it. Kept beside the
    index's folder (`sound-notes.json` next to it), like the user's roles (UserRoles), so they survive a new scan.
    The Track creator moves every note it plays on such an instrument (Listen, Play in Save this sound, the
    sounds added to the project and the written song) by octaves into the range (fitToRange). Plain data: any thread
    but the audio thread, one at a time. */
class NoteRanges
{
public:
    struct Range
    {
        int low = 0, high = 127;
        bool operator== (const Range&) const = default;
    };

    explicit NoteRanges (const juce::File& indexFolder);
    static juce::File fileIn (const juce::File& indexFolder) { return indexFolder.getSiblingFile ("sound-notes.json"); }

    void load();
    bool save() const;

    std::optional<Range> get (const juce::String& instrument) const;
    /** Sets the instrument's range (nullopt: any note again). Not saved until save(). */
    void set (const juce::String& instrument, std::optional<Range>);

    /** A note moved by octaves into the range, as few as it takes (a phrase keeps its shape where it can), or the
        nearest end when the range is less than an octave and no octave of it fits. */
    static int fit (int note, Range);
    /** "E2 to G6", middle C being C5 as in the Piano roll. */
    static juce::String describe (Range);
    static juce::String noteName (int note);

private:
    juce::File file;
    std::map<juce::String, Range> ranges;
};

} // namespace spinus
