#include "OutputMix.h"

#include <algorithm>
#include <iterator>

namespace spinus::engine::outputmix
{

namespace
{
Side sideOf (juce::AudioChannelSet::ChannelType type)
{
    using T = juce::AudioChannelSet::ChannelType;
    static constexpr T lefts[] = {
        T::left,     T::leftSurround, T::leftCentre,  T::leftSurroundSide, T::leftSurroundRear,
        T::wideLeft, T::topFrontLeft, T::topRearLeft, T::topSideLeft
    };
    static constexpr T rights[] = {
        T::right,     T::rightSurround, T::rightCentre,  T::rightSurroundSide, T::rightSurroundRear,
        T::wideRight, T::topFrontRight, T::topRearRight, T::topSideRight
    };
    if (std::find (std::begin (lefts), std::end (lefts), type) != std::end (lefts))
        return Side::left;
    if (std::find (std::begin (rights), std::end (rights), type) != std::end (rights))
        return Side::right;
    return Side::centre;
}
} // namespace

std::vector<Side> sidesOf (const juce::AudioChannelSet& mainOutput)
{
    if (mainOutput.size() <= 2)
        return {};
    std::vector<Side> sides;
    sides.reserve ((size_t)mainOutput.size());
    for (int c = 0; c < mainOutput.size(); ++c)
        sides.push_back (sideOf (mainOutput.getTypeOfChannel (c)));
    return sides;
}

void mixToStereo (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Side* sides, int numSides) noexcept
{
    const auto channels = juce::jmin (numSides, buffer.getNumChannels());
    if (numSides <= 0 || buffer.getNumChannels() < 2 || numSamples <= 0)
        return;

    auto* left = buffer.getWritePointer (0, start);
    auto* right = buffer.getWritePointer (1, start);
    for (int i = 0; i < numSamples; ++i)
    {
        // Each sample's sums are made from every channel before channels 0 and 1 are written.
        float l = 0.0f, r = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const auto value = buffer.getReadPointer (c, start)[i];
            switch (sides[c])
            {
            case Side::left:
                l += value;
                break;
            case Side::right:
                r += value;
                break;
            case Side::centre:
                l += value * centreGain;
                r += value * centreGain;
                break;
            }
        }
        left[i] = l;
        right[i] = r;
    }
    for (int c = 2; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, start, numSamples);
}

} // namespace spinus::engine::outputmix
