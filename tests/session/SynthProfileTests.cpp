#include <catch2/catch_test_macros.hpp>

#include "session/SynthMatch.h"

using namespace spinus;

namespace
{
using Parameter = SynthProfile::Parameter;

Parameter continuous (const juce::String& name)
{
    Parameter p;
    p.id = name.toLowerCase().replaceCharacter (' ', '_');
    p.name = name;
    return p;
}

Parameter stepped (const juce::String& name, int steps)
{
    auto p = continuous (name);
    p.numSteps = steps;
    p.isBoolean = steps == 2;
    return p;
}

juce::StringArray names (const SynthProfile& profile)
{
    juce::StringArray result;
    for (auto& s : profile.settings)
        result.add (s.name);
    return result;
}

const SynthProfile::Setting* findSetting (const SynthProfile& profile, const juce::String& name)
{
    for (auto& s : profile.settings)
        if (s.name == name)
            return &s;
    return nullptr;
}

model::Device installed (const juce::String& name, const juce::String& maker = "Someone",
                         const juce::String& format = "VST3")
{
    model::Device device;
    device.kind = model::Device::Kind::plugin;
    device.plugin = { format, name, maker, "/somewhere/" + name, 1 };
    return device;
}

model::Device stock (const juce::String& stockType)
{
    model::Device device;
    device.stockType = stockType;
    return device;
}
} // namespace

TEST_CASE ("Automatic synth profile: the settings that shape the sound, most important first", "[session][synthmatch]")
{
    std::vector<Parameter> parameters {
        continuous ("Master Volume"),
        stepped ("Bypass", 2),
        stepped ("Voices", 8),
        continuous ("Osc 1 Pitch"),
        continuous ("Osc 2 Fine Tune"),
        stepped ("Transpose", 25),
        continuous ("Pitch Bend Up"),
        continuous ("Glide"),
        continuous ("Unison Voices"),
        continuous ("LFO 1 Rate"),
        continuous ("LFO 1 to Filter Cutoff"),
        continuous ("Filter Env to Pitch"),
        continuous ("Velocity to Amp Env"),
        continuous ("Amp Env Attack Curve"),
        continuous ("Reverb DryWet"),
        continuous ("Delay Mix"),
        continuous ("Chorus Amount"),
        stepped ("Arp Step 3 On", 2),
        continuous ("Arpeggiator Speed"),
        stepped ("Program", 128),
        continuous ("Modwheel"),
        continuous ("Pan Voice 1"),
        continuous ("Filter1 Keyboard Follow"),
        continuous ("Filter1 Osc1 Input"),
        stepped ("Osc 1 Saw Wave", 2), // a switch
        stepped ("Osc1 Wave", 4),
        continuous ("Osc1 Volume"),
        continuous ("Osc2 Level"),
        continuous ("Noise Volume"),
        continuous ("Osc1 PulseWidth"),
        continuous ("Osc 2 Detune"),
        continuous ("Filter1 Saturation"),
        continuous ("AmpEnv Attack"),
        continuous ("AmpEnv Decay"),
        continuous ("AmpEnv Sustain"),
        continuous ("AmpEnv Release"),
        continuous ("FilterEnv Attack"),
        continuous ("FilterEnv Release"),
        continuous ("ModEnv Attack"),
        continuous ("Filter Env Amount"),
        continuous ("VCFCutoff"),
        continuous ("Filter1 Resonance"),
    };
    auto hidden = continuous ("Filter2 Frequency");
    hidden.isAutomatable = false;
    parameters.push_back (hidden);
    auto meta = continuous ("Filter3 Frequency");
    meta.isMeta = true;
    parameters.push_back (meta);

    const auto profile = SynthProfile::makeAutomatic ("Some Synth", parameters);
    REQUIRE (profile.has_value());
    const auto& p = profile.value_or (SynthProfile {});
    INFO (names (p).joinIntoString (", "));
    CHECK (p.automatic);
    CHECK (p.name == "Some Synth");
    CHECK (p.start == "default");
    CHECK (p.getTries() == SynthProfile::defaultTries);
    // Filter first, then the envelopes, then the oscillators.
    CHECK (names (p) == juce::StringArray { "VCFCutoff", "Filter1 Resonance", "Filter Env Amount", "AmpEnv Attack",
                                            "AmpEnv Decay", "AmpEnv Sustain", "AmpEnv Release", "FilterEnv Attack",
                                            "FilterEnv Release", "Osc1 Wave", "Osc1 Volume", "Osc2 Level",
                                            "Noise Volume", "Osc1 PulseWidth", "Osc 2 Detune", "Filter1 Saturation",
                                            "ModEnv Attack" });

    // A wave of four is a choice of four; envelope times and levels stay in useful ranges.
    CHECK (findSetting (p, "Osc1 Wave")->steps == 4);
    CHECK (findSetting (p, "AmpEnv Attack")->steps == 0);
    CHECK (findSetting (p, "AmpEnv Attack")->max <= 0.5f);
    CHECK (findSetting (p, "AmpEnv Release")->max < 1.0f);
    CHECK (juce::approximatelyEqual (findSetting (p, "AmpEnv Sustain")->max, 1.0f));
    CHECK (findSetting (p, "Osc1 Volume")->min > 0.0f); // the first oscillator never goes quiet
    CHECK (juce::approximatelyEqual (findSetting (p, "Osc2 Level")->min, 0.0f));
    CHECK (findSetting (p, "Noise Volume")->max < 1.0f);
    CHECK (findSetting (p, "Filter1 Resonance")->max < 1.0f);
    CHECK (findSetting (p, "VCFCutoff")->id == "vcfcutoff");
}

TEST_CASE ("Automatic synth profile: at most 20 settings, an LFO only for want of anything else, none for too few",
           "[session][synthmatch]")
{
    // Many oscillators and filters: the filters and envelopes come first, then up to 20.
    std::vector<Parameter> many;
    for (int i = 1; i <= 8; ++i)
        for (auto* what : { "Cutoff", "Resonance", "Env Amount", "Wave", "Level", "PW", "Detune" })
            many.push_back (continuous ("Osc" + juce::String (i) + " " + what));
    for (auto* stage : { "Attack", "Decay", "Sustain", "Release" })
    {
        many.push_back (continuous (juce::String ("Amp ") + stage));
        many.push_back (continuous (juce::String ("Filter ") + stage));
    }
    const auto capped = SynthProfile::makeAutomatic ("Big", many).value_or (SynthProfile {});
    INFO (names (capped).joinIntoString (", "));
    CHECK (capped.settings.size() == (size_t)SynthProfile::mostAutomaticSettings);
    CHECK (capped.settings.front().name == "Osc1 Cutoff");
    CHECK (names (capped).contains ("Amp Attack"));
    CHECK (names (capped).contains ("Filter Release"));

    // An organ: nothing to tune.
    CHECK (! SynthProfile::makeAutomatic ("Organ", { continuous ("Volume"), stepped ("Bypass", 2),
                                                     stepped ("Transpose", 25), continuous ("Reverb Mix") }));
    CHECK (! SynthProfile::makeAutomatic ("Nothing", {}));

    // An LFO's speed only when there is too little else.
    const auto withLfo = SynthProfile::makeAutomatic (
        "Small", { continuous ("Cutoff"), continuous ("Resonance"), continuous ("LFO Rate") });
    REQUIRE (withLfo.has_value());
    CHECK (names (withLfo.value_or (SynthProfile {})) == juce::StringArray { "Cutoff", "Resonance", "LFO Rate" });
    const auto withoutLfo = SynthProfile::makeAutomatic (
        "Small", { continuous ("Cutoff"), continuous ("Resonance"), continuous ("Release"), continuous ("LFO Rate") });
    CHECK (! names (withoutLfo.value_or (SynthProfile {})).contains ("LFO Rate"));
}

TEST_CASE ("Synth profiles for installed plugins and the downloaded synths", "[session][synthmatch]")
{
    // An installed plugin's profile: by its name in any case, narrowed by maker and format when given.
    const auto mine = SynthProfile::fromJson (juce::JSON::parse (
        R"({"plugin": "Some Synth", "tries": 300, "settings": [{"name": "Cutoff"}, {"name": "Attack", "max": 0.3}]})"));
    REQUIRE (mine.has_value());
    const auto& p = mine.value_or (SynthProfile {});
    CHECK (p.name == "Some Synth");
    CHECK (p.getTries() == 300);
    CHECK (p.isFor (installed ("some synth")));
    CHECK (p.isFor (installed ("Some Synth", "Anyone", "LV2")));
    CHECK (! p.isFor (installed ("Some Other Synth")));
    CHECK (! p.isFor (stock ("spinus.3x-osc")));
    const auto narrow = SynthProfile::fromJson (juce::JSON::parse (
        R"({"plugin": "Some Synth", "manufacturer": "Someone", "format": "VST3", "settings": [{"name": "Cutoff"}]})"));
    REQUIRE (narrow.has_value());
    CHECK (narrow.value_or (SynthProfile {}).isFor (installed ("Some Synth", "someone", "VST3")));
    CHECK (! narrow.value_or (SynthProfile {}).isFor (installed ("Some Synth", "Someone else", "VST3")));
    CHECK (! narrow.value_or (SynthProfile {}).isFor (installed ("Some Synth", "Someone", "CLAP")));
    // Neither a synth of Spinus's nor a plugin: not a profile.
    CHECK (! SynthProfile::fromJson (juce::JSON::parse (R"({"name": "?", "settings": [{"name": "Cutoff"}]})")));

    // A downloaded synth, also by the plugin's own name.
    const auto byName = SynthProfile::fromJson (
        juce::JSON::parse (R"({"plugin": "odin2", "settings": [{"name": "Filter1 Frequency"}]})"));
    CHECK (byName.value_or (SynthProfile {}).isFor (stock ("bundled.odin2")));
    CHECK (! byName.value_or (SynthProfile {}).isFor (stock ("bundled.ob-xf")));

    // Spinus's own profiles: tried and measured, so not experimental. Odin 2's also fits an installed Odin 2.
    for (auto* type : { "spinus.3x-osc", "bundled.surge-xt", "bundled.odin2", "bundled.ob-xf" })
    {
        INFO (type);
        CHECK (SynthProfile::isTested (stock (type)));
        const auto profile = SynthProfile::find (type);
        REQUIRE (profile.has_value());
        CHECK (profile.value_or (SynthProfile {}).settings.size() >= 15);
        CHECK (profile.value_or (SynthProfile {}).settings.size() <= 20);
    }
    CHECK (SynthProfile::find ("bundled.odin2").value_or (SynthProfile {}).start == "default");
    CHECK (SynthProfile::find ("bundled.ob-xf").value_or (SynthProfile {}).start == "default");
    CHECK (SynthProfile::find ("bundled.surge-xt").value_or (SynthProfile {}).getTries() == 800);
    CHECK (SynthProfile::find ("spinus.3x-osc").value_or (SynthProfile {}).getTries() == 500);
    CHECK (SynthProfile::find (installed ("Odin2", "TheWaveWarden", "CLAP")).value_or (SynthProfile {}).name ==
           "Odin 2");
    CHECK (! SynthProfile::isTested (installed ("Odin2", "TheWaveWarden", "CLAP")));
    CHECK (! SynthProfile::isTested (installed ("Some Synth")));
    CHECK (! SynthProfile::isTested (stock ("bundled.lsp.eq")));
    CHECK (! SynthProfile::find (installed ("Some Synth")));
}
