#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "EqEngine.h"

namespace spinus::dsp
{

class EqProcessor;
class ParameterKnob;

/** The EQ's window: the curve over a live spectrum of the output, with a numbered point per band to drag
    (left and right: frequency; up and down: gain; the mouse wheel: Q; double-click: on or off;
    right-click: the band's type), and the selected band's controls below. */
class EqEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit EqEditor (EqProcessor&);
    ~EqEditor() override;

    int getSelectedBand() const noexcept { return selected; }
    void selectBand (int band);

    /** Where a band's point is drawn, and the frequency and gain at a point of the display. */
    juce::Point<float> getBandPosition (int band) const;
    float frequencyAt (float x) const;
    float gainAt (float y) const;
    juce::Rectangle<int> getDisplayArea() const;

    /** Drags a band's point to a place on the display, as the mouse does (turns the band on). */
    void dragBandTo (int band, juce::Point<float>);
    /** The band under a point of the display, or -1. */
    int bandAt (juce::Point<float>) const;

    /** Feeds the spectrum as if this audio had been played (tests and screenshots). */
    void showSpectrumOf (const float* samples, int count);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    static constexpr int fftOrder = 12, fftSize = 4096, spectrumPoints = 240;

    void timerCallback() override;
    void attachToBand();
    void addSamples (const float*, int);
    void updateSpectrum();
    void setBandValue (int band, const char* part, float value);
    void beginGesture (int band);
    void endGesture();
    void showTypeMenu (int band);
    float xFor (float hz) const;
    float yFor (float db) const;

    EqProcessor& processor;
    int selected = 2, dragging = -1, hovered = -1;

    std::vector<std::unique_ptr<juce::TextButton>> bandButtons;
    juce::ToggleButton onButton { "On" };
    juce::ComboBox type, slope;
    std::unique_ptr<ParameterKnob> frequency, gain, q, output;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment, slopeAttachment;

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t)fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> recent = std::vector<float> ((size_t)fftSize),
                       fftData = std::vector<float> ((size_t)fftSize * 2),
                       incoming = std::vector<float> ((size_t)EqEngine::spectrumFifoSize);
    std::array<float, spectrumPoints> spectrumDb {};
    int recentWrite = 0;
    bool hasSpectrum = false;
    EqSettings shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqEditor)
};

} // namespace spinus::dsp
