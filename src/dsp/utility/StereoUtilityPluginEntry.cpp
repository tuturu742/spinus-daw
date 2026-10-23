// The standalone "Spinus Stereo Utility" plugin (VST3, LV2): the same processor Spinus hosts
// internally, for pluginval and for use in other DAWs.

#include "StereoUtilityProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new spinus::dsp::StereoUtilityProcessor(); }
