#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <bitset>
#include <map>
#include <optional>
#include <vector>

namespace spinus
{

/** The keys an instrument (in one of its modes) plays as notes. Many sampled instruments use other keys as
    switches (a low C changes a guitar to palm mutes) or for other jobs (a strummer's chord keys), so a note that
    lands on one changes the instrument instead of playing it. The Track creator moves every note it plays on such
    an instrument onto these keys (fit): Listen, Play in Save this sound, the sounds added and the written song. */
struct PlayableKeys
{
    std::bitset<128> keys;

    /** Every key from `low` to `high`. */
    static PlayableKeys between (int low, int high);
    bool any() const noexcept { return keys.any(); }
    bool plays (int note) const noexcept { return note >= 0 && note < 128 && keys.test ((size_t)note); }
    int lowest() const noexcept;
    int highest() const noexcept;
    bool operator== (const PlayableKeys&) const = default;

    /** A note moved by octaves onto a playable key, as few as it takes (a phrase keeps its shape where it can; up
        before down when as far), or the nearest playable key when no octave of it plays. Unchanged with no keys. */
    int fit (int note) const noexcept;
    /** "E2 to G6", and how many keys between those do something else ("E2 to G6, 3 keys between do not play"). */
    juce::String describe() const;
};

/** What Spinus knows of each instrument, by the sound index's instrument key: its playing modes and, for each,
    what every key does, how to enter the mode and what it suits. Kept beside the index's folder
    (`sound-profiles.json` next to it), like the user's roles (UserRoles), so it survives a new scan. The notes the
    user set before (Notes it plays, `sound-notes.json`) come in as the first mode's playing keys, and the old file
    goes once the profiles are saved.

    Every key's job records where Spinus learned it (the user now; probing, the plugin's own report and the manual
    later), and the user's word always wins. A profile holds facts only: key numbers, names, a few words. Plain
    data: any thread but the audio thread, one at a time. */
class InstrumentProfiles
{
public:
    /** What a key does in a mode. */
    enum class Job
    {
        unknown,  // not learned yet
        plays,    // sounds as a note, at its pitch
        switches, // changes the articulation (palm mute, slide) or the mode instead of sounding
        special,  // another job in this mode (a chord key, a strum pattern, a mute)
        unused    // does nothing
    };
    enum class Source
    {
        user,
        probing,
        plugin,
        manual,
        bundled
    };
    struct Key
    {
        Job job = Job::unknown;
        juce::String what; // what it does, in a few words ("palm mute"); for switch and special keys
        Source source = Source::user;
        bool operator== (const Key&) const = default;
    };
    /** Whether the mode plays chords or one note at a time. */
    enum class Voices
    {
        unknown,
        chords,
        one
    };
    struct Mode
    {
        juce::String name = "Normal";
        juce::String enter;   // how to enter it, in words ("the Strummer button"); empty: as the instrument opens
        int enterKey = -1;    // the switch key that enters it, if one does
        juce::String goodFor; // which parts it suits ("strummed rhythm, country and folk")
        Voices voices = Voices::unknown;
        std::array<Key, 128> keys {};

        /** The keys that play, if any is known to. */
        std::optional<PlayableKeys> playable() const;
        /** Sets keys `from` to `to` (in either order) to `key`. */
        void setKeys (int from, int to, const Key& key);
        bool operator== (const Mode&) const = default;
    };
    /** A manual of the instrument (Read the manual): a PDF or other file on this computer, or a web page, and
        the plugin version it was last read for (empty until read), so a profile can say it may be out of date
        after the plugin is updated. Only where the manual is, never its text. */
    struct Manual
    {
        juce::String source; // a file's full path, or a web page's address
        juce::String version;
        bool isWebPage() const { return source.startsWith ("https://") || source.startsWith ("http://"); }
        bool operator== (const Manual&) const = default;
    };
    struct Profile
    {
        std::vector<Mode> modes;
        std::vector<Manual> manuals;
        bool operator== (const Profile&) const = default;
    };

    explicit InstrumentProfiles (const juce::File& indexFolder);
    static juce::File fileIn (const juce::File& indexFolder)
    {
        return indexFolder.getSiblingFile ("sound-profiles.json");
    }
    /** Notes it plays, before profiles: read once into them. */
    static juce::File oldNotesFileIn (const juce::File& indexFolder)
    {
        return indexFolder.getSiblingFile ("sound-notes.json");
    }

    void load();
    bool save() const;

    std::optional<Profile> get (const juce::String& instrument) const;
    /** Sets the instrument's profile (nullopt, or one with no modes: forgotten). Not saved until save(). */
    void set (const juce::String& instrument, std::optional<Profile>);
    /** The keys the instrument plays as it opens (its first mode's), if they are known. */
    std::optional<PlayableKeys> playable (const juce::String& instrument) const;

    static juce::String jobName (Job);
    static juce::String sourceName (Source);
    /** A profile as JSON, and back (a profile file's entry); nullopt when it holds no mode. */
    static juce::var toVar (const Profile&);
    static std::optional<Profile> fromVar (const juce::var&);

private:
    juce::File file, oldNotes;
    std::map<juce::String, Profile> profiles;
};

/** "C#6", middle C being C5 as in the Piano roll. */
juce::String keyName (int note);

} // namespace spinus
