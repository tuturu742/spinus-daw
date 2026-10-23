#include "EqEditor.h"

#include "EqProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
constexpr float minHz = 20.0f, maxHz = 20000.0f, rangeDb = 24.0f, spectrumFloorDb = -96.0f;
constexpr int controlsHeight = 150;

const std::array<juce::Colour, EqSettings::numBands> bandColours {
    juce::Colour (0xffe06c6c), juce::Colour (0xffe0a05c), juce::Colour (0xffe8c547), juce::Colour (0xff8fc95a),
    juce::Colour (0xff4fc1a6), juce::Colour (0xff5aa6e0), juce::Colour (0xff9a83e6), juce::Colour (0xffd47ad0),
};

bool sameBand (const EqBand& a, const EqBand& b)
{
    return a.on == b.on && a.type == b.type && juce::exactlyEqual (a.frequencyHz, b.frequencyHz) &&
           juce::exactlyEqual (a.gainDb, b.gainDb) && juce::exactlyEqual (a.q, b.q) && a.slope == b.slope;
}

juce::String describe (int index, const EqBand& b)
{
    auto text = juce::String (index + 1) + ": " + EqProcessor::getTypeNames()[(int)b.type] + ", " +
                (b.frequencyHz < 1000.0f ? juce::String (juce::roundToInt (b.frequencyHz)) + " Hz"
                                         : juce::String (b.frequencyHz / 1000.0f, 2) + " kHz");
    if (b.usesGain())
        text << ", " << (b.gainDb > 0.0f ? "+" : "") << juce::String (b.gainDb, 1) << " dB";
    text << ", Q " << juce::String (b.q, 2);
    if (! b.on)
        text << " (off)";
    return text;
}
} // namespace

EqEditor::EqEditor (EqProcessor& p) : juce::AudioProcessorEditor (p), processor (p)
{
    for (int b = 0; b < EqSettings::numBands; ++b)
    {
        auto button = std::make_unique<juce::TextButton> (juce::String (b + 1));
        button->setColour (juce::TextButton::buttonColourId, look::panel);
        button->setColour (juce::TextButton::buttonOnColourId, bandColours[(size_t)b].withAlpha (0.8f));
        button->setColour (juce::TextButton::textColourOffId, bandColours[(size_t)b]);
        button->setColour (juce::TextButton::textColourOnId, look::background);
        button->setTooltip ("Band " + juce::String (b + 1));
        button->onClick = [this, b] { selectBand (b); };
        addAndMakeVisible (*button);
        bandButtons.push_back (std::move (button));
    }

    type.addItemList (EqProcessor::getTypeNames(), 1);
    slope.addItemList ({ "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 1);
    onButton.setColour (juce::ToggleButton::textColourId, look::text);
    onButton.setColour (juce::ToggleButton::tickColourId, look::accent);
    addAndMakeVisible (onButton);
    addAndMakeVisible (type);
    addAndMakeVisible (slope);

    frequency = std::make_unique<ParameterKnob> ("Frequency");
    gain = std::make_unique<ParameterKnob> ("Gain");
    q = std::make_unique<ParameterKnob> ("Q");
    output = std::make_unique<ParameterKnob> (p.getParameterState(), "output", "Output");
    for (auto* k : { frequency.get(), gain.get(), q.get(), output.get() })
        addAndMakeVisible (*k);

    attachToBand();
    shown = processor.getSettings();
    processor.getEngine().setSpectrumWanted (true);

    setResizable (true, true);
    setResizeLimits (620, 420, 1600, 1000);
    setSize (760, 470);
    startTimerHz (30);
}

EqEditor::~EqEditor() { processor.getEngine().setSpectrumWanted (false); }

void EqEditor::selectBand (int band)
{
    selected = juce::jlimit (0, EqSettings::numBands - 1, band);
    attachToBand();
    repaint();
}

void EqEditor::attachToBand()
{
    auto& state = processor.getParameterState();
    onAttachment.reset();
    typeAttachment.reset();
    slopeAttachment.reset();
    onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        state, EqProcessor::bandParameter (selected, "on"), onButton);
    typeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        state, EqProcessor::bandParameter (selected, "type"), type);
    slopeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        state, EqProcessor::bandParameter (selected, "slope"), slope);
    frequency->attach (state, EqProcessor::bandParameter (selected, "freq"));
    gain->attach (state, EqProcessor::bandParameter (selected, "gain"));
    q->attach (state, EqProcessor::bandParameter (selected, "q"));
    for (size_t b = 0; b < bandButtons.size(); ++b)
        bandButtons[b]->setToggleState ((int)b == selected, juce::dontSendNotification);

    const auto band = processor.getSettings().bands[(size_t)selected];
    slope.setVisible (band.type == EqBand::Type::lowCut || band.type == EqBand::Type::highCut);
    gain->setEnabled (band.usesGain());
}

juce::Rectangle<int> EqEditor::getDisplayArea() const
{
    return getLocalBounds().reduced (12).withTrimmedTop (30).withTrimmedBottom (controlsHeight);
}

float EqEditor::xFor (float hz) const
{
    const auto area = getDisplayArea().toFloat();
    return area.getX() +
           area.getWidth() * std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
}

float EqEditor::yFor (float db) const
{
    const auto area = getDisplayArea().toFloat().reduced (0.0f, 8.0f);
    return area.getCentreY() - area.getHeight() * 0.5f * juce::jlimit (-rangeDb, rangeDb, db) / rangeDb;
}

float EqEditor::frequencyAt (float x) const
{
    const auto area = getDisplayArea().toFloat();
    return juce::jlimit (minHz, maxHz, minHz * std::pow (maxHz / minHz, (x - area.getX()) / area.getWidth()));
}

float EqEditor::gainAt (float y) const
{
    const auto area = getDisplayArea().toFloat().reduced (0.0f, 8.0f);
    return juce::jlimit (-rangeDb, rangeDb, (area.getCentreY() - y) / (area.getHeight() * 0.5f) * rangeDb);
}

juce::Point<float> EqEditor::getBandPosition (int band) const
{
    const auto& b = shown.bands[(size_t)juce::jlimit (0, EqSettings::numBands - 1, band)];
    return { xFor (b.frequencyHz), yFor (b.usesGain() ? b.gainDb : 0.0f) };
}

int EqEditor::bandAt (juce::Point<float> point) const
{
    int found = -1;
    float nearest = 11.0f;
    for (int b = 0; b < EqSettings::numBands; ++b)
        if (const auto d = getBandPosition (b).getDistanceFrom (point); d < nearest)
        {
            nearest = d;
            found = b;
        }
    return found;
}

void EqEditor::setBandValue (int band, const char* part, float value)
{
    if (auto* parameter = processor.getParameterState().getParameter (EqProcessor::bandParameter (band, part)))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void EqEditor::beginGesture (int band)
{
    for (auto* part : { "freq", "gain" })
        if (auto* parameter = processor.getParameterState().getParameter (EqProcessor::bandParameter (band, part)))
            parameter->beginChangeGesture();
}

void EqEditor::endGesture()
{
    if (dragging < 0)
        return;
    for (auto* part : { "freq", "gain" })
        if (auto* parameter = processor.getParameterState().getParameter (EqProcessor::bandParameter (dragging, part)))
            parameter->endChangeGesture();
    dragging = -1;
}

void EqEditor::dragBandTo (int band, juce::Point<float> point)
{
    const auto settings = processor.getSettings();
    const auto& b = settings.bands[(size_t)band];
    if (! b.on)
        setBandValue (band, "on", 1.0f);
    setBandValue (band, "freq", frequencyAt (point.x));
    if (b.usesGain())
        setBandValue (band, "gain", gainAt (point.y));
    shown = processor.getSettings();
    repaint (getDisplayArea());
}

void EqEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto band = bandAt (e.position);
    if (band < 0)
        return;
    if (band != selected)
        selectBand (band);
    if (e.mods.isPopupMenu())
    {
        showTypeMenu (band);
        return;
    }
    dragging = band;
    beginGesture (band);
}

void EqEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging >= 0)
        dragBandTo (dragging, getDisplayArea().toFloat().getConstrainedPoint (e.position));
}

void EqEditor::mouseUp (const juce::MouseEvent&) { endGesture(); }

void EqEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto band = bandAt (e.position); band >= 0)
        setBandValue (band, "on", processor.getSettings().bands[(size_t)band].on ? 0.0f : 1.0f);
}

void EqEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto band = bandAt (e.position) >= 0 ? bandAt (e.position) : selected;
    const auto now = processor.getSettings().bands[(size_t)band].q;
    setBandValue (band, "q", juce::jlimit (0.1f, 18.0f, now * std::exp (wheel.deltaY * 1.5f)));
}

void EqEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto band = bandAt (e.position);
    setMouseCursor (band >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    if (band != hovered)
    {
        hovered = band;
        repaint (getDisplayArea());
    }
}

void EqEditor::showTypeMenu (int band)
{
    const auto b = processor.getSettings().bands[(size_t)band];
    juce::PopupMenu menu;
    menu.addSectionHeader ("Band " + juce::String (band + 1));
    menu.addItem (1, "On", true, b.on);
    menu.addSeparator();
    const auto names = EqProcessor::getTypeNames();
    for (int t = 0; t < names.size(); ++t)
        menu.addItem (10 + t, names[t], true, (int)b.type == t);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe = juce::Component::SafePointer<EqEditor> (this), band, on = b.on] (int chosen)
                        {
                            if (safe == nullptr || chosen == 0)
                                return;
                            if (chosen == 1)
                                safe->setBandValue (band, "on", on ? 0.0f : 1.0f);
                            else
                                safe->setBandValue (band, "type", (float)(chosen - 10));
                            safe->attachToBand();
                        });
}

void EqEditor::addSamples (const float* samples, int count)
{
    for (int i = 0; i < count; ++i)
    {
        recent[(size_t)recentWrite] = samples[i];
        recentWrite = (recentWrite + 1) % fftSize;
    }
}

void EqEditor::showSpectrumOf (const float* samples, int count)
{
    addSamples (samples, count);
    spectrumDb.fill (spectrumFloorDb);
    updateSpectrum();
    repaint();
}

void EqEditor::updateSpectrum()
{
    // The newest fftSize samples, oldest first, windowed.
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t)i] = recent[(size_t)((recentWrite + i) % fftSize)];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable (fftData.data(), (size_t)fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const auto rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    const auto binHz = rate / fftSize;
    const auto scale = 4.0f / (float)fftSize; // a full-scale sine reads 0 dB through the Hann window
    for (int p = 0; p < spectrumPoints; ++p)
    {
        const auto from = minHz * std::pow (maxHz / minHz, (float)p / spectrumPoints);
        const auto to = minHz * std::pow (maxHz / minHz, (float)(p + 1) / spectrumPoints);
        const auto first = juce::jlimit (1, fftSize / 2 - 1, (int)std::floor (from / binHz));
        const auto last = juce::jlimit (first, fftSize / 2 - 1, (int)std::ceil (to / binHz));
        float level = 0.0f;
        for (int bin = first; bin <= last; ++bin)
            level = juce::jmax (level, fftData[(size_t)bin]);
        const auto db = juce::Decibels::gainToDecibels (level * scale, spectrumFloorDb);
        auto& shownDb = spectrumDb[(size_t)p];
        shownDb = juce::jmax (db, shownDb - 1.5f); // falls back slowly
    }
    hasSpectrum = true;
}

void EqEditor::timerCallback()
{
    const auto read = processor.getEngine().readSpectrumSamples (incoming.data(), (int)incoming.size());
    bool changed = false;
    if (read > 0)
    {
        addSamples (incoming.data(), read);
        updateSpectrum();
        changed = true;
    }
    else if (hasSpectrum)
    {
        bool falling = false;
        for (auto& db : spectrumDb)
            if (db > spectrumFloorDb)
            {
                db = juce::jmax (spectrumFloorDb, db - 1.5f);
                falling = true;
            }
        changed = falling;
    }

    const auto now = processor.getSettings();
    for (size_t b = 0; b < now.bands.size(); ++b)
        if (! sameBand (now.bands[b], shown.bands[b]))
            changed = true;
    if (! juce::exactlyEqual (now.outputDb, shown.outputDb))
        changed = true;
    const auto& band = now.bands[(size_t)selected];
    slope.setVisible (band.type == EqBand::Type::lowCut || band.type == EqBand::Type::highCut);
    gain->setEnabled (band.usesGain());
    shown = now;
    if (changed)
        repaint (getDisplayArea().expanded (2, 24));
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("EQ", 10, 6, 140, 24, juce::Justification::centredLeft);

    const auto area = getDisplayArea().toFloat();
    g.setColour (look::panel);
    g.fillRoundedRectangle (area, 5.0f);

    // Grid.
    g.setFont (juce::FontOptions (10.0f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const auto x = xFor (hz);
        g.setColour (look::dim.withAlpha (0.18f));
        g.drawVerticalLine ((int)x, area.getY(), area.getBottom());
        g.setColour (look::dim);
        g.drawText (hz < 1000.0f ? juce::String ((int)hz) : juce::String ((int)(hz / 1000.0f)) + "k", (int)x + 3,
                    (int)area.getBottom() - 14, 40, 12, juce::Justification::left);
    }
    for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
    {
        const auto y = yFor (db);
        g.setColour (look::dim.withAlpha (juce::exactlyEqual (db, 0.0f) ? 0.4f : 0.15f));
        g.drawHorizontalLine ((int)y, area.getX(), area.getRight());
        g.setColour (look::dim);
        g.drawText ((db > 0.0f ? "+" : "") + juce::String ((int)db), (int)area.getRight() - 30, (int)y - 12, 26, 12,
                    juce::Justification::right);
    }

    // The spectrum of the output, behind the curve (0 dBFS at the top, -96 at the bottom).
    if (hasSpectrum)
    {
        juce::Path spectrum;
        spectrum.startNewSubPath (area.getX(), area.getBottom());
        for (int p = 0; p < spectrumPoints; ++p)
        {
            const auto hz = minHz * std::pow (maxHz / minHz, ((float)p + 0.5f) / spectrumPoints);
            const auto level = (spectrumDb[(size_t)p] - spectrumFloorDb) / -spectrumFloorDb;
            spectrum.lineTo (xFor (hz), area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, level));
        }
        spectrum.lineTo (area.getRight(), area.getBottom());
        spectrum.closeSubPath();
        g.setColour (look::text.withAlpha (0.10f));
        g.fillPath (spectrum);
        g.setColour (look::text.withAlpha (0.22f));
        g.strokePath (spectrum, juce::PathStrokeType (1.0f));
    }

    // The curves: the selected band's own, and the whole EQ's.
    const auto rate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
    std::array<EqEngine::Design, EqSettings::numBands> designs;
    for (size_t b = 0; b < designs.size(); ++b)
        designs[b] = shown.bands[b].on ? EqEngine::design (shown.bands[b], rate) : EqEngine::Design {};
    const auto outputGain = juce::Decibels::decibelsToGain (shown.outputDb, -100.0f);
    juce::Path total, own;
    const int points = juce::jmax (100, (int)area.getWidth() / 2);
    for (int i = 0; i <= points; ++i)
    {
        const auto hz = minHz * std::pow (maxHz / minHz, (float)i / (float)points);
        double gainHere = outputGain;
        for (auto& d : designs)
            gainHere *= EqEngine::magnitude (d, hz, rate);
        const auto ownGain = EqEngine::magnitude (designs[(size_t)selected], hz, rate);
        const juce::Point<float> p (xFor (hz), yFor (juce::Decibels::gainToDecibels ((float)gainHere, -100.0f)));
        const juce::Point<float> o (xFor (hz), yFor (juce::Decibels::gainToDecibels ((float)ownGain, -100.0f)));
        if (i == 0)
        {
            total.startNewSubPath (p);
            own.startNewSubPath (o);
        }
        else
        {
            total.lineTo (p);
            own.lineTo (o);
        }
    }
    if (shown.bands[(size_t)selected].on)
    {
        g.setColour (bandColours[(size_t)selected].withAlpha (0.55f));
        g.strokePath (own, juce::PathStrokeType (1.2f));
    }
    auto filled = total;
    filled.lineTo (area.getRight(), yFor (0.0f));
    filled.lineTo (area.getX(), yFor (0.0f));
    filled.closeSubPath();
    g.setColour (look::accent.withAlpha (0.12f));
    g.fillPath (filled);
    g.setColour (look::accent);
    g.strokePath (total, juce::PathStrokeType (2.0f));

    // The band points.
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    for (int b = 0; b < EqSettings::numBands; ++b)
    {
        const auto& band = shown.bands[(size_t)b];
        const auto centre = getBandPosition (b);
        const auto r = juce::Rectangle<float> (16.0f, 16.0f).withCentre (centre);
        g.setColour (bandColours[(size_t)b].withAlpha (band.on ? 0.9f : 0.3f));
        g.fillEllipse (r);
        if (b == selected)
        {
            g.setColour (look::text);
            g.drawEllipse (r.expanded (1.5f), 1.5f);
        }
        g.setColour (look::background.withAlpha (band.on ? 1.0f : 0.6f));
        g.drawText (juce::String (b + 1), r, juce::Justification::centred);
    }

    // What the band under the mouse (or the selected one) does.
    const auto about = hovered >= 0 ? hovered : selected;
    g.setColour (look::text);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (describe (about, shown.bands[(size_t)about]), getDisplayArea().reduced (8, 4),
                juce::Justification::topLeft);
}

void EqEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto controls = area.removeFromBottom (controlsHeight).withTrimmedTop (8);

    auto buttons = controls.removeFromTop (26);
    const auto buttonWidth = juce::jmin (40, buttons.getWidth() / EqSettings::numBands);
    for (auto& b : bandButtons)
        b->setBounds (buttons.removeFromLeft (buttonWidth).reduced (2, 0));

    controls.removeFromTop (6);
    output->setBounds (controls.removeFromRight (90));
    auto choices = controls.removeFromLeft (150).reduced (4, 0);
    onButton.setBounds (choices.removeFromTop (26));
    choices.removeFromTop (4);
    type.setBounds (choices.removeFromTop (26));
    choices.removeFromTop (4);
    slope.setBounds (choices.removeFromTop (26));
    const auto unit = juce::jmin (100, controls.getWidth() / 3);
    for (auto* k : { frequency.get(), gain.get(), q.get() })
        k->setBounds (controls.removeFromLeft (unit));
}

} // namespace spinus::dsp
