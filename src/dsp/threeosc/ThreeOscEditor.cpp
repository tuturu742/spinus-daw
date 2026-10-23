#include "ThreeOscEditor.h"

#include "ThreeOscProcessor.h"
#include "ThreeOscPresets.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
constexpr int rowHeight = 116, sectionTitle = 22;
}

ThreeOscEditor::ThreeOscEditor (ThreeOscProcessor& p) : juce::AudioProcessorEditor (p), synth (p)
{
    auto& state = p.getParameterState();

    presets.addItem (threeOscPresets::defaultName, 1);
    juce::String category;
    const auto& factory = threeOscPresets::all();
    for (size_t i = 0; i < factory.size(); ++i)
    {
        if (factory[i].category != category)
        {
            category = factory[i].category;
            presets.addSectionHeading (category);
        }
        presets.addItem (factory[i].name, (int)i + 2);
    }
    presets.setTooltip ("3x Osc's presets: choosing one sets every control (Default: a new 3x Osc's settings)");
    presets.onChange = [this]
    {
        if (const auto id = presets.getSelectedId(); id > 0)
            synth.choosePreset (id - 1);
        showCurrentPreset();
    };
    addAndMakeVisible (presets);
    previous.setTooltip ("Previous preset");
    next.setTooltip ("Next preset");
    previous.onClick = [this] { stepPreset (-1); };
    next.onClick = [this] { stepPreset (1); };
    addAndMakeVisible (previous);
    addAndMakeVisible (next);

    for (int osc = 0; osc < 3; ++osc)
    {
        auto& row = rows[(size_t)osc];
        row.shape.addItemList ({ "Sine", "Triangle", "Saw", "Square", "Noise" }, 1);
        row.shapeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            state, ThreeOscProcessor::parameterId ("shape", osc), row.shape);
        addAndMakeVisible (row.shape);

        for (auto [id, name] :
             { std::pair { "coarse", "Coarse" }, std::pair { "fine", "Fine" }, std::pair { "level", "Level" },
               std::pair { "pan", "Pan" }, std::pair { "phase", "Phase" } })
        {
            row.knobs.push_back (
                std::make_unique<ParameterKnob> (state, ThreeOscProcessor::parameterId (id, osc), name));
            addAndMakeVisible (*row.knobs.back());
        }
    }

    auto add = [&] (std::vector<std::unique_ptr<ParameterKnob>>& group, const char* id, const char* name)
    {
        group.push_back (std::make_unique<ParameterKnob> (state, id, name));
        addAndMakeVisible (*group.back());
    };
    add (envelope, "attack", "Attack");
    add (envelope, "decay", "Decay");
    add (envelope, "sustain", "Sustain");
    add (envelope, "release", "Release");
    add (filterKnobs, "cutoff", "Cutoff");
    add (filterKnobs, "resonance", "Resonance");
    add (filterKnobs, "envelope", "Envelope");
    add (master, "gain", "Volume");
    add (master, "velocity", "Velocity");
    add (master, "glide", "Glide");

    voices.addItemList ({ "Poly", "Mono (slides)" }, 1);
    voices.setTooltip ("Mono: one note at a time; a note played while another is held slides to it (Glide)");
    voicesAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "voices", voices);
    addAndMakeVisible (voices);

    filter.addItemList ({ "Filter off", "Low pass", "High pass", "Band pass" }, 1);
    filterAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "filter", filter);
    addAndMakeVisible (filter);

    setResizable (true, true);
    setResizeLimits (660, 600, 1400, 1100);
    setSize (740, 620);
    showCurrentPreset();
    startTimerHz (4);
}

ThreeOscEditor::~ThreeOscEditor() = default;

void ThreeOscEditor::stepPreset (int delta)
{
    const auto count = synth.getNumPrograms();
    const auto current = synth.getCurrentProgram();
    synth.choosePreset (((current + delta) % count + count) % count);
    showCurrentPreset();
}

void ThreeOscEditor::timerCallback() { showCurrentPreset(); }

void ThreeOscEditor::showCurrentPreset()
{
    // The preset whose settings the controls have; none once a control has been changed.
    const auto current = synth.findCurrentProgram();
    if (current >= 0)
        presets.setSelectedId (current + 1, juce::dontSendNotification);
    else
    {
        presets.setTextWhenNothingSelected (synth.getProgramName (synth.getCurrentProgram()) + " (changed)");
        presets.setSelectedId (0, juce::dontSendNotification);
    }
}

void ThreeOscEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("3x Osc", 10, 6, 140, 24, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    for (int osc = 0; osc < 3; ++osc)
    {
        const auto bounds = rows[(size_t)osc].shape.getBounds();
        g.setColour (look::panel);
        g.fillRoundedRectangle (juce::Rectangle<int> (6, bounds.getY() - 26, getWidth() - 12, rowHeight - 4).toFloat(),
                                5.0f);
        g.setColour (look::text);
        g.drawText ("Osc " + juce::String (osc + 1), bounds.getX(), bounds.getY() - 24, 80, 20,
                    juce::Justification::centredLeft);
    }

    g.setColour (look::text);
    const auto y = envelope.front()->getY() - sectionTitle;
    g.drawText ("Envelope", envelope.front()->getX(), y, 120, sectionTitle, juce::Justification::centredLeft);
    g.drawText ("Filter", filter.getX(), y, 120, sectionTitle, juce::Justification::centredLeft);
    g.drawText ("Output", voices.getX(), y, 120, sectionTitle, juce::Justification::centredLeft);
}

void ThreeOscEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    // The preset menu, between its arrows, right of the title.
    auto top = area.removeFromTop (30).withTrimmedLeft (90).withTrimmedBottom (4);
    top = top.removeFromLeft (juce::jmin (360, top.getWidth()));
    previous.setBounds (top.removeFromLeft (28));
    next.setBounds (top.removeFromRight (28));
    presets.setBounds (top.reduced (4, 0));

    for (auto& row : rows)
    {
        auto r = area.removeFromTop (rowHeight).withTrimmedTop (24).withTrimmedBottom (6);
        row.shape.setBounds (r.removeFromLeft (110).withSizeKeepingCentre (110, 26));
        r.removeFromLeft (10);
        const auto w = r.getWidth() / (int)row.knobs.size();
        for (auto& k : row.knobs)
            k->setBounds (r.removeFromLeft (w));
    }

    area.removeFromTop (sectionTitle + 12);
    auto bottom = area.removeFromTop (juce::jmin (110, area.getHeight()));
    // Envelope (4 knobs), filter (a type menu one and a half knobs wide, 3 knobs), output (a voices menu and
    // 3 knobs).
    const auto unit = bottom.getWidth() * 2 / 27;
    for (auto& k : envelope)
        k->setBounds (bottom.removeFromLeft (unit));
    bottom.removeFromLeft (unit / 4);
    filter.setBounds (bottom.removeFromLeft (unit * 3 / 2).withSizeKeepingCentre (unit * 3 / 2 - 6, 26));
    for (auto& k : filterKnobs)
        k->setBounds (bottom.removeFromLeft (unit));
    bottom.removeFromLeft (unit / 4);
    voices.setBounds (bottom.removeFromLeft (unit * 3 / 2).withSizeKeepingCentre (unit * 3 / 2 - 6, 26));
    const auto masterWidth = juce::jmin (unit, bottom.getWidth() / (int)master.size());
    for (auto& k : master)
        k->setBounds (bottom.removeFromLeft (masterWidth));
}

} // namespace spinus::dsp
