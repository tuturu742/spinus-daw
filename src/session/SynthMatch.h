#pragma once

#include "HiddenInstrument.h"

#include <random>

namespace spinus
{

/** Synth match: a synth's settings tuned so that it sounds like a stem playing the
    stem's own notes. A starting point to shape by ear, not a copy.

    Which settings are tuned comes from a synth profile: built in for Surge XT, 3x Osc, Odin 2 and OB-Xf,
    and any `*.json` file in ~/.local/share/spinus/synth-profiles (or $XDG_DATA_HOME/spinus/synth-profiles):

        { "name": "3x Osc", "stockType": "spinus.3x-osc", "start": "default",
          "settings": [ { "id": "cutoff" }, { "id": "osc1_shape", "steps": 5 },
                        { "name": "A Filter 1 Cutoff", "min": 0.2, "max": 1.0 } ] }

    A profile is for a synth of Spinus's own or downloaded through it ("stockType"), or for an installed
    plugin ("plugin": its name, any case; "manufacturer" and "format" narrow it down). A setting is found by
    its parameter ID ("id") or name ("name"); "min" and "max" limit it (0 to 1, the plugin's own scale);
    "steps" makes it a choice of that many values. "start" is "default" (the synth's own sound) or
    "closestPreset" (the closest preset, for any synth with presets: InstrumentPresets); "tries" how many sounds the
   search may try.

    Any other instrument gets an automatic profile, made from its parameters' names (makeAutomatic()). */
struct SynthProfile
{
    struct Setting
    {
        juce::String id, name;
        float min = 0.0f, max = 1.0f;
        int steps = 0; // 0: continuous
    };

    juce::String name, stockType, start = "default";
    juce::String plugin, manufacturer, format; // an installed plugin's name, maker and format
    int tries = 0;                             // 0: defaultTries
    bool automatic = false;                    // made by makeAutomatic(), not written for the synth
    std::vector<Setting> settings;

    static constexpr int defaultTries = 600;
    /** An automatic profile with fewer settings than this does not tune: the synth plays as it is. */
    static constexpr int fewestSettings = 3;
    static constexpr int mostAutomaticSettings = 20;
    int getTries() const noexcept { return tries > 0 ? tries : defaultTries; }

    /** Whether this profile is for that instrument (by stock type, or by the plugin's name, maker and format;
        a downloaded plugin also by its own name, e.g. "Odin2"). */
    bool isFor (const model::Device&) const;

    static std::optional<SynthProfile> fromJson (const juce::var&);
    /** Spinus's own profiles, then the user's (a user profile replaces a built-in one for the same synth). */
    static std::vector<SynthProfile> getAll();
    static std::optional<SynthProfile> find (const juce::String& stockType);
    /** The profile for this instrument: the user's own when there is one, else Spinus's. None: makeAutomatic(). */
    static std::optional<SynthProfile> find (const model::Device&);
    /** Whether Spinus has a profile of its own, tried and measured, for this instrument (Surge XT, 3x Osc,
        Odin 2, OB-Xf). Tuning others is experimental. */
    static bool isTested (const model::Device&);
    static juce::File getUserFolder();

    /** What makeAutomatic() needs to know of a plugin's parameter. */
    struct Parameter
    {
        juce::String id, name;
        int numSteps = 0x7fffffff; // juce::AudioProcessor::getDefaultNumParameterSteps(): continuous
        bool isBoolean = false, isAutomatable = true, isMeta = false;
    };
    static Parameter describe (const juce::AudioProcessorParameter&);

    /** A profile made from parameter names: those that shape the sound (filter cutoff and resonance, the
        envelopes, the oscillators' waves, levels, pulse widths and detune, drive), at most
        mostAutomaticSettings, in that order. Volume, tuning, voices, switches, effects, modulation,
        arpeggiators and presets are left out. None when fewer than fewestSettings are found. */
    static std::optional<SynthProfile> makeAutomatic (const juce::String& synthName, const std::vector<Parameter>&);
};

/** How different two sounds are, ignoring overall loudness: log-magnitude spectra compared at three
    resolutions (256, 1024 and 4096-sample frames) plus how their loudness moves (10 ms levels), both made
    equally loud first. 0 for the same sound. */
double spectralDistance (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b);

/** The part of a stem to match: the window (at most `longest` seconds) holding the most note time, with
    the notes that start in it (times from the window's start, cut at its end). */
struct Excerpt
{
    double start = 0.0, length = 0.0;
    std::vector<HiddenInstrument::Note> notes;
};
Excerpt chooseExcerpt (const std::vector<HiddenInstrument::Note>& notes, double longest = 3.0);

/** That part of an audio file, mono at HiddenInstrument::sampleRate, with `tail` seconds more (as the
    synth renders a tail after the last note). Empty if the file cannot be read. */
juce::AudioBuffer<float> readExcerpt (const juce::File&, double start, double length, double tail = 0.2);

/** CMA-ES with a diagonal covariance (sep-CMA-ES, Ros and Hansen 2008), on [0, 1] in every dimension:
    each generation asks for `getPopulation()` points and is told how good each was (lower is better).
    Points may lie outside [0, 1]: fold() reflects them back in, and that is where they are tried. */
class Cmaes
{
public:
    Cmaes (std::vector<double> start, double sigma, uint32_t seed = 1);

    int getPopulation() const noexcept { return lambda; }
    std::vector<std::vector<double>> ask();
    void tell (const std::vector<std::vector<double>>& points, const std::vector<double>& losses);

    static double fold (double);
    const std::vector<double>& getBest() const noexcept { return best; }
    double getBestLoss() const noexcept { return bestLoss; }
    double getSigma() const noexcept { return sigma; }

private:
    size_t n;
    int lambda, mu;
    std::vector<double> weights, mean, diagonal, pathSigma, pathC, best;
    double sigma, muEff, cSigma, dSigma, cC, c1, cMu, chiN, bestLoss = std::numeric_limits<double>::max();
    std::vector<std::vector<double>> lastZ;
    std::mt19937 random;
};

/** Tunes a hidden copy of a synth on a thread of its own: each try sets the profile's settings, plays the
    notes and compares the sound with the target. Progress and the end are reported on the message thread. */
class SynthMatcher : private juce::Thread, private juce::AsyncUpdater
{
public:
    /** For the profile's synth (its stockType). */
    SynthMatcher (Session&, SynthProfile);
    /** For any instrument: with this profile, or without one an automatic profile made from its parameters
        (SynthProfile::makeAutomatic). Check getNumSettings(): an instrument with too few to tune has none. */
    SynthMatcher (Session&, const model::Device&, std::optional<SynthProfile> = std::nullopt);
    ~SynthMatcher() override;

    juce::String getProblem() const { return instrument.getProblem(); }
    const SynthProfile& getProfile() const noexcept { return profile; }
    /** The settings the profile names that the synth really has (the others are left out). */
    int getNumSettings() const noexcept { return (int)parameters.size(); }

    /** Starts from this preset (message thread; for "closestPreset" profiles). */
    juce::Result loadPreset (const juce::File& preset) { return instrument.loadPreset (preset); }
    juce::Result loadPreset (const InstrumentPreset& preset) { return instrument.loadPreset (preset); }

    /** Tunes for up to `tries` renders. `progress` gets (tries done, best distance), `finished` the result. */
    void start (juce::AudioBuffer<float> target, std::vector<HiddenInstrument::Note> notes, int tries,
                std::function<void (int, double)> progress, std::function<void()> finished);
    /** Stops early, keeping the best so far (finished is still called). */
    void stop() { signalThreadShouldExit(); }

    struct Result
    {
        std::vector<std::pair<juce::String, float>> settings; // parameter ID, value (0 to 1)
        double startDistance = 0.0, distance = 0.0;
        int tries = 0;
    };
    /** After finished: the best settings found. */
    const Result& getResult() const noexcept { return result; }

    /** Sets these settings on a channel's instrument (message thread): the user's synth then sounds like the
        match. Settings the instrument does not have are skipped. */
    static int apply (Session&, model::ChannelId, const Result&);

    /** A parameter's ID as Result and profiles keep it: Spinus's parameter ID (engine/ParameterIds.h, as
        device.setParameters and automation use). */
    static juce::String getParameterId (const juce::AudioProcessorParameter&);
    /** By Spinus's parameter ID; a profile's "id" may also be the plugin's own ID (as profiles had it before). */
    static juce::AudioProcessorParameter* findParameter (const juce::AudioPluginInstance&, const juce::String& id);

private:
    void findSettings();
    void run() override;
    void handleAsyncUpdate() override;
    std::vector<float> toValues (const std::vector<double>& point) const;
    double tryValues (const std::vector<float>& values);

    SynthProfile profile;
    HiddenInstrument instrument;
    std::vector<std::pair<juce::AudioProcessorParameter*, SynthProfile::Setting>> parameters;
    juce::AudioBuffer<float> target;
    std::vector<HiddenInstrument::Note> notes;
    int budget = 0;
    std::function<void (int, double)> onProgress;
    std::function<void()> onFinished;
    Result result;
    std::atomic<int> triesDone { 0 };
    std::atomic<double> bestSoFar { 0.0 };
    std::atomic<bool> isFinished { false };
};

} // namespace spinus
