#include "LoudnessMeterEditor.h"

#include "LoudnessMeterProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
constexpr float graphTop = 0.0f, graphBottom = -40.0f; // LUFS

juce::String formatReading (float value)
{
    return value <= LoudnessEngine::silence ? juce::String::fromUTF8 ("–") : juce::String (value, 1);
}
} // namespace

LoudnessMeterEditor::LoudnessMeterEditor (LoudnessMeterProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    auto& state = p.getParameterState();
    int id = 1;
    for (auto& t : LoudnessMeterProcessor::getTargets())
        target.addItem (t.name, id++);
    target.setTooltip ("What the song is aimed at: the loudness it should have, and the highest peak");
    targetAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "target", target);
    addAndMakeVisible (target);

    resetOnPlay.setColour (juce::ToggleButton::textColourId, look::text);
    resetOnPlay.setColour (juce::ToggleButton::tickColourId, look::accent);
    resetOnPlay.setTooltip ("Measure the song again from where playback starts, each time you press Play");
    resetOnPlayAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, "reset_on_play", resetOnPlay);
    addAndMakeVisible (resetOnPlay);

    reset.setTooltip ("Start measuring again now");
    reset.onClick = [this] { processor.getEngine().requestReset(); };
    addAndMakeVisible (reset);

    setResizable (true, true);
    setResizeLimits (480, 320, 1000, 700);
    setSize (560, 360);
    startTimerHz (15);
}

LoudnessMeterEditor::~LoudnessMeterEditor() = default;

void LoudnessMeterEditor::timerCallback()
{
    const auto& engine = processor.getEngine();
    momentary = engine.getMomentary();
    shortTerm = engine.getShortTerm();
    integrated = engine.getIntegrated();
    truePeak = engine.getTruePeakDb();
    seconds = engine.getMeasuredSeconds();
    historyCount = engine.copyHistory (history);
    repaint();
}

juce::String LoudnessMeterEditor::getVerdict() const
{
    const auto& t = processor.getTarget();
    if (seconds < 3.0 || integrated <= LoudnessEngine::silence)
        return "Play the song (or its loudest part) to measure it.";

    juce::String text;
    const auto difference = integrated - t.lufs;
    if (difference > 1.0f)
        text << juce::String (difference, 1) << " dB louder than the target. "
             << (t.lufs <= -14.0f ? "Services will turn it down by that much, so it won't sound louder, only more "
                                    "squashed. You can turn the master down or ease off the limiter."
                                  : "Turn the master down or ease off the limiter.");
    else if (difference < -1.0f)
        text << juce::String (-difference, 1) << " dB quieter than the target: it will play quieter than other "
             << "songs. Raise the level into a limiter on the Master.";
    else
        text << "On target.";

    if (truePeak > t.peakDbTp)
        text << " The true peak is above " << juce::String (t.peakDbTp, 1)
             << " dBTP: it may distort once converted (MP3, AAC). Lower the limiter's ceiling.";
    return text;
}

void LoudnessMeterEditor::drawReading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title,
                                       float value, const juce::String& unit, bool big, juce::Colour colour) const
{
    g.setColour (look::panel);
    g.fillRoundedRectangle (area.toFloat().reduced (3.0f), 5.0f);
    area = area.reduced (10, 6);
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (title, area.removeFromTop (14), juce::Justification::centredLeft);
    g.setColour (colour);
    g.setFont (juce::FontOptions (big ? 30.0f : 22.0f, juce::Font::bold));
    auto unitArea = area.removeFromBottom (14);
    g.drawText (formatReading (value), area, juce::Justification::centredLeft);
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (unit, unitArea, juce::Justification::centredLeft);
}

void LoudnessMeterEditor::paint (juce::Graphics& g)
{
    const auto& t = processor.getTarget();
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Loudness Meter", 10, 6, 200, 24, juce::Justification::centredLeft);

    // Readings: green near the target, yellow away from it; the peak red above its limit.
    auto colourFor = [&t] (float lufs)
    {
        if (lufs <= LoudnessEngine::silence)
            return look::dim;
        return std::abs (lufs - t.lufs) <= 1.0f ? look::loop : look::accent;
    };
    auto row = readings;
    const auto w = row.getWidth() / 4;
    drawReading (g, row.removeFromLeft (w), "Short-term (3 s)", shortTerm, "LUFS", true, colourFor (shortTerm));
    drawReading (g, row.removeFromLeft (w), "Integrated (whole song)", integrated, "LUFS", true,
                 colourFor (integrated));
    drawReading (g, row.removeFromLeft (w), "Momentary (0.4 s)", momentary, "LUFS", false, look::text);
    drawReading (g, row, "True peak", truePeak, "dBTP", false, truePeak > t.peakDbTp ? look::error : look::text);

    // The last minute of short-term loudness, with the target as a line.
    g.setColour (look::panel);
    g.fillRoundedRectangle (graph.toFloat(), 5.0f);
    const auto area = graph.reduced (8).toFloat();
    auto yOf = [&area] (float lufs)
    {
        const auto v = juce::jlimit (0.0f, 1.0f, (lufs - graphBottom) / (graphTop - graphBottom));
        return area.getBottom() - v * area.getHeight();
    };
    g.setFont (juce::FontOptions (10.0f));
    for (int line = 0; line <= (int)(graphTop - graphBottom) / 10; ++line)
    {
        const auto lufs = graphBottom + 10.0f * (float)line;
        g.setColour (look::background);
        g.drawHorizontalLine (juce::roundToInt (yOf (lufs)), area.getX(), area.getRight());
        g.setColour (look::dim);
        g.drawText (juce::String ((int)lufs), juce::Rectangle<float> (area.getX(), yOf (lufs) - 12.0f, 30.0f, 11.0f),
                    juce::Justification::centredLeft);
    }
    g.setColour (look::loop.withAlpha (0.8f));
    g.drawHorizontalLine (juce::roundToInt (yOf (t.lufs)), area.getX(), area.getRight());
    g.drawText ("target", juce::Rectangle<float> (area.getRight() - 50.0f, yOf (t.lufs) - 12.0f, 50.0f, 11.0f),
                juce::Justification::centredRight);

    if (historyCount > 1)
    {
        juce::Path line;
        const auto step = area.getWidth() / (float)(LoudnessEngine::historySteps - 1);
        const auto startX = area.getRight() - step * (float)(historyCount - 1);
        for (int i = 0; i < historyCount; ++i)
        {
            const auto point = juce::Point<float> (startX + step * (float)i, yOf (history[(size_t)i]));
            if (i == 0)
                line.startNewSubPath (point);
            else
                line.lineTo (point);
        }
        g.setColour (look::accent);
        g.strokePath (line, juce::PathStrokeType (1.5f));
    }

    g.setColour (look::text);
    g.setFont (juce::FontOptions (12.5f));
    g.drawFittedText (getVerdict(), verdict, juce::Justification::topLeft, 3);
}

void LoudnessMeterEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (26);
    top.removeFromLeft (150);
    reset.setBounds (top.removeFromRight (70));
    top.removeFromRight (8);
    resetOnPlay.setBounds (top.removeFromRight (150));
    top.removeFromRight (8);
    target.setBounds (top);
    area.removeFromTop (8);
    readings = area.removeFromTop (78);
    area.removeFromTop (6);
    verdict = area.removeFromBottom (50);
    area.removeFromBottom (6);
    graph = area;
}

} // namespace spinus::dsp
