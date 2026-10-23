// The standalone "Spinus Loudness Meter" plugin (VST3, LV2): the same processor Spinus hosts
// internally, for pluginval and for use in other DAWs.

#include "LoudnessMeterProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new spinus::dsp::LoudnessMeterProcessor(); }
