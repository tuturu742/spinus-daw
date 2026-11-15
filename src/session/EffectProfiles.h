#pragma once

#include "session/InstrumentProfiles.h"

namespace spinus
{

/** What Spinus knows of each effect (Learn this effect), by its key (InstrumentPresets::libraryOf, as for an
    instrument): what it is good for, and for each of its settings what it does, a safe range and whether it
    matters most. A plugin's settings often have short names ("Thresh", "Param 7") and say nothing of what is
    sensible, so the assistant, outside agents and the Automixer read this to choose and set effects.

    Kept beside the sound index's folder (`effect-profiles.json` next to it), like the instrument profiles. Every
    setting records where Spinus learned it (the user now; the manual and listening later), and the user's word
    always wins. Facts only: setting ids and names, a few words, value texts as the plugin shows them ("-12 dB").
    Plain data: any thread but the audio thread, one at a time. */
class EffectProfiles
{
public:
    using Source = InstrumentProfiles::Source;
    using Manual = InstrumentProfiles::Manual;

    struct Setting
    {
        juce::String id;        // the plugin's parameter id
        juce::String name;      // its name when it was learned
        juce::String what;      // what it does, in a few words ("how hard the signal is pushed")
        juce::String low, high; // a safe range, as the plugin shows values ("-30 dB", "-6 dB"); empty: any
        bool matters = false;   // one of the few settings that shape the sound most
        Source source = Source::user;
        bool operator== (const Setting&) const = default;
        /** Nothing learned of it (it is not kept). */
        bool isEmpty() const { return what.isEmpty() && low.isEmpty() && high.isEmpty() && ! matters; }
    };
    struct Profile
    {
        juce::String goodFor; // what it suits ("tape-style saturation for drums and bass")
        std::vector<Setting> settings;
        std::vector<Manual> manuals;
        bool operator== (const Profile&) const = default;
        bool isEmpty() const { return goodFor.isEmpty() && settings.empty() && manuals.empty(); }
        const Setting* find (const juce::String& id) const;
        /** By id, else (a shipped profile names settings as plugins show them, whose ids differ between hosts and
           versions) by name, ignoring case. */
        const Setting* find (const juce::String& id, const juce::String& name) const;
    };

    explicit EffectProfiles (const juce::File& indexFolder);
    static juce::File fileIn (const juce::File& indexFolder)
    {
        return indexFolder.getSiblingFile ("effect-profiles.json");
    }

    void load();
    bool save() const;

    /** The effect's profile: the user's, else the one Spinus ships for it (bundled), else none. */
    std::optional<Profile> get (const juce::String& effect) const;
    /** The profile Spinus ships for one of its own effects or the free ones it gets, by the effect's key; none for
        others. Files in packaging/share/effect-profiles, each an exported profile (toShared) with the key it is
        `for`, built into Spinus; their settings are Source::bundled and may name a setting without its id. */
    static std::optional<Profile> bundled (const juce::String& effect);
    /** Sets the effect's profile (nullopt or an empty one: forgotten). Settings with nothing learned are left out.
        Not saved until save(). */
    void set (const juce::String& effect, std::optional<Profile>);

    /** What the assistant and outside agents are told of an effect: what it is good for and its learned settings
        (id, name, what it does, safe range, matters). Void when nothing is learned. */
    static juce::var describeForAgents (const Profile&);

    /** The AI's reading of the effect's manuals (Read the manual, the AI service's ai.v1.effect.readManual), as a
        proposal: what the effect is good for and what the manuals say of its settings (Source::manual). */
    struct Reading
    {
        juce::String goodFor;
        std::vector<Setting> settings; // by id; the names are the panel's
        juce::String notes, by;        // the AI's one-sentence note, and which model read it
        juce::StringArray problems;    // the manuals that could not be read, and why
    };
    /** The service's answer as a reading; nullopt when it says nothing. */
    static std::optional<Reading> readingFrom (const juce::var&);

    static juce::var toVar (const Profile&);
    static std::optional<Profile> fromVar (const juce::var&);
    /** A profile as a file to share ({kind: "spinus.effect-profile", version, effect, profile}): only the manuals
        that are web pages. */
    static juce::var toShared (const Profile&, const juce::String& effectName);
    /** A shared profile back, every setting marked as imported; nullopt when it is not one (`problem` says why). */
    static std::optional<Profile> fromShared (const juce::var&, juce::String& effectName, juce::String& problem);

private:
    juce::File file;
    std::map<juce::String, Profile> profiles;
};

} // namespace spinus
