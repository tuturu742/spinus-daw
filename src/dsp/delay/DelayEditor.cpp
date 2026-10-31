#include "DelayEditor.h"

#include "DelayProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
constexpr int displayHeight = 150, sectionTitle = 22, rowHeight = 112;
constexpr int mostEchoes = 12;
constexpr float quietest = 0.01f; // -40 dB: echoes below are not drawn
} // namespace

DelayEditor::DelayEditor (DelayProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    auto& state = p.getParameterState();

    auto combo = [&] (juce::ComboBox& box, const char* id)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id));
        jassert (parameter != nullptr);
        box.addItemList (parameter->choices, 1);
        box.setTooltip (parameter->getName (100));
        comboAttachments.push_back (std::make_unique<ComboAttachment> (state, id, box));
        addAndMakeVisible (box);
    };
    combo (sync, "sync");
    combo (mode, "mode");

    auto knob = [&] (std::unique_ptr<ParameterKnob>& k, const char* id, const char* label)
    {
        k = std::make_unique<ParameterKnob> (state, id, label);
        addAndMakeVisible (*k);
    };
    knob (note, "note", "Time");
    knob (time, "time", "Time");
    knob (feedback, "feedback", "Feedback");
    knob (lowCut, "low_cut", "Low cut");
    knob (highCut, "high_cut", "High cut");
    knob (width, "width", "Width");
    knob (mix, "mix", "Mix");

    setResizable (true, true);
    setResizeLimits (580, 330, 1300, 800);
    setSize (640, 350);
    timerCallback();
    startTimerHz (30);
}

DelayEditor::~DelayEditor() = default;

juce::Rectangle<int> DelayEditor::getDisplayArea() const
{
    return getLocalBounds().reduced (12).withTrimmedTop (30).removeFromTop (
        juce::jmax (60, getHeight() - 24 - 30 - sectionTitle - rowHeight - 8));
}

juce::String DelayEditor::getTimeText() const
{
    const auto s = processor.getSettings();
    const auto bpm = (double)processor.getEngine().getBpm();
    auto ms = DelayProcessor::timeText (DelayEngine::timeMsFor (s, bpm));
    if (! s.sync)
        return ms;
    const auto beat = std::find_if (DelayProcessor::noteBeats.begin(), DelayProcessor::noteBeats.end(),
                                    [&] (double b) { return juce::approximatelyEqual (b, s.beats); });
    const auto name = DelayProcessor::getNoteNames()[(int)(beat - DelayProcessor::noteBeats.begin())];
    const auto tempo =
        std::abs (bpm - std::round (bpm)) < 0.05 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 1);
    return name + " at " + tempo + " BPM: " + ms;
}

void DelayEditor::timerCallback()
{
    // The note length for a synced delay, milliseconds for a free one.
    const bool synced = processor.getSettings().sync;
    note->setVisible (synced);
    time->setVisible (! synced);

    // Every setting shows in the picture: redraw when one of them or the tempo changes.
    auto now = getTimeText();
    for (auto* p : processor.getParameters())
        now << "|" << p->getValue();
    if (now != shownTime)
    {
        shownTime = std::move (now);
        repaint();
    }
}

void DelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Spinus Delay", 10, 6, 160, 24, juce::Justification::centredLeft);
    g.setColour (look::text);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (getTimeText(), getLocalBounds().reduced (12, 6).withHeight (24), juce::Justification::centredRight);

    const auto area = getDisplayArea().toFloat();
    g.setColour (look::panel);
    g.fillRoundedRectangle (area, 5.0f);

    // The echoes of one sound: when they come (from the left edge, the sound itself), on which side, how loud.
    const auto s = processor.getSettings();
    const auto ms = DelayEngine::timeMsFor (s, (double)processor.getEngine().getBpm());
    const auto mixAmount = juce::jlimit (0.0f, 1.0f, s.mix);
    const auto dry = juce::jmin (1.0f, 2.0f * (1.0f - mixAmount)), wet = juce::jmin (1.0f, 2.0f * mixAmount);
    int echoes = 1;
    while (echoes < mostEchoes && wet * std::pow (s.feedback, (float)echoes) >= quietest)
        ++echoes;
    const auto span = (float)ms * ((float)juce::jmax (echoes, 4) + 0.5f);

    auto inner = area.reduced (10.0f, 8.0f);
    inner.removeFromBottom (14.0f); // the time under the first echo
    const auto labelWidth = 16.0f;
    auto lanes = inner.withTrimmedLeft (labelWidth);
    const auto laneHeight = lanes.getHeight() / 2.0f;
    const std::array<juce::Rectangle<float>, 2> lane { lanes.withHeight (laneHeight - 3.0f),
                                                       lanes.withTrimmedTop (laneHeight + 3.0f) };
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    for (size_t side = 0; side < 2; ++side)
    {
        g.setColour (look::background.withAlpha (0.6f));
        g.fillRect (lane[side]);
        g.setColour (look::dim);
        g.drawText (side == 0 ? "L" : "R",
                    juce::Rectangle<float> (inner.getX(), lane[side].getY(), labelWidth, lane[side].getHeight()),
                    juce::Justification::centredLeft);
    }

    auto xFor = [&] (double atMs) { return lanes.getX() + 4.0f + (lanes.getWidth() - 8.0f) * (float)(atMs / span); };
    auto bar = [&] (size_t side, float x, float level, juce::Colour colour)
    {
        const auto& r = lane[side];
        const auto h = r.getHeight() * juce::jlimit (0.0f, 1.0f, level);
        g.setColour (colour);
        g.fillRoundedRectangle (x - 2.5f, r.getBottom() - h, 5.0f, h, 1.5f);
    };
    for (size_t side = 0; side < 2; ++side)
        bar (side, xFor (0.0), dry, look::dim);

    const auto spread = juce::jlimit (0.0f, 1.0f, s.width);
    for (int k = 1; k <= echoes; ++k)
    {
        const auto level = wet * std::pow (s.feedback, (float)(k - 1));
        // Which line the echo comes from: ping-pong starts on the left and bounces.
        auto left = level, right = level;
        if (s.mode == DelaySettings::Mode::pingPong)
        {
            left = k % 2 == 1 ? level : 0.0f;
            right = k % 2 == 1 ? 0.0f : level;
        }
        const auto mid = 0.5f * (left + right), sideLevel = 0.5f * (left - right) * spread;
        const auto x = xFor (ms * k);
        bar (0, x, mid + sideLevel, look::accent);
        bar (1, x, mid - sideLevel, look::loop);
    }

    g.setColour (look::dim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (DelayProcessor::timeText (ms),
                juce::Rectangle<float> (xFor (ms) - 40.0f, inner.getBottom() + 1.0f, 80.0f, 12.0f),
                juce::Justification::centred);

    g.setColour (look::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    for (auto [r, text] :
         { std::pair { timeTitle, "Time" }, std::pair { colourTitle, "Echoes" }, std::pair { outputTitle, "Output" } })
        g.drawText (text, r, juce::Justification::centredLeft);
}

void DelayEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30 + getDisplayArea().getHeight() + 8);
    auto row = area.removeFromTop (sectionTitle + rowHeight);
    auto titles = row.removeFromTop (sectionTitle);

    const auto unit = juce::jmin (90, (int)((float)row.getWidth() / 7.4f));
    auto stack = row.removeFromLeft (unit * 14 / 10).reduced (4, 0);
    timeTitle = titles.removeFromLeft (stack.getWidth() + 8 + unit);
    const auto top = stack.getCentreY() - 32;
    sync.setBounds (stack.getX(), top, stack.getWidth(), 26);
    mode.setBounds (stack.getX(), top + 34, stack.getWidth(), 26);
    const auto timeBounds = row.removeFromLeft (unit);
    note->setBounds (timeBounds);
    time->setBounds (timeBounds);
    feedback->setBounds (row.removeFromLeft (unit));
    colourTitle = titles.removeFromLeft (3 * unit);
    lowCut->setBounds (row.removeFromLeft (unit));
    highCut->setBounds (row.removeFromLeft (unit));
    outputTitle = titles;
    width->setBounds (row.removeFromLeft (unit));
    mix->setBounds (row.removeFromLeft (unit));
}

} // namespace spinus::dsp
