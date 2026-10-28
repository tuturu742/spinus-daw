#pragma once

#include "model/Model.h"
#include "OfflineInstrument.h"

#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <vector>

/** The Track creator's fourth step: the new song (spinus_ai.songwriter's result) into the project, through
    commands only, as one proposal: one undo step, applied only when the user clicks Apply.

    A song (`kind: spinus.song`) has a tempo, a key, sections (each with its chords and its bars), the order the
    sections play in (`arrangement`, by bar), and per part ("drums", "bass", "chords", "lead") and section the
    notes, in ticks from the section's start (drum notes name a drum: "kick", "snare", "hat"...).

    makeProposal: the song's tempo and key; a channel for each part that has none yet (milestone 3's channels,
    "<Part> (Track creator)", are written into when they are there; otherwise Spinus's own instruments, or the
    sounds the user chose); one pattern per section ("Song 1: Verse") with every part's notes and the songs'
    swing; a Playlist track playing the sections in order, and a marker where each starts.

    A full arrangement (the song's `size` "full", spinus_ai.songwriter's layers) is laid out as a producer would:
    a channel per layer ("Kick", "Pad", "Riser"...), coloured by its family and in the Channel Rack group of it
    (Drums, Bass, Synths, Leads, FX), each on its own mixer insert of that colour, the inserts of a family
    playing into a bus of its name; a pattern per layer and section ("Song 1: Chorus - Pad", only where the
    layer plays) and one per effect ("Song 1: Riser"); and on the Playlist a folder track per family
    ("Song 1: Drums") with a track per layer under it, so the layers can be seen coming and going.

    makeRedoProposal: one part (in every section) or one section (every part) written again, into the patterns
    the song made: only those notes change (in a full arrangement, the layers made from that part).

    Plain data, no engine: any thread but the audio thread. */
namespace spinus::song
{

/** The song's parts, in order. */
const juce::StringArray& partNames();
/** The style summary's part (a stem) a song part is written from: chords from "other", lead from "vocals". */
juce::String summaryPart (const juce::String& part);
/** "Drums", "Bass", "Chords", "Lead". */
juce::String displayName (const juce::String& part);
/** The name of the part's channel, as milestone 3 names it: "Bass (Track creator)", "Chords and melody (Track
    creator)", "Lead (Track creator)", "Drums (Track creator)". */
juce::String channelName (const juce::String& part);

/** The notes a drum channel plays each drum on: the Drum Machine's pads ("kick" 60, "snare" 61, "hat" 63...),
    or General MIDI's drums (36, 38, 42...) for any other instrument. */
std::map<juce::String, int> drumNotesFor (const model::Channel&);

/** The project's channels for the song's parts: those named as milestone 3 names them (the last one of a name,
    the newest). */
std::map<juce::String, model::ChannelId> findChannels (const model::Project&);

/** What a part without a channel gets. */
struct Sound
{
    juce::var instrument;              // as channel.add takes it
    juce::String preset;               // a 3x Osc preset to load (device.loadPreset), or empty
    std::map<juce::String, int> drums; // a drum instrument's notes per drum (empty: drumNotesFor)
    std::optional<double> volumeDb;    // its insert's fader (startingFaders), or as it is
};
/** Spinus's own: the Drum Machine, 3x Osc with "Pluck Bass", "Soft Keys" and "Saw Lead". */
Sound defaultSound (const juce::String& part);

/** A layer of a full arrangement (the song's `layers`). */
struct Layer
{
    juce::String id, name, family;     // a part of the user's list (parts::SongPart): its id and name
    juce::String colour;               // its own colour (#AARRGGBB, the AI's choice), or empty: its family's
    juce::String part;                 // the song part it is made from ("fx" for the effects)
    juce::String role;                 // what it plays: parts::songRoles()
    juce::String soundRole;            // the role of sound it wants (the sound index's roles)
    bool drums = false;                // its notes name drums
    int instance = 0;                  // how many parts of its role come before it
    int low = 0, middle = 0, high = 0; // the notes it plays (0: unknown)
};
/** Whether the song has its parts as layers (spinus_ai.songwriter writes them so). */
bool isFull (const juce::var& song);
/** The song's layers (its parts), in order (none for a song of the four fixed parts). */
std::vector<Layer> layersOf (const juce::var& song);
/** The colour of a family's channels, inserts and tracks (#AARRGGBB), as the demo songs have them. */
juce::String familyColour (const juce::String& family);
/** A layer's colour: its own, else its family's. */
juce::String layerColour (const Layer&);
/** Spinus's own sound for a layer: the Drum Machine for drums, 3x Osc with a preset of its role otherwise
    ("Sub Bass", "Warm Pad", "Pluck", "Bell", "Noise Riser"...). */
Sound defaultSound (const Layer&);
/** The channels the Sounds page added for the layers ("<name> (Track creator)", or milestone 3's names for the
    first drums, bass, chords and lead), by layer id: the song writes into them. */
std::map<juce::String, model::ChannelId> findLayerChannels (const model::Project&, const std::vector<Layer>&);
/** The newest song the Track creator put into the project ("Song 2"), or empty. */
juce::String lastSongName (const model::Project&);
/** The steps that take song `name` out of the project: its patterns (and their clips), its Playlist tracks and
    markers, and the channels only it plays (with their inserts and its family buses); the Sounds page's
    channels stay. `start` gets where it started on the Playlist (-1: nowhere). */
juce::Array<juce::var> removeSongSteps (const model::Project&, const juce::String& name, model::Ticks& start);

struct Options
{
    std::map<juce::String, model::ChannelId> channels; // channels to write into (findChannels; by layer id:
                                                       // findLayerChannels)
    std::map<juce::String, Sound> sounds;              // for the parts without one (defaultSound otherwise)
    std::map<juce::String, Sound> layerSounds;         // full arrangement: per layer id (defaultSound otherwise)
    std::map<juce::String, int> drums;                 // the drum channel's notes per drum, when known
    juce::String name = "Song 1";                      // names the patterns ("Song 1: Verse") and the track
    juce::String replace; // a song of the project the new one replaces, in its place ("Song 1"), or none
};

/** A name for the next song in the project: "Song 1", or "Song 2" when patterns of Song 1 are there, ... */
juce::String nextSongName (const model::Project&);

/** The proposal ({summary, steps}) that puts the whole song into the project (see above). ai::Proposal checks
    it; applying it is one undo step. An empty var when the song is not one (`error` says why). */
juce::var makeProposal (const juce::var& song, const model::Project&, const Options&, juce::String* error = nullptr);

/** What an applied song made: its patterns per section and its channels per part. */
struct Applied
{
    juce::String name;                                        // "Song 1"
    std::map<juce::String, model::PatternId> patterns;        // section id -> pattern
    std::map<juce::String, model::ChannelId> channels;        // part -> channel
    std::map<juce::String, int> drums;                        // the drum channel's notes per drum
    std::map<juce::String, std::vector<model::ClipId>> clips; // section id -> its clips on the song's track

    // A full arrangement: per layer id.
    bool full = false;
    std::map<juce::String, std::map<juce::String, model::PatternId>> layerPatterns;        // -> section -> pattern
    std::map<juce::String, model::ChannelId> layerChannels;                                // -> its channel
    std::map<juce::String, std::map<juce::String, int>> layerDrums;                        // -> notes per drum
    std::map<juce::String, std::map<juce::String, std::vector<model::ClipId>>> layerClips; // -> section -> clips

    /** Whether its patterns and channels are all still in the project (not undone or deleted). */
    bool isIn (const model::Project&) const;
};
/** After makeProposal's proposal was applied: the patterns and channels it made or wrote into, found by name. */
Applied findApplied (const juce::var& song, const model::Project&, const juce::String& name);

/** The proposal that writes one part (`part`, in every section) or one section (`section`, every part) of `song`
    into what an earlier song made (`applied`): pattern.setNotes on those patterns only. Empty when it cannot
    (`error` says why). */
juce::var makeRedoProposal (const juce::var& song, const Applied&, const juce::String& part,
                            const juce::String& section, juce::String* error = nullptr);

/** A layer's notes in its busiest section, in seconds from that section's start (at most `seconds` of them),
    drums played on `drums`' notes: how the part sounds with its sound, to measure how loud it is. None when it
    plays nothing. */
std::vector<OfflineInstrument::Note> layerExcerpt (const juce::var& song, const Layer&,
                                                   const std::map<juce::String, int>& drums, double seconds = 12.0);

/** How loud a part of each role usually sits in a mix, in dB next to the drums (0): the bass close to them, a
    pad well under. */
double roleLevelDb (const juce::String& role);

/** A starting balance for a written song's layers: from each layer's role and how loud its excerpt measured
    with its sound (dB RMS), its fader, so that every part sits at its role's level; the loudest fader is 0 dB
    (nothing is turned up past where it was), none under -30 dB. By layer id; layers not measured are left out. */
std::map<juce::String, double> startingFaders (const std::vector<Layer>&,
                                               const std::map<juce::String, double>& measured);

/** What will be written, in plain words (the song's `plain`), and its numbers next to the style's. */
juce::String describe (const juce::var& song);

} // namespace spinus::song
