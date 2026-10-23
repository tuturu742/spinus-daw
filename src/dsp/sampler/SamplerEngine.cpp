#include "SamplerEngine.h"

namespace spinus::dsp
{

void SamplerEngine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    const juce::dsp::ProcessSpec spec { newSampleRate, (juce::uint32)juce::jmax (1, maxBlockSize), 1 };
    for (auto& v : voices)
    {
        v.envelope.setSampleRate (newSampleRate);
        for (auto& f : v.filters)
            f.prepare (spec);
    }
    reset();
}

void SamplerEngine::reset()
{
    for (auto& v : voices)
    {
        v.zone = nullptr;
        v.envelope.reset();
        for (auto& f : v.filters)
            f.reset();
    }
    pitchBend = 0.0f;
    sustainPedal = false;
    expressions.reset();
}

int SamplerEngine::getNumActiveVoices() const noexcept
{
    int n = 0;
    for (auto& v : voices)
        n += v.isActive() ? 1 : 0;
    return n;
}

double SamplerEngine::incrementFor (const Voice& v, const SamplerSettings& s) const noexcept
{
    const auto cents = (double)s.fine + (double)expressions.get (v.channel).cents;
    const auto semitones = (double)(v.note - v.zone->rootNote) + s.coarse + cents / 100.0 + pitchBend;
    return v.zone->sampleRate / sampleRate * std::pow (2.0, semitones / 12.0);
}

void SamplerEngine::applySettings (Voice& v, const SamplerSettings& s) noexcept
{
    v.envelope.setParameters ({ s.attack, s.decay, s.sustain, s.release });
    v.increment = incrementFor (v, s);

    if (s.filter != SamplerSettings::Filter::off)
    {
        using Type = juce::dsp::StateVariableTPTFilterType;
        const auto type = s.filter == SamplerSettings::Filter::lowPass    ? Type::lowpass
                          : s.filter == SamplerSettings::Filter::highPass ? Type::highpass
                                                                          : Type::bandpass;
        const auto cutoff = juce::jlimit (20.0f, (float)(sampleRate * 0.45), s.cutoffHz);
        const auto q = 0.5f + juce::jlimit (0.0f, 1.0f, s.resonance) * 9.5f;
        for (auto& f : v.filters)
        {
            f.setType (type);
            f.setCutoffFrequency (cutoff);
            f.setResonance (q);
        }
    }
}

const SamplerSettings& SamplerEngine::settingsFor (const SampleZone& zone, Settings settings) noexcept
{
    static const SamplerSettings defaults;
    if (settings.empty())
        return defaults;
    return settings[(size_t)juce::jlimit (0, (int)settings.size() - 1, zone.settingsIndex)];
}

void SamplerEngine::startVoice (int note, int velocity, int channel, const SampleSet& set, Settings settings) noexcept
{
    const auto* zone = set.pick (note, velocity, roundRobin++);
    if (zone == nullptr)
        return;
    const auto& s = settingsFor (*zone, settings);

    // Choke groups: an open hi-hat stops when the closed one plays.
    if (zone->chokeGroup > 0)
        for (auto& v : voices)
            if (v.isActive() && v.zone != zone && v.zone->chokeGroup == zone->chokeGroup)
                v.fadeOut = v.fadeOut > 0 ? v.fadeOut : fadeSamples;

    // A free voice, or the oldest one (all 32 busy: it restarts with the new note).
    Voice* target = nullptr;
    for (auto& v : voices)
        if (! v.isActive())
        {
            target = &v;
            break;
        }

    if (target == nullptr)
    {
        Voice* oldest = &voices[0];
        for (auto& v : voices)
            if (v.age < oldest->age)
                oldest = &v;
        target = oldest;
    }

    auto& v = *target;
    v.zone = zone;
    v.note = note;
    v.channel = channel;
    v.position = (double)zone->start;
    v.held = true;
    v.fadeOut = 0;
    v.age = nextAge++;

    const auto velocityLevel = (float)velocity / 127.0f;
    v.level = juce::jmap (juce::jlimit (0.0f, 1.0f, s.velocitySensitivity), 1.0f, velocityLevel * velocityLevel);

    for (auto& f : v.filters)
        f.reset();
    v.envelope.reset();
    applySettings (v, s);
    v.envelope.noteOn();
}

void SamplerEngine::handle (const juce::MidiMessage& m, const SampleSet* set, Settings settings) noexcept
{
    if (expressions.handle (m))
        return;

    if (m.isNoteOn())
    {
        if (set != nullptr)
            startVoice (m.getNoteNumber(), m.getVelocity(), m.getChannel(), *set, settings);
    }
    else if (m.isNoteOff())
    {
        for (auto& v : voices)
            if (v.isActive() && v.note == m.getNoteNumber() && v.channel == m.getChannel() && v.held)
            {
                v.held = false;
                if (! settingsFor (*v.zone, settings).oneShot && ! sustainPedal)
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
            if (v.isActive() && ! v.held && ! settingsFor (*v.zone, settings).oneShot)
                v.envelope.noteOff();
    }
    else if (m.isPitchWheel())
    {
        pitchBend = (float)(m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f; // +-2 semitones
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
            if (v.isActive())
                v.fadeOut = v.fadeOut > 0 ? v.fadeOut : fadeSamples;
    }
}

void SamplerEngine::renderVoice (Voice& v, juce::AudioBuffer<float>& out, int start, int count,
                                 const SamplerSettings& s) noexcept
{
    const auto& audio = *v.zone->audio;
    const auto sourceChannels = audio.getNumChannels();
    const auto outChannels = juce::jmin (2, out.getNumChannels());
    const auto end = (double)v.zone->end;
    const bool loop = v.zone->loop && v.zone->loopEnd > v.zone->loopStart;
    const auto loopStart = (double)v.zone->loopStart, loopEnd = (double)v.zone->loopEnd;
    const auto& expression = expressions.get (v.channel);
    const auto gain = juce::Decibels::decibelsToGain (s.gainDb) * v.level * expression.volume;
    // Balance: the far side goes down as the pan moves away from it.
    const auto notePan = noteExpression::panGains (expression.pan);
    const float channelGain[2] = { juce::jmin (1.0f, 1.0f - s.pan) * notePan[0],
                                   juce::jmin (1.0f, 1.0f + s.pan) * notePan[1] };
    const bool filtered = s.filter != SamplerSettings::Filter::off;
    const auto length = audio.getNumSamples();

    // Cubic (Catmull-Rom) interpolation between samples.
    auto read = [&] (int channel, double pos) noexcept
    {
        const auto* data = audio.getReadPointer (juce::jmin (channel, sourceChannels - 1));
        const auto i = (int)pos;
        const auto frac = (float)(pos - i);
        auto at = [&] (int k) noexcept
        {
            if (loop && k >= (int)loopEnd)
                k -= (int)(loopEnd - loopStart);
            return k >= 0 && k < length ? data[k] : 0.0f;
        };
        const auto y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        return y1 +
               0.5f * frac *
                   (y2 - y0 + frac * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + frac * (3.0f * (y1 - y2) + y3 - y0)));
    };

    for (int n = 0; n < count; ++n)
    {
        if (loop)
        {
            while (v.position >= loopEnd)
                v.position -= loopEnd - loopStart;
        }
        else if (v.position >= end)
        {
            v.zone = nullptr;
            return;
        }

        auto env = v.envelope.getNextSample();
        if (v.fadeOut > 0)
        {
            env *= (float)v.fadeOut / (float)fadeSamples;
            if (--v.fadeOut == 0)
            {
                v.zone = nullptr;
                return;
            }
        }

        if (! v.envelope.isActive())
        {
            v.zone = nullptr;
            return;
        }

        if (v.zone->fadeOutSamples > 0 && ! loop)
            env *= juce::jmin (1.0f, (float)((end - v.position) / v.zone->fadeOutSamples));

        for (int c = 0; c < outChannels; ++c)
        {
            auto sample = read (c, v.position);
            if (filtered)
                sample = v.filters[(size_t)c].processSample (0, sample);
            out.addSample (c, start + n, sample * env * gain * (outChannels > 1 ? channelGain[c] : 1.0f));
        }

        v.position += v.increment;
    }
}

void SamplerEngine::useSet (const SampleSet* set, int maxFade) noexcept
{
    // A new set: the voices' zones belong to the old one, which the owner may free after the next
    // block. Stop them within it.
    if (set == lastSet)
        return;
    for (auto& v : voices)
        if (v.isActive())
            v.fadeOut = juce::jmin (v.fadeOut > 0 ? v.fadeOut : fadeSamples, juce::jmax (1, maxFade));
    lastSet = set;
}

void SamplerEngine::render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, const SampleSet* set,
                            Settings settings, int startSample, int numSamples) noexcept
{
    useSet (set, numSamples);

    for (auto& v : voices)
        if (v.isActive())
            applySettings (v, settingsFor (*v.zone, settings));

    int position = startSample;
    const int end = startSample + numSamples;

    auto renderUntil = [&] (int until)
    {
        if (until > position)
            for (auto& v : voices)
                if (v.isActive())
                    renderVoice (v, buffer, position, until - position, settingsFor (*v.zone, settings));
        position = until;
    };

    for (const auto metadata : midi)
    {
        const auto at = juce::jlimit (startSample, end, metadata.samplePosition);
        renderUntil (at);
        handle (metadata.getMessage(), set, settings);
    }

    renderUntil (end);
}

} // namespace spinus::dsp
