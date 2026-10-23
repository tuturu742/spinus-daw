#include "DynamicsEditors.h"

#include "DynamicsProcessors.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
constexpr int knobRow = 100, choiceRow = 28, statusRow = 36;
constexpr float reductionRangeDb = 24.0f;

juce::String sidechainStatus (bool followsSidechain, bool soundArrives, const char* what)
{
    if (! followsSidechain)
        return juce::String ("Follows its own sound. To ") + what +
               " on another insert's sound (e.g. the kick's), choose Sidechain and, in the Mixer, right-click "
               "this insert's name, Sidechain from.";
    if (soundArrives)
        return "Follows the sidechain: sound is arriving.";
    return "Set to follow the sidechain, but no sound arrives yet. In the Mixer, right-click this insert's name "
           "and choose Sidechain from, then the insert to follow.";
}
} // namespace

void ReadingHistory::push (float value) noexcept
{
    values[(size_t)write] = value;
    write = (write + 1) % length;
}

//==============================================================================
DynamicsEditorBase::DynamicsEditorBase (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& s,
                                        const juce::String& name)
    : juce::AudioProcessorEditor (p), state (s), title (name)
{
    setResizable (true, true);
    setResizeLimits (560, 380, 1400, 900);
}

DynamicsEditorBase::~DynamicsEditorBase() = default;

void DynamicsEditorBase::addKnob (const char* id, const char* label)
{
    knobs.push_back (std::make_unique<ParameterKnob> (state, id, label));
    addAndMakeVisible (*knobs.back());
}

void DynamicsEditorBase::addChoice (const char* id)
{
    auto box = std::make_unique<juce::ComboBox>();
    if (auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id)))
        box->addItemList (parameter->choices, 1);
    box->setTooltip (state.getParameter (id)->getName (100));
    choiceAttachments.push_back (
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, id, *box));
    addAndMakeVisible (*box);
    choices.push_back (std::move (box));
}

void DynamicsEditorBase::addToggle (const char* id, const char* label)
{
    auto button = std::make_unique<juce::ToggleButton> (label);
    button->setColour (juce::ToggleButton::textColourId, look::text);
    button->setColour (juce::ToggleButton::tickColourId, look::accent);
    toggleAttachments.push_back (
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, id, *button));
    addAndMakeVisible (*button);
    toggles.push_back (std::move (button));
}

void DynamicsEditorBase::setStatus (const juce::String& text)
{
    if (text != status)
    {
        status = text;
        repaint (statusArea);
    }
}

void DynamicsEditorBase::paintFrame (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText (title, 10, 6, 200, 24, juce::Justification::centredLeft);
    g.setColour (look::panel);
    g.fillRoundedRectangle (display.toFloat(), 5.0f);
    g.setColour (status.startsWith ("Set to follow") ? look::dim : look::text);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (status, statusArea, juce::Justification::centredLeft, 2);
}

void DynamicsEditorBase::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);
    auto knobArea = area.removeFromBottom (knobRow);
    auto choiceArea = area.removeFromBottom (choiceRow);
    statusArea = area.removeFromBottom (statusRow);
    area.removeFromBottom (4);
    display = area;

    const auto unit = juce::jmin (96, knobArea.getWidth() / juce::jmax (1, (int)knobs.size()));
    for (auto& k : knobs)
        k->setBounds (knobArea.removeFromLeft (unit));
    for (auto& c : choices)
        c->setBounds (choiceArea.removeFromLeft (150).reduced (4, 1));
    for (auto& t : toggles)
        t->setBounds (choiceArea.removeFromLeft (130).reduced (4, 1));
}

void DynamicsEditorBase::drawReductionMeter (juce::Graphics& g, juce::Rectangle<float> area, float reductionDb,
                                             float rangeDb)
{
    g.setColour (look::background);
    g.fillRoundedRectangle (area, 3.0f);
    const auto inner = area.reduced (3.0f);
    const auto depth = juce::jlimit (0.0f, 1.0f, -reductionDb / rangeDb);
    g.setColour (juce::Colour (0xffe8913a)); // the Mixer's gain-reduction orange
    g.fillRect (inner.withHeight (inner.getHeight() * depth));
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (juce::String (reductionDb, 1), area.withTrimmedTop (area.getHeight() - 14).expanded (14.0f, 0.0f),
                juce::Justification::centred);
}

void DynamicsEditorBase::drawHistory (juce::Graphics& g, juce::Rectangle<float> area, const ReadingHistory& history,
                                      float bottomDb, juce::Colour colour, bool fillFromTop)
{
    juce::Path line;
    for (int age = ReadingHistory::length - 1; age >= 0; --age)
    {
        const auto x = area.getRight() - area.getWidth() * (float)age / (float)(ReadingHistory::length - 1);
        const auto y = area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, history.get (age) / bottomDb);
        if (age == ReadingHistory::length - 1)
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);
    }
    auto filled = line;
    const auto edge = fillFromTop ? area.getY() : area.getBottom();
    filled.lineTo (area.getRight(), edge);
    filled.lineTo (area.getX(), edge);
    filled.closeSubPath();
    g.setColour (colour.withAlpha (0.25f));
    g.fillPath (filled);
    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (1.5f));
}

//==============================================================================
CompressorEditor::CompressorEditor (CompressorProcessor& p)
    : DynamicsEditorBase (p, p.getParameterState(), "Compressor"), processor (p)
{
    addKnob ("threshold", "Threshold");
    addKnob ("ratio", "Ratio");
    addKnob ("knee", "Knee");
    addKnob ("attack", "Attack");
    addKnob ("release", "Release");
    addKnob ("makeup", "Makeup");
    addKnob ("mix", "Mix");
    addChoice ("detector");
    addChoice ("sidechain");
    addToggle ("auto_makeup", "Auto makeup");
    setSize (700, 420);
    timerCallback();
    startTimerHz (30);
}

void CompressorEditor::timerCallback()
{
    const auto& engine = processor.getEngine();
    const auto now = juce::Time::getMillisecondCounter();
    if (engine.getSidechainPeak() > 0.003f)
        lastSidechainSound = now;
    setStatus (sidechainStatus (processor.getSettings().sidechain,
                                lastSidechainSound != 0 && now - lastSidechainSound < 2000, "compress"));
    shownReduction = engine.getDeepestReductionDb();
    shownLevel = engine.getInputLevelDb();
    reduction.push (shownReduction);
    repaint (display);
}

void CompressorEditor::paint (juce::Graphics& g)
{
    paintFrame (g);
    const auto settings = processor.getSettings();
    auto area = display.toFloat().reduced (8.0f);
    const auto meter = area.removeFromRight (22.0f);
    area.removeFromRight (8.0f);
    const auto curveArea = area.removeFromLeft (juce::jmin (area.getHeight(), area.getWidth() * 0.45f));
    area.removeFromLeft (10.0f);
    const auto historyArea = area;

    // Transfer curve: input level (right) against output level (up), -60 to 0 dB, makeup included.
    constexpr float lowDb = -60.0f;
    auto xFor = [&] (float db) { return curveArea.getX() + curveArea.getWidth() * (db - lowDb) / -lowDb; };
    auto yFor = [&] (float db)
    { return curveArea.getBottom() - curveArea.getHeight() * juce::jlimit (0.0f, 1.0f, (db - lowDb) / -lowDb); };
    g.setColour (look::background.withAlpha (0.6f));
    g.fillRect (curveArea);
    g.setColour (look::dim.withAlpha (0.3f));
    g.drawLine (xFor (lowDb), yFor (lowDb), xFor (0.0f), yFor (0.0f), 1.0f);
    g.drawVerticalLine ((int)xFor (settings.thresholdDb), curveArea.getY(), curveArea.getBottom());
    const auto makeupDb = settings.makeupDb + (settings.autoMakeup ? CompressorEngine::autoMakeupDb (settings) : 0.0f);
    auto outputFor = [&] (float in)
    {
        const auto compressed = CompressorEngine::staticCurve (settings, in) + makeupDb;
        const auto m = juce::jlimit (0.0f, 1.0f, settings.mix);
        return juce::Decibels::gainToDecibels ((1.0f - m) * juce::Decibels::decibelsToGain (in) +
                                                   m * juce::Decibels::decibelsToGain (compressed),
                                               -120.0f);
    };
    juce::Path curve;
    for (int i = 0; i <= 120; ++i)
    {
        const auto in = lowDb * (1.0f - (float)i / 120.0f);
        const juce::Point<float> point (xFor (in), yFor (outputFor (in)));
        if (i == 0)
            curve.startNewSubPath (point);
        else
            curve.lineTo (point);
    }
    g.saveState();
    g.reduceClipRegion (curveArea.toNearestInt());
    g.setColour (look::accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f));
    if (shownLevel > lowDb)
    {
        g.setColour (look::text);
        g.fillEllipse (
            juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ xFor (shownLevel), yFor (outputFor (shownLevel)) }));
    }
    g.restoreState();
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("in", curveArea.reduced (4.0f), juce::Justification::bottomRight);
    g.drawText ("out", curveArea.reduced (4.0f), juce::Justification::topLeft);

    // Gain reduction over the last seconds (growing down from the top) and now.
    g.setColour (look::background.withAlpha (0.6f));
    g.fillRect (historyArea);
    drawHistory (g, historyArea, reduction, -reductionRangeDb, juce::Colour (0xffe8913a), true);
    g.setColour (look::dim);
    g.drawText ("gain reduction, last 5 s", historyArea.reduced (4.0f), juce::Justification::bottomLeft);
    drawReductionMeter (g, meter, shownReduction, reductionRangeDb);
}

//==============================================================================
LimiterEditor::LimiterEditor (LimiterProcessor& p)
    : DynamicsEditorBase (p, p.getParameterState(), "Limiter"), processor (p)
{
    addKnob ("input", "Input gain");
    addKnob ("ceiling", "Ceiling");
    addKnob ("release", "Release");
    addKnob ("lookahead", "Lookahead");
    addToggle ("true_peak", "True peak");
    setSize (620, 400);
    timerCallback();
    startTimerHz (30);
}

void LimiterEditor::timerCallback()
{
    const auto& engine = processor.getEngine();
    shownReduction = engine.getDeepestReductionDb();
    shownOutput = engine.getOutputPeakDb();
    reduction.push (shownReduction);
    const auto ms = processor.getLookaheadMs();
    setStatus ("The look-ahead delays the sound by " + juce::String (ms, 1) +
               " ms so the limiter can turn down before a peak; Spinus keeps the other tracks in time with it.");
    repaint (display);
}

void LimiterEditor::paint (juce::Graphics& g)
{
    paintFrame (g);
    auto area = display.toFloat().reduced (8.0f);
    const auto output = area.removeFromRight (22.0f);
    area.removeFromRight (6.0f);
    const auto meter = area.removeFromRight (22.0f);
    area.removeFromRight (8.0f);

    g.setColour (look::background.withAlpha (0.6f));
    g.fillRect (area);
    drawHistory (g, area, reduction, -reductionRangeDb, juce::Colour (0xffe8913a), true);
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("gain reduction, last 5 s", area.reduced (4.0f), juce::Justification::bottomLeft);
    drawReductionMeter (g, meter, shownReduction, reductionRangeDb);

    // The output peak, against the ceiling (-24 to 0 dB).
    const auto ceiling = processor.getSettings().ceilingDb;
    auto yFor = [&] (float db)
    { return output.getBottom() - output.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 24.0f) / 24.0f); };
    g.setColour (look::background);
    g.fillRoundedRectangle (output, 3.0f);
    g.setColour (look::loop);
    g.fillRect (output.reduced (3.0f, 0.0f).withTop (yFor (shownOutput)).withBottom (output.getBottom() - 3.0f));
    g.setColour (look::error);
    g.drawHorizontalLine ((int)yFor (ceiling), output.getX(), output.getRight());
    g.setColour (look::dim);
    g.drawText ("out", output.withTrimmedTop (output.getHeight() - 14).expanded (6.0f, 0.0f),
                juce::Justification::centred);
}

//==============================================================================
GateEditor::GateEditor (GateProcessor& p) : DynamicsEditorBase (p, p.getParameterState(), "Gate"), processor (p)
{
    addKnob ("threshold", "Threshold");
    addKnob ("hysteresis", "Hysteresis");
    addKnob ("range", "Range");
    addKnob ("attack", "Attack");
    addKnob ("hold", "Hold");
    addKnob ("release", "Release");
    addChoice ("sidechain");
    level.fill (-120.0f);
    setSize (660, 420);
    timerCallback();
    startTimerHz (30);
}

juce::String GateEditor::getStateText() const
{
    switch (processor.getEngine().getState())
    {
    case GateEngine::State::open:
        return "Open";
    case GateEngine::State::opening:
        return "Opening";
    case GateEngine::State::holding:
        return "Holding";
    case GateEngine::State::closing:
        return "Closing";
    case GateEngine::State::closed:
        break;
    }
    return "Closed";
}

void GateEditor::timerCallback()
{
    const auto& engine = processor.getEngine();
    const auto now = juce::Time::getMillisecondCounter();
    if (engine.getSidechainPeak() > 0.003f)
        lastSidechainSound = now;
    setStatus (sidechainStatus (processor.getSettings().sidechain,
                                lastSidechainSound != 0 && now - lastSidechainSound < 2000, "open"));
    level.push (engine.getLevelDb());
    gain.push (juce::Decibels::gainToDecibels (engine.getGain(), -120.0f));
    repaint (display);
}

void GateEditor::paint (juce::Graphics& g)
{
    paintFrame (g);
    const auto settings = processor.getSettings();
    auto area = display.toFloat().reduced (8.0f);
    const auto stateArea = area.removeFromTop (18.0f);
    g.setColour (look::background.withAlpha (0.6f));
    g.fillRect (area);

    // Level over the last seconds (-80 to 0 dB, up), against the thresholds; the gain as a band behind.
    constexpr float lowDb = -80.0f;
    auto yFor = [&] (float db)
    { return area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, (db - lowDb) / -lowDb); };
    juce::Path gainPath, levelPath;
    for (int age = ReadingHistory::length - 1; age >= 0; --age)
    {
        const auto x = area.getRight() - area.getWidth() * (float)age / (float)(ReadingHistory::length - 1);
        const auto gy = area.getBottom() - area.getHeight() * 0.25f * juce::Decibels::decibelsToGain (gain.get (age));
        const auto ly = yFor (level.get (age));
        if (age == ReadingHistory::length - 1)
        {
            gainPath.startNewSubPath (x, gy);
            levelPath.startNewSubPath (x, ly);
        }
        else
        {
            gainPath.lineTo (x, gy);
            levelPath.lineTo (x, ly);
        }
    }
    auto gainFill = gainPath;
    gainFill.lineTo (area.getRight(), area.getBottom());
    gainFill.lineTo (area.getX(), area.getBottom());
    gainFill.closeSubPath();
    g.setColour (look::loop.withAlpha (0.25f));
    g.fillPath (gainFill);
    g.setColour (look::text.withAlpha (0.8f));
    g.strokePath (levelPath, juce::PathStrokeType (1.5f));

    g.setColour (look::accent);
    g.drawHorizontalLine ((int)yFor (settings.thresholdDb), area.getX(), area.getRight());
    g.setColour (look::accent.withAlpha (0.45f));
    g.drawHorizontalLine ((int)yFor (settings.thresholdDb - settings.hysteresisDb), area.getX(), area.getRight());
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("opens", juce::Rectangle<float> (area.getX() + 4.0f, yFor (settings.thresholdDb) - 13.0f, 60.0f, 12.0f),
                juce::Justification::left);
    g.drawText ("closes",
                juce::Rectangle<float> (area.getX() + 4.0f, yFor (settings.thresholdDb - settings.hysteresisDb) + 1.0f,
                                        60.0f, 12.0f),
                juce::Justification::left);

    const auto text = getStateText();
    g.setColour (text == "Closed" ? look::dim : look::loop);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (text, stateArea, juce::Justification::centredRight);
    g.setColour (look::dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("level and gate, last 5 s", stateArea, juce::Justification::centredLeft);
}

} // namespace spinus::dsp
