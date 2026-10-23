#pragma once

#include <juce_dsp/juce_dsp.h>

namespace spinus::dsp
{

/** Finds the pitch of a voice or a single instrument: the YIN method (de Cheveigné and Kawahara, 2002)
    with its difference function computed through an FFT.

    prepare() allocates; analyse() does not, so it can run on the audio thread.
*/
class PitchDetector
{
public:
    struct Result
    {
        float period = 0.0f;  // in samples; 0 when no pitch was found (silence, noise, consonants)
        float clarity = 0.0f; // 0..1: how periodic the frame is (1 - YIN's normalised difference)

        bool isVoiced() const noexcept { return period > 0.0f; }
    };

    /** Pitches from minHz to maxHz can be found. */
    void prepare (double sampleRate, float minHz, float maxHz);

    /** The number of samples analyse() reads: the longest period twice. */
    int getFrameLength() const noexcept { return windowSize + maxLag; }

    /** Analyses the frame (getFrameLength() samples, oldest first). */
    Result analyse (const float* frame) noexcept;

    /** YIN's threshold: a frame is voiced when its normalised difference dips below this. */
    static constexpr float threshold = 0.15f;

private:
    double sampleRate = 48000.0;
    int windowSize = 0, minLag = 0, maxLag = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> windowSpectrum, frameSpectrum, difference;
};

} // namespace spinus::dsp
