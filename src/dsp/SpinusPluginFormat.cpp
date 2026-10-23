#include "SpinusPluginFormat.h"

#include "drums/DrumMachineProcessor.h"
#include "ducker/DuckerProcessor.h"
#include "dynamics/DynamicsProcessors.h"
#include "eq/EqProcessor.h"
#include "filter/FilterProcessor.h"
#include "sampler/SamplerProcessor.h"
#include "slicer/BeatSlicerProcessor.h"
#include "threeosc/ThreeOscProcessor.h"
#include "tune/TuneProcessor.h"
#include "loudness/LoudnessMeterProcessor.h"
#include "utility/StereoUtilityProcessor.h"

namespace spinus::dsp
{

std::unique_ptr<juce::AudioPluginInstance> SpinusPluginFormat::create (const juce::String& identifier)
{
    if (identifier == SamplerProcessor::identifier)
        return std::make_unique<SamplerProcessor>();
    if (identifier == DrumMachineProcessor::identifier)
        return std::make_unique<DrumMachineProcessor>();
    if (identifier == ThreeOscProcessor::identifier)
        return std::make_unique<ThreeOscProcessor>();
    if (identifier == FilterProcessor::identifier)
        return std::make_unique<FilterProcessor>();
    if (identifier == DuckerProcessor::identifier)
        return std::make_unique<DuckerProcessor>();
    if (identifier == StereoUtilityProcessor::identifier)
        return std::make_unique<StereoUtilityProcessor>();
    if (identifier == BeatSlicerProcessor::identifier)
        return std::make_unique<BeatSlicerProcessor>();
    if (identifier == TuneProcessor::identifier)
        return std::make_unique<TuneProcessor>();
    if (identifier == LoudnessMeterProcessor::identifier)
        return std::make_unique<LoudnessMeterProcessor>();
    if (identifier == EqProcessor::identifier)
        return std::make_unique<EqProcessor>();
    if (identifier == CompressorProcessor::identifier)
        return std::make_unique<CompressorProcessor>();
    if (identifier == LimiterProcessor::identifier)
        return std::make_unique<LimiterProcessor>();
    if (identifier == GateProcessor::identifier)
        return std::make_unique<GateProcessor>();
    return nullptr;
}

juce::Array<juce::PluginDescription> SpinusPluginFormat::getDescriptions()
{
    // Described once: making an instance just to describe it is not free.
    static const auto descriptions = []
    {
        juce::Array<juce::PluginDescription> result;
        for (auto* id : { SamplerProcessor::identifier, DrumMachineProcessor::identifier, ThreeOscProcessor::identifier,
                          FilterProcessor::identifier, DuckerProcessor::identifier, StereoUtilityProcessor::identifier,
                          BeatSlicerProcessor::identifier, TuneProcessor::identifier,
                          LoudnessMeterProcessor::identifier, EqProcessor::identifier, CompressorProcessor::identifier,
                          LimiterProcessor::identifier, GateProcessor::identifier })
            if (auto instance = create (id))
                result.add (instance->getPluginDescription());
        return result;
    }();
    return descriptions;
}

std::optional<juce::PluginDescription> SpinusPluginFormat::findDescription (const juce::String& identifier)
{
    for (auto& d : getDescriptions())
        if (d.fileOrIdentifier == identifier)
            return d;
    return std::nullopt;
}

void SpinusPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                                              const juce::String& identifier)
{
    if (auto d = findDescription (identifier))
        results.add (new juce::PluginDescription (*d));
}

juce::String SpinusPluginFormat::getNameOfPluginFromIdentifier (const juce::String& identifier)
{
    auto d = findDescription (identifier);
    return d ? d->name : identifier;
}

void SpinusPluginFormat::createPluginInstance (const juce::PluginDescription& description, double sampleRate,
                                               int bufferSize, PluginCreationCallback callback)
{
    auto instance = create (description.fileOrIdentifier);
    if (instance == nullptr)
    {
        callback (nullptr, "Unknown Spinus device: " + description.fileOrIdentifier);
        return;
    }
    instance->setRateAndBufferSizeDetails (sampleRate, bufferSize);
    callback (std::move (instance), {});
}

} // namespace spinus::dsp
