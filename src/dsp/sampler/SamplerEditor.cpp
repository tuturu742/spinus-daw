#include "SamplerEditor.h"

#include "SamplerProcessor.h"
#include "dsp/DeviceControls.h"

namespace spinus::dsp
{

namespace
{
using look::accent;
using look::background;
using look::dim;
using look::panel;
using look::text;
const auto loopColour = look::loop;

void configureNoteSlider (juce::Slider& s, int min, int max, bool notes)
{
    s.setSliderStyle (juce::Slider::IncDecButtons);
    s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 22);
    s.setRange (min, max, 1);
    if (notes)
    {
        s.textFromValueFunction = [] (double v) { return noteName ((int)v); };
        s.valueFromTextFunction = [] (const juce::String& t)
        { return (double)noteFromFileName (t.trim()).value_or (t.getIntValue()); };
        s.updateText();
    }
}
} // namespace

//==============================================================================
/** The selected zone's waveform, with its start/end (yellow) and loop (green) markers to drag. */
class SamplerEditor::Waveform final : public juce::Component
{
public:
    explicit Waveform (SamplerEditor& e) : editor (e) {}

    void setZone (const SampleZone* z)
    {
        hasZone = z != nullptr;
        if (hasZone)
            zone = *z;
        else
            peaksFor = nullptr;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (panel);
        if (! hasZone || zone.audio == nullptr || zone.getLength() == 0)
        {
            g.setColour (dim);
            g.drawText ("Drop audio files here, or click Load", getLocalBounds(), juce::Justification::centred);
            return;
        }

        updatePeaks();
        const auto mid = (float)getHeight() * 0.5f;
        if (zone.loop)
        {
            g.setColour (loopColour.withAlpha (0.15f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xOf (zone.loopStart), 0.0f, xOf (zone.loopEnd),
                                                                    (float)getHeight()));
        }

        g.setColour (accent.withAlpha (0.8f));
        for (int x = 0; x < (int)peaks.size(); ++x)
            g.drawVerticalLine (x, mid - peaks[(size_t)x].second * mid, mid - peaks[(size_t)x].first * mid);

        // Outside start..end is not played.
        g.setColour (background.withAlpha (0.6f));
        g.fillRect (0.0f, 0.0f, xOf (zone.start), (float)getHeight());
        g.fillRect (xOf (zone.end), 0.0f, (float)getWidth() - xOf (zone.end), (float)getHeight());

        auto marker = [&] (juce::int64 at, juce::Colour c, const char* label)
        {
            // Kept inside the view: the end marker is usually at the very end.
            const auto x = juce::jlimit (0, getWidth() - 1, juce::roundToInt (xOf (at)));
            g.setColour (c);
            g.drawVerticalLine (x, 0.0f, (float)getHeight());
            g.setFont (juce::FontOptions (10.0f));
            if (x > getWidth() - 44)
                g.drawText (label, x - 42, 2, 40, 12, juce::Justification::right);
            else
                g.drawText (label, x + 2, 2, 40, 12, juce::Justification::left);
        };
        marker (zone.start, accent, "Start");
        marker (zone.end, accent, "End");
        if (zone.loop)
        {
            marker (zone.loopStart, loopColour, "Loop");
            marker (zone.loopEnd, loopColour, "");
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = nullptr;
        if (! hasZone)
            return;

        // The nearest marker within 8 pixels.
        float best = 8.0f;
        auto consider = [&] (juce::int64 SampleZone::* member, bool visible)
        {
            if (visible)
                if (auto d = std::abs (xOf (zone.*member) - e.position.x); d < best)
                {
                    best = d;
                    dragging = member;
                }
        };
        consider (&SampleZone::start, true);
        consider (&SampleZone::end, true);
        consider (&SampleZone::loopStart, zone.loop);
        consider (&SampleZone::loopEnd, zone.loop);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging == nullptr || ! hasZone)
            return;
        const auto at =
            (juce::int64)juce::jlimit (0.0, (double)zone.getLength(),
                                       (double)e.position.x / juce::jmax (1, getWidth()) * (double)zone.getLength());
        const auto member = dragging;
        editor.sampler.updateZone (editor.selectedZone, [member, at] (SampleZone& z) { z.*member = at; });
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = nullptr; }

private:
    float xOf (juce::int64 sample) const
    {
        return hasZone && zone.getLength() > 0 ? (float)sample / (float)zone.getLength() * (float)getWidth() : 0.0f;
    }

    void updatePeaks()
    {
        if (peaksFor == zone.audio.get() && (int)peaks.size() == getWidth())
            return;

        peaksFor = zone.audio.get();
        peaks.assign ((size_t)juce::jmax (0, getWidth()), { 0.0f, 0.0f });
        const auto& audio = *zone.audio;
        const auto length = audio.getNumSamples();
        for (int x = 0; x < getWidth(); ++x)
        {
            const auto from = (int)((juce::int64)x * length / getWidth());
            const auto to = juce::jmax (from + 1, (int)((juce::int64)(x + 1) * length / getWidth()));
            const auto range = audio.findMinMax (0, from, juce::jmin (to, length) - from);
            peaks[(size_t)x] = { juce::jlimit (-1.0f, 1.0f, range.getStart()),
                                 juce::jlimit (-1.0f, 1.0f, range.getEnd()) };
        }
    }

    SamplerEditor& editor;
    SampleZone zone;
    bool hasZone = false;
    const juce::AudioBuffer<float>* peaksFor = nullptr;
    std::vector<std::pair<float, float>> peaks;
    juce::int64 SampleZone::* dragging = nullptr;
};

//==============================================================================
//==============================================================================
SamplerEditor::SamplerEditor (SamplerProcessor& p) : juce::AudioProcessorEditor (p), sampler (p)
{
    waveform = std::make_unique<Waveform> (*this);
    addAndMakeVisible (*waveform);

    zones.setTextWhenNoChoicesAvailable ("No samples");
    zones.onChange = [this] { selectZone (zones.getSelectedItemIndex()); };
    addAndMakeVisible (zones);

    load.onClick = [this] { chooseFiles (true); };
    add.onClick = [this] { chooseFiles (false); };
    remove.onClick = [this] { sampler.removeZone (selectedZone); };
    loop.onClick = [this]
    { sampler.updateZone (selectedZone, [on = loop.getToggleState()] (SampleZone& z) { z.loop = on; }); };
    for (auto* c :
         { (juce::Component*)&load, (juce::Component*)&add, (juce::Component*)&remove, (juce::Component*)&loop })
        addAndMakeVisible (c);

    auto zoneSlider = [this] (juce::Slider& s, int min, int max, bool notes, auto member)
    {
        configureNoteSlider (s, min, max, notes);
        s.onValueChange = [this, &s, member]
        { sampler.updateZone (selectedZone, [v = (int)s.getValue(), member] (SampleZone& z) { z.*member = v; }); };
        addAndMakeVisible (s);
    };
    zoneSlider (root, 0, 127, true, &SampleZone::rootNote);
    zoneSlider (lowKey, 0, 127, true, &SampleZone::lowKey);
    zoneSlider (highKey, 0, 127, true, &SampleZone::highKey);
    zoneSlider (lowVelocity, 1, 127, false, &SampleZone::lowVelocity);
    zoneSlider (highVelocity, 1, 127, false, &SampleZone::highVelocity);

    for (auto [label, t] : { std::pair { &rootLabel, "Root" }, std::pair { &keysLabel, "Keys" },
                             std::pair { &velocityLabel, "Velocity" } })
    {
        label->setText (t, juce::dontSendNotification);
        label->setColour (juce::Label::textColourId, dim);
        addAndMakeVisible (*label);
    }
    message.setColour (juce::Label::textColourId, look::error);
    addAndMakeVisible (message);

    auto& state = sampler.getParameterState();
    for (auto [id, name] :
         { std::pair { "gain", "Volume" }, std::pair { "attack", "Attack" }, std::pair { "decay", "Decay" },
           std::pair { "sustain", "Sustain" }, std::pair { "release", "Release" }, std::pair { "cutoff", "Cutoff" },
           std::pair { "resonance", "Resonance" }, std::pair { "velocity", "Velocity" },
           std::pair { "coarse", "Pitch" }, std::pair { "fine", "Fine" } })
    {
        knobs.push_back (std::make_unique<ParameterKnob> (state, id, name));
        addAndMakeVisible (*knobs.back());
    }

    filter.addItemList ({ "Filter off", "Low pass", "High pass", "Band pass" }, 1);
    filterAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, "filter", filter);
    oneShotAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, "oneShot", oneShot);
    addAndMakeVisible (filter);
    addAndMakeVisible (oneShot);

    for (auto* c : getChildren())
    {
        c->setColour (juce::ToggleButton::textColourId, text);
        c->setColour (juce::Slider::textBoxTextColourId, text);
    }

    sampler.addChangeListener (this);
    refresh();
    setResizable (true, true);
    setResizeLimits (560, 360, 1600, 1000);
    setSize (720, 420);
}

SamplerEditor::~SamplerEditor() { sampler.removeChangeListener (this); }

void SamplerEditor::selectZone (int index)
{
    selectedZone = juce::jmax (0, index);
    refresh();
}

void SamplerEditor::refresh()
{
    const auto all = sampler.getZones();
    selectedZone = juce::jlimit (0, juce::jmax (0, (int)all.size() - 1), selectedZone);

    zones.clear (juce::dontSendNotification);
    for (size_t i = 0; i < all.size(); ++i)
        zones.addItem (juce::String ((int)i + 1) + ": " + all[i].name + "  (" + noteName (all[i].rootNote) + ")",
                       (int)i + 1);
    zones.setSelectedItemIndex (selectedZone, juce::dontSendNotification);

    const auto* zone = all.empty() ? nullptr : &all[(size_t)selectedZone];
    waveform->setZone (zone);
    for (auto* c : { (juce::Component*)&root, (juce::Component*)&lowKey, (juce::Component*)&highKey,
                     (juce::Component*)&lowVelocity, (juce::Component*)&highVelocity, (juce::Component*)&loop,
                     (juce::Component*)&remove })
        c->setEnabled (zone != nullptr);

    if (zone != nullptr)
    {
        root.setValue (zone->rootNote, juce::dontSendNotification);
        lowKey.setValue (zone->lowKey, juce::dontSendNotification);
        highKey.setValue (zone->highKey, juce::dontSendNotification);
        lowVelocity.setValue (zone->lowVelocity, juce::dontSendNotification);
        highVelocity.setValue (zone->highVelocity, juce::dontSendNotification);
        loop.setToggleState (zone->loop, juce::dontSendNotification);
    }
}

void SamplerEditor::chooseFiles (bool replace)
{
    chooser = std::make_unique<core::FileDialog> (replace ? "Load samples" : "Add samples",
                                                  juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                  SamplerProcessor::getFormats().getWildcardForAllFormats());
    const auto chooserFlags =
        (int)((unsigned)juce::FileBrowserComponent::openMode | (unsigned)juce::FileBrowserComponent::canSelectFiles |
              (unsigned)juce::FileBrowserComponent::canSelectMultipleItems);
    chooser->launchAsync (
        chooserFlags,
        [safe = juce::Component::SafePointer<SamplerEditor> (this), replace] (const core::FileDialog& fc)
        {
            if (safe == nullptr || fc.getResults().isEmpty())
                return;
            const auto r = safe->sampler.loadFiles (fc.getResults(), replace);
            safe->message.setText (r.getErrorMessage(), juce::dontSendNotification);
        });
}

bool SamplerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (SamplerProcessor::getFormats().findFormatForFileExtension (juce::File (f).getFileExtension()) != nullptr)
            return true;
    return false;
}

void SamplerEditor::filesDropped (const juce::StringArray& paths, int, int)
{
    juce::Array<juce::File> files;
    for (auto& f : paths)
        files.add (juce::File (f));
    // Dropping replaces the samples (as in FL); "Add..." adds to them.
    const auto r = sampler.loadFiles (files, true);
    message.setText (r.getErrorMessage(), juce::dontSendNotification);
}

void SamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Sampler", 10, 6, 100, 24, juce::Justification::centredLeft);
}

void SamplerEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    top.removeFromLeft (90);
    zones.setBounds (top.removeFromLeft (juce::jmin (260, top.getWidth() / 2)));
    for (auto* b : { &load, &add, &remove })
    {
        top.removeFromLeft (6);
        b->setBounds (top.removeFromLeft (70));
    }
    top.removeFromLeft (10);
    loop.setBounds (top.removeFromLeft (70));

    area.removeFromTop (6);
    auto knobsArea = area.removeFromBottom (86);
    auto zoneRow = area.removeFromBottom (30);
    message.setBounds (area.removeFromBottom (20));
    waveform->setBounds (area.reduced (0, 4));

    auto placeLabelled = [&] (juce::Label& label, int labelWidth, std::initializer_list<juce::Slider*> sliders)
    {
        label.setBounds (zoneRow.removeFromLeft (labelWidth));
        for (auto* s : sliders)
        {
            s->setBounds (zoneRow.removeFromLeft (100).reduced (2, 3));
            zoneRow.removeFromLeft (2);
        }
        zoneRow.removeFromLeft (10);
    };
    placeLabelled (rootLabel, 36, { &root });
    placeLabelled (keysLabel, 36, { &lowKey, &highKey });
    placeLabelled (velocityLabel, 58, { &lowVelocity, &highVelocity });

    auto controls = knobsArea.removeFromRight (110);
    filter.setBounds (controls.removeFromTop (28).reduced (0, 2));
    oneShot.setBounds (controls.removeFromTop (28));
    const auto knobWidth = knobsArea.getWidth() / juce::jmax (1, (int)knobs.size());
    for (auto& k : knobs)
        k->setBounds (knobsArea.removeFromLeft (knobWidth));
}

} // namespace spinus::dsp
