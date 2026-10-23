#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

namespace spinus::dsp
{

class CompressorProcessor;
class LimiterProcessor;
class GateProcessor;
class ParameterKnob;

/** The last few seconds of a reading (30 a second), for a scrolling graph. */
class ReadingHistory
{
public:
    static constexpr int length = 150; // 5 s

    void push (float value) noexcept;
    float get (int age) const noexcept { return values[(size_t)((write - 1 - age + 2 * length) % length)]; }
    void fill (float value) noexcept { values.fill (value); }

private:
    std::array<float, length> values {};
    int write = 0;
};

/** What the three dynamics windows share: a title, a row of knobs, the choices beside them, a status line,
    and a timer that reads the engine 30 times a second. */
class DynamicsEditorBase : public juce::AudioProcessorEditor, protected juce::Timer
{
public:
    ~DynamicsEditorBase() override;

    /** The status line (tests). */
    juce::String getStatusText() const { return status; }

    void resized() override;

protected:
    DynamicsEditorBase (juce::AudioProcessor&, juce::AudioProcessorValueTreeState&, const juce::String& title);

    void addKnob (const char* id, const char* label);
    void addChoice (const char* id);
    void addToggle (const char* id, const char* label);
    void setStatus (const juce::String&);
    void paintFrame (juce::Graphics&);

    /** The area above the controls for the device's own display. */
    juce::Rectangle<int> display;
    juce::Rectangle<int> statusArea;
    juce::String status;

    static void drawReductionMeter (juce::Graphics&, juce::Rectangle<float>, float reductionDb, float rangeDb);
    static void drawHistory (juce::Graphics&, juce::Rectangle<float>, const ReadingHistory&, float bottomDb,
                             juce::Colour, bool fillFromTop);

private:
    juce::AudioProcessorValueTreeState& state;
    juce::String title;
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
    std::vector<std::unique_ptr<juce::ComboBox>> choices;
    std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> choiceAttachments;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> toggleAttachments;
};

/** The Compressor's window: the transfer curve with the level now on it, the gain reduction over the last
    seconds and now, the knobs, and whether sound arrives on the sidechain. */
class CompressorEditor final : public DynamicsEditorBase
{
public:
    explicit CompressorEditor (CompressorProcessor&);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    CompressorProcessor& processor;
    ReadingHistory reduction;
    float shownReduction = 0.0f, shownLevel = -120.0f;
    juce::uint32 lastSidechainSound = 0;
};

/** The Limiter's window: the gain reduction over the last seconds and now, the output peak against the
    ceiling, and the knobs; it says the latency the look-ahead adds. */
class LimiterEditor final : public DynamicsEditorBase
{
public:
    explicit LimiterEditor (LimiterProcessor&);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    LimiterProcessor& processor;
    ReadingHistory reduction;
    float shownReduction = 0.0f, shownOutput = -120.0f;
};

/** The Gate's window: the level over the last seconds against the open and close thresholds, the gate's
    gain, whether it is open, holding or closed, and whether sound arrives on the sidechain. */
class GateEditor final : public DynamicsEditorBase
{
public:
    explicit GateEditor (GateProcessor&);
    void paint (juce::Graphics&) override;

    /** "Open", "Holding", "Closed" ... (tests). */
    juce::String getStateText() const;

private:
    void timerCallback() override;

    GateProcessor& processor;
    ReadingHistory level, gain;
    juce::uint32 lastSidechainSound = 0;
};

} // namespace spinus::dsp
