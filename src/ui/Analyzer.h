#pragma once

#include "core/FileDialog.h"
#include "Workspace.h"
#include "ai/AiService.h"
#include "session/AnalysisLibrary.h"
#include "session/Session.h"
#include "session/PresetPreviews.h"
#include "session/StemSamples.h"
#include "session/SynthMatch.h"
#include "MessageLabel.h"

namespace spinus::ui
{

/** The Analyzer: drop a song in to learn from it. Each step shows its
    progress:

    1. Prepare: the song is decoded to a WAV file in the project's cache (cache/analysis).
    2. Tempo and beats (Beat This!), then the key: each with "Use for the song".
    3. Stems (Demucs, 4 or 6, or MVSep's many instruments), on request. Each stem is then left out,
       replicated (notes, played by the sound source chosen beside it, in the way chosen next to that) or
       added as audio (a Playlist track "<Stem> stem (<song>)", lined up so the song's first downbeat is on
       bar 1), all as one undo step; when that cannot be done, nothing is added and the status line says why.

    It needs AI turned on, the analysis engine installed (Options > AI > Analysis engine) and the model
    files downloaded; it says what is missing for the choices made (the Stems box, the stems' sounds) and
    offers to fix it (downloads state their size first), then carries on with what was asked for.
    A notice says that the models are open-source ones made by others, and that analyzing songs is for
    study and personal use.

    Message thread only (decoding runs on a thread of its own). */
class Analyzer final : public Panel,
                       public juce::FileDragAndDropTarget,
                       private juce::AsyncUpdater,
                       private juce::ChangeListener
{
public:
    Analyzer (Session&, ai::AiService*);
    ~Analyzer() override;

    /** The AI service (the main window gets it after making its panels). */
    void setService (ai::AiService*);

    /** Opens Options > AI on the analysis engine page. */
    std::function<void()> onSetUpEngine;

    enum class Step
    {
        idle,
        preparing,
        tempo,
        key,
        separating,
        transcribing,
        downloading,
        done
    };

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.
    bool analyze (const juce::File& song);
    /** Downloads the model files the analysis needs (the user agreed to their size). */
    void downloadModels();
    void showSources();
    /** separate()'s choice for many instruments (up to 53 kinds; only those heard are kept). */
    static constexpr int manyInstruments = 53;
    /** Splits the song into 4 or 6 stems, or many instruments (and chooses that in the Stems box). */
    bool separate (int stems);
    /** Chooses 4, 6 or manyInstruments in the Stems box, as the user does: what the window says it needs
        to download follows at once. */
    void setStemChoice (int stems);
    int getStemChoice() const { return stemCount.getSelectedId(); }
    /** The model file a choice of the Stems box separates with ("htdemucs", "htdemucs_6s", "mvsep_mega_53"). */
    static juce::String separationModel (int stems);
    /** "lead-vocal" -> "lead vocals", "hh" -> "hi-hat": a stem's name as the window shows it. */
    static juce::String stemDisplayName (const juce::String& stem);
    void cancel();

    /** The analysis library (shared with the Track creator; none without the AI service). What the Analyzer
        finds goes there: tempo, key, stems (as FLAC) and notes, by the song's contents. */
    AnalysisLibrary getLibrary() const;
    /** Opens the library's window (its songs, the space they take, deleting). */
    void showLibrary();
    /** The song was analyzed before (the library has its tempo, key or stems): analyze() stops once the song
        is prepared, and offers to continue from it or to analyze it anew. */
    bool hasEarlierAnalysis() const;
    /** Continues from the earlier analysis: its tempo, key and kept stems (no separating again), and later the
        notes found before. What is missing is found as usual. */
    bool continueFromEarlier();
    /** Analyzes the song again from the start, replacing what was kept (its notes are found again too). */
    bool analyzeAnew();
    bool useTempo();
    bool useKey();
    /** Puts these stems on Playlist tracks of their own, as audio. One undo step. */
    bool addStemsAsAudio (const juce::StringArray& stemNames);

    /** What happens to a stem. */
    enum class StemUse
    {
        leaveOut,
        replicate, // notes, played by the sound chosen for it (makeNotesWith)
        audio      // the stem itself on a Playlist track (addStemsAsAudio)
    };
    /** How the chosen sound source plays a replicated stem (the second list beside it). For the drums on the
        Drum Machine: asIs is its Synth Kit, closestPreset the closest drum samples. */
    enum class Way
    {
        asIs,          // the instrument's own default sound
        closestPreset, // the instrument's preset closest to the stem (Surge XT, OB-Xf, Odin 2, any instrument with
                       // presets Spinus can choose from)
        tuned          // the synth tuned until it sounds like the stem (slow; experimental for synths Spinus has
                       // no profile of its own for)
    };
    /** What plays the notes made from stems. */
    enum class Sounds
    {
        spinus,         // 3x Osc, and the Drum Machine's Synth Kit
        song,           // the song's own sounds: its drum hits on the pads, its phrases in a Sampler
        presets,        // the instrument chosen for the stem (Surge XT if none) with the preset closest to it
        tunedSurge,     // Surge XT, from the closest preset, tuned to sound like each stem
        tunedThreeOsc,  // 3x Osc tuned to sound like each stem
        instrument,     // an instrument of the user's choice, e.g. an installed plugin (instrumentPerStem)
        tunedInstrument // that instrument tuned to sound like each stem (experimental unless Spinus has a
                        // tested profile for it, SynthProfile::isTested)
    };

    /** Turns stems into notes: a new channel per stem, a pattern "Analysis: <song>" with their notes on the
        song's beats, placed on the Playlist at bar 1. One undo step.
        With Spinus's instruments, melodic stems (bass, vocals, other, piano, guitar) become notes found by
        Basic Pitch on 3x Osc, and drums become steps on a Drum Machine with its Synth Kit.
        With the song's own sounds, the drums' pads get the song's clearest hit of each drum,
        and each melodic stem becomes a Sampler holding its phrases, one key each, with the notes that play
        them back where they were.
        With the closest presets, each melodic stem's notes play on Surge XT (or the instrument chosen for it:
        OB-Xf, Odin 2, or any instrument whose presets Spinus can choose from, InstrumentPresets) with the
        preset that sounds most like the stem (CLAP and plain measurements, ai.v1.presets.*). The first time,
        Spinus records each of the instrument's presets playing a short phrase and the AI service measures
        them (kept for next time, per preset). An instrument without presets plays as it is, and the status
        line says so.
        Tuned, each melodic stem's synth (Surge XT from the closest preset, or 3x Osc) has its main settings
        tuned until it sounds like the stem's busiest few seconds playing the same notes (SynthMatcher):
        about half a minute per stem. Cancel keeps what was found so far. Any other instrument can be tuned
        too (Sounds::tunedInstrument): Odin 2 and OB-Xf with Spinus's profiles, others with the user's own
        profile or one made from the names of their settings; one with too few settings to tune plays as it
        is, and the status line says so. */
    bool makeNotes (const juce::StringArray& stemNames, Sounds = Sounds::spinus);
    /** The same, with what plays the notes chosen per stem (the drums: spinus, song or presets, the last
        meaning the closest drum samples). */
    bool makeNotesWith (const std::map<juce::String, Sounds>& soundPerStem,
                        const std::map<juce::String, model::Device>& instrumentPerStem = {});
    /** Each stem as chosen: replicated with its sound (spinus when not given), added as audio, or left out.
        One undo step for all of them, once the notes are made. */
    bool addStems (const std::map<juce::String, StemUse>& usePerStem,
                   const std::map<juce::String, Sounds>& soundPerStem = {},
                   const std::map<juce::String, model::Device>& instrumentPerStem = {});
    /** The window's choices beside each stem (what "Add to the project" does). */
    std::map<juce::String, StemUse> getChosenUses() const;
    std::map<juce::String, Sounds> getChosenSounds() const;
    /** The instruments chosen for stems whose sound is Sounds::instrument (the window's own plugins). */
    std::map<juce::String, model::Device> getChosenInstruments() const;
    void setChosenUse (const juce::String& stem, StemUse);
    /** The sound sources offered for a stem (the first list beside it): for a melodic stem its own phrases,
        then every instrument in alphabetical order (Spinus's 3x Osc, the free open-source instruments that
        are downloaded and every installed instrument plugin, each once); for the drums the Drum Machine or the song's
       own hits. */
    juce::StringArray getSoundSources (bool forDrums) const;
    /** The ways a stem's chosen source can play it (what the second list offers; one: no second list). */
    std::vector<Way> getWays (const juce::String& stem) const;
    juce::String getChosenSource (const juce::String& stem) const;
    /** The second list's choices for a stem, as the window shows them (e.g. "Tuned to it (experimental)"). */
    juce::StringArray getWayNames (const juce::String& stem) const;
    /** Chooses a stem's source (by its name in the list) and way, as the user does. False if there is no such
        stem or source, or the source cannot play it that way. */
    bool setChosenSound (const juce::String& stem, const juce::String& source, Way = Way::asIs);
    /** What "Add to the project" does: addStems() with the window's choices. Says in the status line why,
        when nothing can be added. */
    bool addChosen();

    /** For each stem made with the closest presets: the presets that came closest, best first. */
    const std::map<juce::String, std::vector<InstrumentPreset>>& getClosestPresets() const noexcept { return closest; }
    /** The drums stem's sounds: hits of one kind that sound clearly different are separate groups on pads
        of their own. */
    struct DrumGroup
    {
        int pad = 60;
        juce::String kind, name;
        int count = 0;
        double start = 0.0, end = 0.0; // its clearest hit, in seconds of the stem
    };
    const std::vector<DrumGroup>& getDrumGroups() const noexcept { return drumGroups; }
    /** With the closest presets or tuned synths, the drum samples (Spinus's Synth Kit and the drum samples
        folder) that sound most like each group, best first, by pad. */
    const std::map<int, std::vector<juce::File>>& getDrumSuggestions() const noexcept { return drumSuggestions; }
    /** Loads one of a group's suggested samples onto its pad. */
    bool useDrumSuggestion (int pad, int index);
    /** The folder of drum samples to suggest from (searched with its subfolders); remembered. */
    void setDrumSamplesFolder (const juce::File&);
    juce::File getDrumSamplesFolder() const { return drumSamplesFolder; }

    /** For each stem tuned: what the synth match found. */
    const std::map<juce::String, SynthMatcher::Result>& getTuned() const noexcept { return tuned; }
    /** Tests: fewer tries for synth match. */
    void limitTriesForTests (int most) { tryLimit = most; }
    /** Tests: measure only the first presets (all of them otherwise). */
    void limitPresetsForTests (int most) { presetLimit = most; }
    /** Loads one of a stem's closest presets into its channel's instrument (as its preset browser does). */
    bool useClosestPreset (const juce::String& stem, int index);
    const std::map<juce::String, std::vector<model::Note>>& getTranscribed() const noexcept { return transcribed; }
    const std::map<juce::String, std::vector<StemPiece>>& getPieces() const noexcept { return pieces; }

    Step getStep() const noexcept { return step; }
    bool isBusy() const noexcept { return step != Step::idle && step != Step::done; }
    juce::File getSong() const { return song; }
    juce::File getPreparedFile() const { return prepared; }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getTempoText() const { return tempoText.getText(); }
    juce::String getKeyText() const { return keyText.getText(); }
    juce::String getNeedsText() const { return needsText.getText(); }
    struct ModelSource
    {
        juce::String name, license;
        juce::int64 size = 0;
        juce::String page; // to read about it and check its licence
        bool downloaded = false;
        juce::String id; // e.g. "htdemucs"
    };
    /** The model files still missing (tests). */
    const std::vector<ModelSource>& getMissingSources() const noexcept { return missingSources; }
    /** Every model the Analyzer may use, downloaded or not, as last listed (tests). */
    const std::vector<ModelSource>& getAllSources() const noexcept { return allSources; }
    juce::int64 getDownloadSize() const noexcept { return downloadSize; }

    struct Rhythm
    {
        double tempo = 0.0, firstDownbeat = 0.0;
        int beatsPerBar = 4;
    };
    std::optional<Rhythm> getRhythm() const { return rhythm; }
    std::optional<model::Key> getKey() const { return key; }
    const std::map<juce::String, juce::File>& getStems() const noexcept { return stems; }

    juce::Rectangle<int> getDefaultBounds() const override { return { 80, 60, 660, 560 }; }

    /** Where analysis files go: the project's cache/analysis, or a temporary folder while untitled. */
    juce::File getCacheFolder() const;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Decoder;
    class Steps;

    void handleAsyncUpdate() override;                               // the decoder finished
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the device list changed
    void checkReady (const std::function<void()>& then, const juce::StringArray& alsoNeeded = {});
    /** The model files the window's choices need: the beat model, Basic Pitch, the separation model the Stems
        box names, and CLAP when a stem is to be replicated with sounds compared by ear. */
    juce::StringArray neededModels() const;
    /** Says which of the needed model files are missing (from the last list of models), with the Download
        button; nothing once they are all there. */
    void showNeeds (const juce::StringArray& alsoNeeded = {});
    void readModels (const juce::var& list);
    /** Each instrument's presets, one instrument after another: found, then the ones not measured yet
        recorded and measured. */
    void measurePresets (std::vector<model::Device> instruments, size_t index, const std::function<void()>& then);
    void measureBatch (const juce::String& instrument, std::vector<juce::String> missing, size_t from,
                       const std::function<void()>& then);
    void updatePresetChoices();
    void tuneSynth (const juce::String& stem);
    bool usesClap (const juce::String& stem) const;
    /** The stem plays on an instrument with its closest preset, or is tuned starting from it. */
    bool usesPresets (const juce::String& stem) const;
    /** What plays a melodic stem's notes (Surge XT for the closest presets when none is chosen). */
    model::Device instrumentOf (const juce::String& stem) const;
    juce::String instrumentName (const model::Device&) const;
    static juce::String instrumentKey (const model::Device&);
    /** Whether an installed instrument may have presets: false only once Spinus has looked and found none
        (instrument-presets.json beside analyzer.json). */
    bool mayHavePresets (const model::Device&) const;
    void rememberPresets (const model::Device&, int count);
    void measureDrumSamples (const std::function<void()>& then);
    void addSounds (std::vector<juce::String> missing, size_t from, const std::function<void()>& then);
    void matchDrumGroups (size_t index, const std::function<void()>& then);
    juce::File getSettingsFile() const;
    void runJob (const juce::String& method, const juce::var& params, const juce::String& what,
                 const std::function<void (const juce::var&)>& done);
    void findTempo();
    void transcribeNext();
    bool addNotesToProject();
    void cannotAdd (const juce::String& why); // says so in the status line
    bool addAudioSteps (Steps&, const juce::StringArray& names, int& added);
    void updateNoteChoices();

    /** A sound source in a stem's first list. */
    struct Source
    {
        enum class Kind
        {
            own,         // the song's own phrases (Sampler) or drum hits (Drum Machine)
            drumMachine, // the Drum Machine
            threeOsc,    // Spinus's 3x Osc
            surge,       // Surge XT, downloaded through Spinus
            device       // another instrument: a free open-source one or an installed plugin
        };
        juce::String name, key; // key: the same source after the device list changes
        Kind kind = Kind::device;
        model::Device device; // for Kind::device and Kind::surge
        std::vector<Way> ways { Way::asIs };
        bool experimental = false;     // tuning it (and playing its presets) is experimental: no tested profile
        bool startsFromPreset = false; // tuned from its closest preset (its profile's start)
    };
    std::vector<Source> makeSources (bool forDrums) const;
    struct StemRow
    {
        std::unique_ptr<juce::ComboBox> use, source, way;
        std::vector<Source> sources;
        const Source* chosen() const;
        Way chosenWay() const;
    };
    void fillSources (const juce::String& stem, StemRow&, const juce::String& keepKey);
    void fillWays (const juce::String& stem, StemRow&);
    bool replicates (const juce::String& stem) const;
    static Sounds soundFor (const Source&, Way);
    void findKey();
    void applyTempo (const juce::var& result);
    void applyKey (const juce::var& result);
    /** Uses a separation's stems kept in the library (decoded into the cache folder), then `then`. */
    void useKeptStems (const juce::String& model, const std::function<void()>& then);
    void showStems (const juce::String& model);
    void fail (const juce::String& message);
    void setStep (Step, const juce::String& message = {});
    void updateButtons();

    Session& session;
    ai::AiService* service;
    int listener = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    Step step = Step::idle;
    juce::File song, prepared;
    juce::String songDigest;                  // its contents' SHA-256: its name in the analysis library
    juce::var earlier;                        // what the library had of it (void when analyzed anew)
    bool choosing = false;                    // asking whether to continue from the earlier analysis
    juce::String stemsModel;                  // the separation model of `stems`
    std::unique_ptr<BackgroundTask> stemTask; // keeping or restoring stems
    std::unique_ptr<Decoder> decoder;
    juce::String job, beatModel;
    std::vector<juce::String> missingModels;
    std::vector<ModelSource> missingSources, allSources;
    std::map<juce::String, model::Device> stemInstruments; // what plays the stems with Sounds::instrument
    bool needsShowModels = false;                          // needsText lists missing model files (not another problem)
    std::function<void()> resume; // what was asked for when model files were missing: done once they are there
    bool clapWanted = false;      // the notes being made compare sounds by ear (CLAP)
    juce::int64 downloadSize = 0;
    double progress = -1.0;
    std::optional<Rhythm> rhythm;
    std::optional<model::Key> key;
    std::map<juce::String, juce::File> stems;
    std::vector<double> beatTimes;
    juce::StringArray notesQueue;
    std::map<juce::String, std::vector<model::Note>> transcribed;
    std::map<juce::String, std::vector<StemPiece>> pieces; // the song's own sounds, by stem
    std::map<juce::String, Sounds> stemSounds;             // what plays each stem's notes, this time
    Sounds soundOf (const juce::String& stem) const;
    std::unique_ptr<PresetPreviews> previews; // while measuring an instrument's presets
    struct PresetSet
    {
        juce::String name, library; // the instrument's name, and its index at the AI service
        std::vector<InstrumentPreset> presets;
    };
    std::map<juce::String, PresetSet> presetSets; // by instrumentKey
    std::map<juce::String, int> presetCounts;     // instrumentKey -> how many presets Spinus found
    int presetLimit = -1, tryLimit = -1;
    std::unique_ptr<SynthMatcher> matcher;
    juce::StringArray tuneNotes; // instruments that could not be tuned, said with the result
    std::map<juce::String, SynthMatcher::Result> tuned;
    std::map<juce::String, std::vector<HiddenInstrument::Note>> heard; // the notes, in seconds of the stem
    std::vector<DrumGroup> drumGroups;
    std::map<int, std::vector<juce::File>> drumSuggestions;
    juce::File drumSamplesFolder;
    std::map<juce::String, juce::File> drumCandidates; // id -> file
    std::map<juce::String, std::vector<InstrumentPreset>> closest;
    std::map<juce::String, model::ChannelId> madeChannels;
    std::map<juce::String, std::unique_ptr<juce::ComboBox>> presetChoices;
    std::map<juce::String, StemRow> rows; // each stem: what happens to it, and what plays it how
    juce::StringArray audioStems;         // added as audio with the notes being made, in their undo step
    mutable juce::File tempFolder;

    MessageLabel status, notice;
    juce::Label intro, songText, tempoLabel, tempoText, keyLabel, keyText, stemsLabel, stemsText, needsText;
    juce::TextButton chooseButton { "Choose a song..." }, useTempoButton { "Use for the song" },
        useKeyButton { "Use for the song" }, separateButton { "Separate" }, downloadButton { "Download" },
        sourcesButton { "Sources and licences..." }, addButton { "Add to the project" },
        engineButton { "Set up the analysis engine..." }, cancelButton { "Cancel" },
        drumFolderButton { "Drum samples..." }, continueButton { "Continue from the existing analysis" },
        anewButton { "Analyze anew" }, libraryButton { "Saved analyses..." };
    juce::ComboBox stemCount;
    juce::Viewport stemsView;
    juce::Component stemRows; // the stems' rows, scrolled by stemsView
    juce::ProgressBar bar { progress };
    std::unique_ptr<core::FileDialog> chooser;
};

} // namespace spinus::ui
