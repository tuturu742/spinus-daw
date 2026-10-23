#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/ParameterText.h"
#include "dsp/SpinusPluginFormat.h"
#include "engine/ParameterIds.h"

using namespace spinus;
using namespace spinus::dsp;

namespace
{
/** A Spinus device's setting by its parameter ID, and what typed text sets it to, in its own units. */
struct Device
{
    explicit Device (const juce::String& identifier) : instance (SpinusPluginFormat::create (identifier))
    {
        REQUIRE (instance != nullptr);
    }

    juce::RangedAudioParameter& parameter (const juce::String& id) const
    {
        for (auto* p : instance->getParameters())
            if (engine::getParameterId (*p) == id)
                if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                    return *ranged;
        FAIL ("no parameter " << id);
        throw std::logic_error ("unreachable");
    }

    /** The value (in the setting's own range) typed text gives, or NaN when it is not read. */
    float typed (const juce::String& id, const juce::String& text) const
    {
        auto& p = parameter (id);
        const auto v = param::normalisedForText (p, text);
        return v ? p.convertFrom0to1 (*v) : std::numeric_limits<float>::quiet_NaN();
    }

    std::unique_ptr<juce::AudioPluginInstance> instance;
};

bool unread (float v) { return std::isnan (v); }
} // namespace

TEST_CASE ("Every Spinus number setting reads back the text it shows", "[dsp][parameters]")
{
    for (auto& description : SpinusPluginFormat::getDescriptions())
    {
        auto instance = SpinusPluginFormat::create (description.fileOrIdentifier);
        REQUIRE (instance != nullptr);
        for (auto* p : instance->getParameters())
        {
            INFO (description.name << ": " << p->getName (100));
            if (dynamic_cast<juce::AudioParameterFloat*> (p) != nullptr)
                CHECK (dynamic_cast<param::FloatSetting*> (p) != nullptr); // reads typed text in its own units
            for (int k = 0; k <= 20; ++k)
            {
                const auto text = p->getText ((float)k / 20.0f, 200);
                INFO (text);
                const auto v = param::normalisedForText (*p, text);
                REQUIRE (v.has_value());
                CHECK (p->getText (*v, 200) == text);
                CHECK (p->getText (p->getValueForText (text), 200) == text); // a knob's text box: the same
            }
        }
    }
}

TEST_CASE ("Typed settings are in the units the device shows", "[dsp][parameters]")
{
    SECTION ("Sidechain Ducker: percent and milliseconds")
    {
        const Device ducker ("spinus:ducker");
        CHECK (ducker.typed ("amount", "35") == Catch::Approx (0.35f));
        CHECK (ducker.typed ("amount", "35%") == Catch::Approx (0.35f));
        CHECK (ducker.typed ("amount", "35 %") == Catch::Approx (0.35f));
        CHECK (ducker.typed ("release", "220") == Catch::Approx (0.22f)); // stored in seconds, shown in ms
        CHECK (ducker.typed ("release", "220 ms") == Catch::Approx (0.22f));
        CHECK (ducker.typed ("release", "0.22 s") == Catch::Approx (0.22f));
        CHECK (ducker.typed ("release", "5 s") == Catch::Approx (1.0f)); // its longest
        CHECK (ducker.typed ("trigger", "every beat") == Catch::Approx (1.0f));
        CHECK (unread (ducker.typed ("amount", "-6 dB")));
        CHECK (unread (ducker.typed ("release", "50%")));
        CHECK (unread (ducker.typed ("amount", "loud")));
    }
    SECTION ("Stereo Utility: a width up to 200%, pan sides, decibels")
    {
        const Device utility ("spinus:stereo-utility");
        CHECK (utility.typed ("width", "100%") == Catch::Approx (1.0f));
        CHECK (utility.typed ("width", "150") == Catch::Approx (1.5f));
        CHECK (utility.typed ("width", "190 %") == Catch::Approx (1.9f));
        CHECK (utility.typed ("pan", "30% L") == Catch::Approx (-0.3f));
        CHECK (utility.typed ("pan", "30 R") == Catch::Approx (0.3f));
        CHECK (utility.typed ("pan", "-30") == Catch::Approx (-0.3f));
        CHECK (utility.typed ("pan", "Centre") == Catch::Approx (0.0f));
        CHECK (utility.typed ("gain", "-6 dB") == Catch::Approx (-6.0f));
        CHECK (utility.typed ("gain", "-6") == Catch::Approx (-6.0f));
        CHECK (utility.typed ("mono", "on") == Catch::Approx (1.0f));
    }
    SECTION ("Filter: hertz and kilohertz, percent, choices by name")
    {
        const Device filter ("spinus:filter");
        CHECK (filter.typed ("resonance", "35") == Catch::Approx (0.35f));
        CHECK (filter.typed ("resonance", "35%") == Catch::Approx (0.35f));
        CHECK (filter.typed ("cutoff", "1.2 kHz") == Catch::Approx (1200.0f));
        CHECK (filter.typed ("cutoff", "1.2kHz") == Catch::Approx (1200.0f));
        CHECK (filter.typed ("cutoff", "1200") == Catch::Approx (1200.0f));
        CHECK (filter.typed ("cutoff", "1200 Hz") == Catch::Approx (1200.0f));
        CHECK (filter.typed ("type", "high pass") == Catch::Approx (1.0f));
        CHECK (filter.typed ("slope", "24 dB") == Catch::Approx (1.0f));
        CHECK (filter.typed ("env_attack", "50") == Catch::Approx (0.05f)); // stored in seconds
        CHECK (unread (filter.typed ("cutoff", "50%")));
    }
    SECTION ("3x Osc: seconds, semitones, cents, degrees")
    {
        const Device osc ("spinus:3x-osc");
        CHECK (osc.typed ("resonance", "45") == Catch::Approx (0.45f));
        CHECK (osc.typed ("attack", "1.5 s") == Catch::Approx (1.5f));
        CHECK (osc.typed ("attack", "1500") == Catch::Approx (1.5f));
        CHECK (osc.typed ("attack", "250 ms") == Catch::Approx (0.25f));
        CHECK (osc.typed ("osc1_coarse", "-12 st") == Catch::Approx (-12.0f));
        CHECK (osc.typed ("osc1_coarse", "-12") == Catch::Approx (-12.0f));
        CHECK (osc.typed ("osc1_fine", "7 ct") == Catch::Approx (7.0f));
        CHECK (osc.typed ("osc1_phase", "90") == Catch::Approx (0.25f));
        CHECK (osc.typed ("cutoff", "2.5 kHz") == Catch::Approx (2500.0f));
        CHECK (osc.typed ("gain", "-3 dB") == Catch::Approx (-3.0f));
    }
    SECTION ("Compressor and Limiter: ratios, milliseconds stored as milliseconds, decibels")
    {
        const Device compressor ("spinus:compressor");
        CHECK (compressor.typed ("ratio", "4:1") == Catch::Approx (4.0f));
        CHECK (compressor.typed ("ratio", "4") == Catch::Approx (4.0f));
        CHECK (compressor.typed ("attack", "15") == Catch::Approx (15.0f));
        CHECK (compressor.typed ("attack", "15 ms") == Catch::Approx (15.0f));
        CHECK (compressor.typed ("release", "1.2 s") == Catch::Approx (1200.0f));
        CHECK (compressor.typed ("mix", "50%") == Catch::Approx (0.5f));
        CHECK (compressor.typed ("mix", "50") == Catch::Approx (0.5f));
        CHECK (compressor.typed ("threshold", "-14") == Catch::Approx (-14.0f));
        CHECK (compressor.typed ("threshold", "-14 dB") == Catch::Approx (-14.0f));
        CHECK (unread (compressor.typed ("threshold", "50%")));
        CHECK (unread (compressor.typed ("threshold", "200 Hz")));

        const Device limiter ("spinus:limiter");
        CHECK (limiter.typed ("ceiling", "-1.2 dB") == Catch::Approx (-1.2f));
        CHECK (limiter.typed ("release", "60") == Catch::Approx (60.0f));
        CHECK (limiter.typed ("lookahead", "3 ms") == Catch::Approx (3.0f));
        CHECK (limiter.typed ("true_peak", "Off") == Catch::Approx (0.0f));
    }
    SECTION ("Spinus Tune, EQ, Sampler and Drum Machine")
    {
        const Device tune ("spinus:tune");
        CHECK (tune.typed ("retune", "Robot") == Catch::Approx (0.0f));
        CHECK (tune.typed ("retune", "60") == Catch::Approx (60.0f));
        CHECK (tune.typed ("key", "F#") == Catch::Approx (6.0f));

        const Device eq ("spinus:eq");
        CHECK (eq.typed ("band1_q", "0.7") == Catch::Approx (0.7f));
        CHECK (eq.typed ("band1_freq", "2.5 kHz") == Catch::Approx (2500.0f));
        CHECK (eq.typed ("band1_gain", "+3 dB") == Catch::Approx (3.0f));

        const Device sampler ("spinus:sampler");
        CHECK (sampler.typed ("release", "300") == Catch::Approx (0.3f));
        CHECK (sampler.typed ("release", "0.3 s") == Catch::Approx (0.3f));
        CHECK (sampler.typed ("sustain", "80") == Catch::Approx (0.8f));

        const Device drums ("spinus:drum-machine");
        CHECK (drums.typed ("pad1_pan", "20% L") == Catch::Approx (-0.2f));
        CHECK (drums.typed ("pad1_decay", "2 s") == Catch::Approx (2.0f));
    }
}

TEST_CASE ("A knob's text box ignores text it cannot read", "[dsp][parameters]")
{
    const Device ducker ("spinus:ducker");
    auto& amount = ducker.parameter ("amount");
    amount.setValueNotifyingHost (0.4f);
    CHECK (amount.getValueForText ("-6 dB") == Catch::Approx (0.4f));
    CHECK (amount.getValueForText ("") == Catch::Approx (0.4f));
    CHECK (amount.getValueForText ("60") == Catch::Approx (0.6f));
}

TEST_CASE ("Typed units: what reads and what agrees", "[dsp][parameters]")
{
    using param::UnitKind;
    auto kind = [] (const char* text)
    { return param::read (text).value_or (param::Reading { 0.0, UnitKind::unknown }); };
    CHECK (kind ("1.2 kHz").value == Catch::Approx (1200.0));
    CHECK (kind ("1.2 kHz").kind == UnitKind::frequency);
    CHECK (kind ("1.5 s").value == Catch::Approx (1500.0));
    CHECK (kind ("1.5 s").kind == UnitKind::time);
    CHECK (kind ("-6 dB").value == Catch::Approx (-6.0));
    CHECK (kind ("4:1").kind == UnitKind::ratio);
    CHECK (kind ("30% L").value == Catch::Approx (-30.0));
    CHECK (param::read (juce::String (juce::CharPointer_UTF8 ("90\xc2\xb0")))->kind == UnitKind::degrees);
    CHECK (kind ("12").kind == UnitKind::none);
    CHECK (! param::read ("Centre").has_value());
    CHECK (param::unitsAgree ("250 ms", "1.20 s"));
    CHECK (param::unitsAgree ("800 Hz", "1.20 kHz"));
    CHECK (param::unitsAgree ("12", "-6.0 dB"));
    CHECK (! param::unitsAgree ("50%", "-6.0 dB"));
    CHECK (param::unitsAgree ("50%", "Centre"));
}
