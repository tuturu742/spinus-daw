#include "DelayProcessor.h"

#include "DelayEditor.h"
#include "dsp/OwnEffectParameters.h"

namespace spinus::dsp
{

namespace
{
juce::String hertz (float v)
{
    return v < 999.5f ? juce::String (juce::roundToInt (v)) + " Hz" : juce::String (v / 1000.0f, 1) + " kHz";
}
juce::String lowCutText (float v, int) { return v <= DelaySettings::lowCutOff ? juce::String ("Off") : hertz (v); }
juce::String highCutText (float v, int) { return v >= DelaySettings::highCutOff ? juce::String ("Off") : hertz (v); }
juce::String timeKnobText (float v, int) { return DelayProcessor::timeText (v); }
} // namespace

juce::String DelayProcessor::timeText (double ms)
{
    if (ms < 10.0)
        return juce::String (ms, 2) + " ms";
    if (ms < 100.0)
        return juce::String (ms, 1) + " ms";
    if (ms < 999.5)
        return juce::String (juce::roundToInt (ms)) + " ms";
    return juce::String (ms / 1000.0, 2) + " s";
}

juce::StringArray DelayProcessor::getNoteNames()
{
    return { "1/32",        "1/16 triplet", "1/16",       "1/16 dotted", "1/8 triplet", "1/8",        "1/8 dotted",
             "1/4 triplet", "1/4",          "1/4 dotted", "1/2 triplet", "1/2",         "1/2 dotted", "1 bar" };
}

DelayProcessor::DelayProcessor()
    : OwnEffect (pluginName, "SpinusDelay", false,
                 []
                 {
                     using namespace param;
                     juce::AudioProcessorValueTreeState::ParameterLayout layout;
                     layout.add (choiceParameter ("sync", "Sync", { "Free", "Tempo" }, 1),
                                 choiceParameter ("note", "Note", getNoteNames(), defaultNote),
                                 floatParameter ("time", "Time", logRange (1.0f, 2000.0f), 350.0f, timeKnobText),
                                 floatParameter ("feedback", "Feedback", { 0.0f, 0.95f }, 0.35f, percent),
                                 choiceParameter ("mode", "Mode", { "Stereo", "Ping-pong", "Mono" }, 0),
                                 floatParameter ("low_cut", "Low cut", logRange (DelaySettings::lowCutOff, 2000.0f),
                                                 80.0f, lowCutText),
                                 floatParameter ("high_cut", "High cut", logRange (1000.0f, DelaySettings::highCutOff),
                                                 8000.0f, highCutText),
                                 floatParameter ("width", "Width", { 0.0f, 1.0f }, 1.0f, percent),
                                 floatParameter ("mix", "Mix", { 0.0f, 1.0f }, 0.3f, percent));
                     return layout;
                 }())
{
    auto get = [this] (const char* id) { return parameters.getRawParameterValue (id); };
    sync = get ("sync");
    note = get ("note");
    time = get ("time");
    feedback = get ("feedback");
    mode = get ("mode");
    lowCut = get ("low_cut");
    highCut = get ("high_cut");
    width = get ("width");
    mix = get ("mix");
}

DelaySettings DelayProcessor::getSettings() const noexcept
{
    DelaySettings s;
    s.sync = sync->load() > 0.5f;
    s.beats = noteBeats[(size_t)juce::jlimit (0, (int)noteBeats.size() - 1, juce::roundToInt (note->load()))];
    s.timeMs = time->load();
    s.feedback = feedback->load();
    s.mode = (DelaySettings::Mode)juce::jlimit (0, 2, juce::roundToInt (mode->load()));
    s.lowCutHz = lowCut->load();
    s.highCutHz = highCut->load();
    s.width = width->load();
    s.mix = mix->load();
    return s;
}

double DelayProcessor::getTailLengthSeconds() const
{
    const auto s = getSettings();
    // At the slowest usual tempo when synced: the host may ask before it plays.
    const auto seconds = DelayEngine::timeMsFor (s, s.sync ? 60.0 : 120.0) / 1000.0;
    const auto repeats = s.feedback > 0.001f ? std::log (0.001) / std::log ((double)s.feedback) : 0.0;
    return juce::jmin (30.0, seconds * (1.0 + repeats));
}

void DelayProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    describe (d, identifier, "A tempo-synced delay: stereo, ping-pong or mono, with feedback filters");
}

void DelayProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void DelayProcessor::releaseResources() { engine.reset(); }

void DelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (auto c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    double bpm = 120.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            bpm = position->getBpm().orFallback (120.0);

    auto main = getBusBuffer (buffer, false, 0);
    engine.process (main, getSettings(), bpm);
}

juce::AudioProcessorEditor* DelayProcessor::createEditor() { return new DelayEditor (*this); }

} // namespace spinus::dsp
