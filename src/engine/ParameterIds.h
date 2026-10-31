#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <unordered_map>
#include <vector>

namespace spinus::engine
{

/** Spinus's ID for a setting of an instrument or effect, the one everything uses:
    device.setParameters and Device::parameters, automation targets, the device menu's automation list, the
    last tweaked control, synth match and its profiles.

    It is the ID Tracktion's automation knows the parameter by (te::AutomatableParameter::paramID): a Spinus
    device's own parameter ID ("cutoff", "osc1_shape"); for any other plugin (CLAP, VST3, LV2, VST, and plugins
    running separately) the parameter's position in the plugin's list, "0", "1", "2"... That is not the
    plugin's own ID (a CLAP plugin's clap_id, a VST3 ParamID), which for many plugins (OB-Xf, Surge XT Effects)
    is some other number. Message thread. */
juce::String getParameterId (const juce::AudioProcessorParameter&);

/** The plugin's own ID for a parameter (a CLAP clap_id, a VST3 ParamID; a Spinus device's parameter ID). Only
    for reading Device::parameters saved before Spinus used getParameterId() there (see ParameterFinder). */
juce::String getPluginOwnParameterId (const juce::AudioProcessorParameter&);

/** Finds a plugin's parameters by Spinus's ID (getParameterId). Device::parameters keys saved by older
    versions (Copy effect wrote the plugin's own IDs) are found too, deterministically: a key is first read as
    Spinus's ID; only a key that is no parameter's Spinus ID is read as the plugin's own ID. Built once for an
    instance (message thread); the instance's parameter list must not change while it is used. */
class ParameterFinder
{
public:
    explicit ParameterFinder (const juce::AudioProcessor&);

    /** By Spinus's ID only. */
    juce::AudioProcessorParameter* find (const juce::String& id) const;

    /** By Spinus's ID, else (a saved older key) by the plugin's own ID; legacy says which one matched. */
    juce::AudioProcessorParameter* findSaved (const juce::String& key, bool* legacy = nullptr) const;

private:
    std::unordered_map<juce::String, juce::AudioProcessorParameter*> bySpinusId, byPluginId;
};

/** Lets setting changes made on a plugin take effect now, so its settings' names, texts and state show them:
    many plugins take in a change only when they next process audio, and an effect's type may pick which
    settings it has and what they mean. Not being played, the plugin processes a short block of silence on
    this thread; while the engine plays it, this waits briefly for the audio thread instead (CLAP plugins;
    nothing for other plugins, which keep their own ways). Message thread. */
void letChangesTakeEffect (juce::AudioProcessor&);

/** Sets several of a plugin's parameters (0 to 1) as one change: then lets the change take effect and sets again
    any the plugin put elsewhere meanwhile, as a plugin may put its settings back to their defaults when one
    of them changes (an effect's type), whatever their order. Message thread. */
void setParameterValues (juce::AudioProcessor&, const std::vector<std::pair<juce::AudioProcessorParameter*, float>>&);

/** A plugin's parameters now, 0 to 1, by Spinus's ID. */
std::map<juce::String, float> readParameterValues (const juce::AudioProcessor&);

} // namespace spinus::engine
