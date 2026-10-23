#include "ThreeOscEngine.h"

namespace spinus::dsp
{

namespace
{
/** PolyBLEP: smooths a waveform's jump at phase 0 so it does not alias (t: phase 0..1, dt: step). */
inline float polyBlep (double t, double dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return (float)(t + t - t * t - 1.0);
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return (float)(t * t + t + t + 1.0);
    }
    return 0.0f;
}

inline float oscillator (ThreeOscSettings::Shape shape, double phase, double dt, juce::Random& noise) noexcept
{
    using Shape = ThreeOscSettings::Shape;
    switch (shape)
    {
    case Shape::sine:
        return (float)std::sin (juce::MathConstants<double>::twoPi * phase);
    case Shape::triangle:
        return (float)(phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase);
    case Shape::saw:
        return (float)(2.0 * phase - 1.0) - polyBlep (phase, dt);
    case Shape::square:
    {
        auto v = phase < 0.5 ? 1.0f : -1.0f;
        v += polyBlep (phase, dt);
        v -= polyBlep (std::fmod (phase + 0.5, 1.0), dt);
        return v;
    }
    case Shape::noise:
        return noise.nextFloat() * 2.0f - 1.0f;
    }
    return 0.0f;
}
} // namespace

void ThreeOscEngine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    const juce::dsp::ProcessSpec spec { newSampleRate, (juce::uint32)juce::jmax (1, maxBlockSize), 1 };
    int seed = 1;
    for (auto& v : voices)
    {
        v.envelope.setSampleRate (newSampleRate);
        for (auto& f : v.filters)
            f.prepare (spec);
        v.noise.setSeed (seed++);
    }
    reset();
}

void ThreeOscEngine::reset()
{
    for (auto& v : voices)
    {
        v.active = false;
        v.envelope.reset();
        for (auto& f : v.filters)
            f.reset();
    }
    pitchBend = 0.0f;
    sustainPedal = false;
    monoCount = 0;
    expressions.reset();
}

void ThreeOscEngine::glideTo (Voice& v, int note, const ThreeOscSettings& s) noexcept
{
    v.note = note;
    v.target = note;
    const auto samples = (double)s.glide * sampleRate;
    if (samples < 1.0)
        v.pitch = v.target;
    v.glideStep = samples < 1.0 ? 0.0 : std::abs (v.target - v.pitch) / samples;
}

void ThreeOscEngine::startVoice (Voice& v, const juce::MidiMessage& m, const ThreeOscSettings& s) noexcept
{
    v.note = m.getNoteNumber();
    v.pitch = v.target = v.note;
    v.glideStep = 0.0;
    v.channel = m.getChannel();
    v.active = v.held = true;
    v.age = nextAge++;
    const auto velocity = m.getFloatVelocity();
    v.level = juce::jmap (juce::jlimit (0.0f, 1.0f, s.velocitySensitivity), 1.0f, velocity * velocity);
    for (size_t i = 0; i < 3; ++i)
        v.phases[i] = juce::jlimit (0.0, 0.999999, (double)s.oscillators[i].phase);
    for (auto& f : v.filters)
        f.reset();
    v.envelope.reset();
    v.envelope.setParameters ({ s.attack, s.decay, s.sustain, s.release });
    v.envelope.noteOn();
}

int ThreeOscEngine::getNumActiveVoices() const noexcept
{
    int n = 0;
    for (auto& v : voices)
        n += v.active ? 1 : 0;
    return n;
}

void ThreeOscEngine::handle (const juce::MidiMessage& m, const ThreeOscSettings& s) noexcept
{
    if (expressions.handle (m))
        return;

    if (s.mono && (m.isNoteOn() || m.isNoteOff()))
    {
        // One voice; the keys held, newest last.
        const auto note = m.getNoteNumber();
        int kept = 0;
        for (int i = 0; i < monoCount; ++i)
            if (monoHeld[(size_t)i] != note)
                monoHeld[(size_t)kept++] = monoHeld[(size_t)i];
        monoCount = kept;
        auto& v = voices[0];
        for (size_t i = 1; i < voices.size(); ++i) // from poly play just before
            if (voices[i].active)
                voices[i].envelope.noteOff();
        if (m.isNoteOn())
        {
            const bool legato = monoCount > 0 && v.active && v.held;
            if (monoCount < (int)monoHeld.size())
                monoHeld[(size_t)monoCount++] = note;
            if (legato)
                glideTo (v, note, s); // slide; the envelope goes on
            else
                startVoice (v, m, s);
        }
        else if (v.active && v.note == note)
        {
            if (monoCount > 0)
                glideTo (v, monoHeld[(size_t)monoCount - 1], s); // back to the key still held
            else
            {
                v.held = false;
                if (! sustainPedal)
                    v.envelope.noteOff();
            }
        }
        return;
    }

    if (m.isNoteOn())
    {
        Voice* target = nullptr;
        for (auto& v : voices)
            if (! v.active)
            {
                target = &v;
                break;
            }
        if (target == nullptr)
        {
            target = &voices[0];
            for (auto& v : voices)
                if (v.age < target->age)
                    target = &v;
        }

        startVoice (*target, m, s);
    }
    else if (m.isNoteOff())
    {
        for (auto& v : voices)
            if (v.active && v.held && v.note == m.getNoteNumber() && v.channel == m.getChannel())
            {
                v.held = false;
                if (! sustainPedal)
                    v.envelope.noteOff();
            }
    }
    else if (m.isSustainPedalOn())
    {
        sustainPedal = true;
    }
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (auto& v : voices)
            if (v.active && ! v.held)
                v.envelope.noteOff();
    }
    else if (m.isPitchWheel())
    {
        pitchBend = (float)(m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
            if (v.active)
                v.envelope.noteOff();
    }
}

void ThreeOscEngine::renderVoice (Voice& v, juce::AudioBuffer<float>& out, int start, int count,
                                  const ThreeOscSettings& s) noexcept
{
    const auto& expression = expressions.get (v.channel);
    const auto notePan = noteExpression::panGains (expression.pan);
    std::array<double, 3> steps {};
    std::array<std::array<float, 2>, 3> gains {};
    auto updateSteps = [&]
    {
        for (size_t i = 0; i < 3; ++i)
        {
            const auto& o = s.oscillators[i];
            const auto note = v.pitch + o.coarse + (o.fine + expression.cents) / 100.0 + pitchBend;
            steps[i] = juce::jmin (0.49, 440.0 * std::pow (2.0, (note - 69.0) / 12.0) / sampleRate);
        }
    };
    updateSteps();
    for (size_t i = 0; i < 3; ++i)
    {
        const auto& o = s.oscillators[i];
        gains[i] = { o.level * juce::jmin (1.0f, 1.0f - o.pan) * notePan[0],
                     o.level * juce::jmin (1.0f, 1.0f + o.pan) * notePan[1] };
    }

    v.envelope.setParameters ({ s.attack, s.decay, s.sustain, s.release });
    const bool filtered = s.filter != ThreeOscSettings::Filter::off;
    if (filtered)
    {
        using Type = juce::dsp::StateVariableTPTFilterType;
        const auto type = s.filter == ThreeOscSettings::Filter::lowPass    ? Type::lowpass
                          : s.filter == ThreeOscSettings::Filter::highPass ? Type::highpass
                                                                           : Type::bandpass;
        for (auto& f : v.filters)
        {
            f.setType (type);
            f.setResonance (0.5f + juce::jlimit (0.0f, 1.0f, s.resonance) * 9.5f);
        }
    }

    // From an MPE controller: pressing harder swells the note (up to +6 dB); the slide (timbre) opens or closes
    // the filter by up to two octaves.
    const auto gain = juce::Decibels::decibelsToGain (s.gainDb) * v.level * expression.volume *
                      (1.0f + expression.pressure) * 0.3f; // headroom for 3 oscillators
    const auto timbreOctaves = (expression.timbre - 0.5f) * 4.0f;
    const auto channels = juce::jmin (2, out.getNumChannels());
    const auto maxCutoff = (float)(sampleRate * 0.45);

    for (int n = 0; n < count; ++n)
    {
        // Sliding to another note (mono glide): the pitch moves at an even rate.
        if (! juce::approximatelyEqual (v.pitch, v.target))
        {
            v.pitch = v.pitch < v.target ? juce::jmin (v.target, v.pitch + v.glideStep)
                                         : juce::jmax (v.target, v.pitch - v.glideStep);
            if (n % 16 == 0 || juce::approximatelyEqual (v.pitch, v.target))
                updateSteps();
        }
        const auto env = v.envelope.getNextSample();
        if (! v.envelope.isActive())
        {
            v.active = false;
            return;
        }

        float left = 0.0f, right = 0.0f;
        for (size_t i = 0; i < 3; ++i)
        {
            if (s.oscillators[i].level <= 0.0f)
                continue;
            const auto sample = oscillator (s.oscillators[i].shape, v.phases[i], steps[i], v.noise);
            left += sample * gains[i][0];
            right += sample * gains[i][1];
            v.phases[i] += steps[i];
            if (v.phases[i] >= 1.0)
                v.phases[i] -= 1.0;
        }

        if (filtered)
        {
            // The envelope moves the cutoff; updating it every 16 samples is smooth enough and cheap.
            if (n % 16 == 0)
            {
                const auto cutoff = juce::jlimit (
                    20.0f, maxCutoff, s.cutoffHz * std::pow (2.0f, s.envelopeAmount * env * 6.0f + timbreOctaves));
                for (auto& f : v.filters)
                    f.setCutoffFrequency (cutoff);
            }
            left = v.filters[0].processSample (0, left);
            right = v.filters[1].processSample (0, right);
        }

        out.addSample (0, start + n, left * env * gain);
        if (channels > 1)
            out.addSample (1, start + n, right * env * gain);
    }
}

void ThreeOscEngine::render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi,
                             const ThreeOscSettings& settings, int startSample, int numSamples) noexcept
{
    int position = startSample;
    const int end = startSample + numSamples;
    auto renderUntil = [&] (int until)
    {
        if (until > position)
            for (auto& v : voices)
                if (v.active)
                    renderVoice (v, buffer, position, until - position, settings);
        position = until;
    };

    for (const auto metadata : midi)
    {
        renderUntil (juce::jlimit (startSample, end, metadata.samplePosition));
        handle (metadata.getMessage(), settings);
    }
    renderUntil (end);
}

} // namespace spinus::dsp
