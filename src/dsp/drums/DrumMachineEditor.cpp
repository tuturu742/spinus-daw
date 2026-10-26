#include "DrumMachineEditor.h"

#include "../sampler/SampleSet.h"
#include "DrumMachineProcessor.h"
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

// One colour per row of pads, bottom row first (as on hardware drum machines).
const juce::Colour rowColours[] = { juce::Colour (0xffd6604f), juce::Colour (0xffe8a547), juce::Colour (0xff5fae6a),
                                    juce::Colour (0xff4f8fd6) };
} // namespace

//==============================================================================
class DrumMachineEditor::PadButton final : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    PadButton (DrumMachineEditor& e, int index) : editor (e), pad (index) {}

    void paint (juce::Graphics& g) override
    {
        const auto& p = editor.drums.getPads()[(size_t)pad];
        const bool selected = editor.selectedPad == pad;
        auto area = getLocalBounds().toFloat().reduced (3.0f);
        const auto colour = rowColours[pad / 4];

        g.setColour (p.audio != nullptr ? colour.withAlpha (pressed ? 0.9f : 0.45f) : panel);
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (selected ? accent : (dropping ? text : colour.withAlpha (0.8f)));
        g.drawRoundedRectangle (area, 6.0f, selected || dropping ? 2.5f : 1.0f);

        auto inner = area.reduced (6.0f);
        g.setColour (text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (p.audio != nullptr ? p.name : juce::String ("(empty)"), inner.toNearestInt(),
                          juce::Justification::centred, 2);
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (dim.brighter (0.3f));
        g.drawText (juce::String (pad + 1), inner.removeFromTop (12.0f), juce::Justification::topLeft);
        g.drawText (noteName (p.note) + (p.chokeGroup > 0 ? "  choke " + juce::String (p.chokeGroup) : juce::String()),
                    inner.removeFromBottom (12.0f), juce::Justification::bottomLeft);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        editor.selectPad (pad);
        pressed = true;
        editor.drums.audition (pad, true);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        pressed = false;
        editor.drums.audition (pad, false);
        repaint();
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override
    {
        dropping = true;
        repaint();
    }
    void fileDragExit (const juce::StringArray&) override
    {
        dropping = false;
        repaint();
    }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dropping = false;
        editor.selectPad (pad);
        const auto r = editor.drums.loadPadFile (pad, juce::File (files[0]));
        editor.message.setText (r.getErrorMessage(), juce::dontSendNotification);
        repaint();
    }

private:
    DrumMachineEditor& editor;
    int pad;
    bool pressed = false, dropping = false;
};

//==============================================================================
//==============================================================================
DrumMachineEditor::DrumMachineEditor (DrumMachineProcessor& p) : juce::AudioProcessorEditor (p), drums (p)
{
    for (int i = 0; i < DrumMachineProcessor::numPads; ++i)
    {
        padButtons.push_back (std::make_unique<PadButton> (*this, i));
        addAndMakeVisible (*padButtons.back());
    }

    kitButton.onClick = [this] { showKitMenu(); };
    loadButton.onClick = [this] { choosePadFile(); };
    clearButton.onClick = [this] { drums.clearPad (selectedPad); };
    for (auto* b : { &kitButton, &loadButton, &clearButton })
        addAndMakeVisible (b);

    padTitle.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    padTitle.setColour (juce::Label::textColourId, text);
    message.setColour (juce::Label::textColourId, look::error);
    addAndMakeVisible (padTitle);
    addAndMakeVisible (message);

    for (auto [id, name] :
         { std::pair { "volume", "Volume" }, std::pair { "pan", "Pan" }, std::pair { "tune", "Tune" },
           std::pair { "fine", "Fine" }, std::pair { "attack", "Attack" }, std::pair { "decay", "Decay" },
           std::pair { "sustain", "Sustain" }, std::pair { "release", "Release" }, std::pair { "cutoff", "Cutoff" },
           std::pair { "resonance", "Resonance" }, std::pair { "velocity", "Velocity" } })
    {
        knobs.push_back (std::make_unique<ParameterKnob> (name));
        knobIds.add (id);
        addAndMakeVisible (*knobs.back());
    }

    filter.addItemList ({ "Filter off", "Low pass", "High pass", "Band pass" }, 1);
    choke.addItem ("No choke group", 1);
    for (int g = 1; g <= 8; ++g)
        choke.addItem ("Choke group " + juce::String (g), g + 1);
    choke.onChange = [this] { drums.setPadChoke (selectedPad, choke.getSelectedId() - 1); };

    note.setSliderStyle (juce::Slider::IncDecButtons);
    note.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 22);
    note.setRange (0, 127, 1);
    note.textFromValueFunction = [] (double v) { return noteName ((int)v); };
    note.valueFromTextFunction = [] (const juce::String& t)
    { return (double)noteFromFileName (t.trim()).value_or (t.getIntValue()); };
    note.onValueChange = [this] { drums.setPadNote (selectedPad, (int)note.getValue()); };
    noteLabel.setText ("Note", juce::dontSendNotification);
    noteLabel.setColour (juce::Label::textColourId, dim);
    oneShot.setColour (juce::ToggleButton::textColourId, text);
    for (auto* c : { (juce::Component*)&filter, (juce::Component*)&choke, (juce::Component*)&note,
                     (juce::Component*)&noteLabel, (juce::Component*)&oneShot })
        addAndMakeVisible (c);

    drums.addChangeListener (this);
    attachToPad();
    refresh();
    setResizable (true, true);
    setResizeLimits (640, 400, 1600, 1000);
    setSize (800, 520);
}

DrumMachineEditor::~DrumMachineEditor() { drums.removeChangeListener (this); }

void DrumMachineEditor::selectPad (int pad)
{
    if (pad == selectedPad || ! juce::isPositiveAndBelow (pad, DrumMachineProcessor::numPads))
        return;
    selectedPad = pad;
    attachToPad();
    refresh();
}

void DrumMachineEditor::attachToPad()
{
    auto& state = drums.getParameterState();
    for (size_t i = 0; i < knobs.size(); ++i)
        knobs[i]->attach (state, DrumMachineProcessor::parameterId (knobIds[(int)i], selectedPad));
    filterAttachment = nullptr;
    oneShotAttachment = nullptr;
    filterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        state, DrumMachineProcessor::parameterId ("filter", selectedPad), filter);
    oneShotAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        state, DrumMachineProcessor::parameterId ("oneShot", selectedPad), oneShot);
}

void DrumMachineEditor::refresh()
{
    const auto& pad = drums.getPads()[(size_t)selectedPad];
    padTitle.setText ("Pad " + juce::String (selectedPad + 1) + ": " +
                          (pad.audio != nullptr ? pad.name : juce::String ("empty")),
                      juce::dontSendNotification);
    choke.setSelectedId (pad.chokeGroup + 1, juce::dontSendNotification);
    note.setValue (pad.note, juce::dontSendNotification);
    note.updateText();
    clearButton.setEnabled (pad.audio != nullptr);
    kitButton.setButtonText (drums.getKitDisplayName());
    kitButton.setTooltip ("The kit: choose a factory kit, or load or save a .spinuskit file (* once a pad is changed)");
    for (auto& b : padButtons)
        b->repaint();
}

void DrumMachineEditor::choosePadFile()
{
    chooser = std::make_unique<core::FileDialog> ("Load a sample into pad " + juce::String (selectedPad + 1),
                                                  juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    const auto chooserFlags =
        (int)((unsigned)juce::FileBrowserComponent::openMode | (unsigned)juce::FileBrowserComponent::canSelectFiles);
    chooser->launchAsync (
        chooserFlags,
        [safe = juce::Component::SafePointer<DrumMachineEditor> (this), pad = selectedPad] (const core::FileDialog& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File())
                return;
            const auto r = safe->drums.loadPadFile (pad, fc.getResult());
            safe->message.setText (r.getErrorMessage(), juce::dontSendNotification);
        });
}

void DrumMachineEditor::showKitMenu()
{
    juce::PopupMenu menu;
    // The factory kits: the Synth Kit, then the starter pack's (when the pack is installed).
    const auto current = drums.isKitModified() ? juce::String() : drums.getKitName();
    menu.addItem ("Spinus Synth Kit", true, current == "Spinus Synth Kit", [this] { drums.loadSynthKit(); });
    if (const auto pack = starter::findPack(); pack != juce::File())
        for (const auto& kit : starter::readKits (pack))
            menu.addItem (kit.name, true, current == kit.name,
                          [this, kit, pack]
                          {
                              const auto r = drums.loadFactoryKit (kit, pack);
                              message.setText (r.getErrorMessage(), juce::dontSendNotification);
                          });
    menu.addSeparator();
    menu.addItem ("Load kit...",
                  [this]
                  {
                      chooser = std::make_unique<core::FileDialog> (
                          "Load a drum kit", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                          juce::String ("*") + DrumMachineProcessor::kitExtension);
                      chooser->launchAsync (
                          (int)((unsigned)juce::FileBrowserComponent::openMode |
                                (unsigned)juce::FileBrowserComponent::canSelectFiles),
                          [safe = juce::Component::SafePointer<DrumMachineEditor> (this)] (const core::FileDialog& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              const auto r = safe->drums.loadKit (fc.getResult());
                              safe->message.setText (r.getErrorMessage(), juce::dontSendNotification);
                          });
                  });
    menu.addItem ("Save kit...",
                  [this]
                  {
                      chooser = std::make_unique<core::FileDialog> (
                          "Save the drum kit",
                          juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                              .getChildFile (juce::String ("My kit") + DrumMachineProcessor::kitExtension),
                          juce::String ("*") + DrumMachineProcessor::kitExtension);
                      chooser->launchAsync (
                          (int)((unsigned)juce::FileBrowserComponent::saveMode |
                                (unsigned)juce::FileBrowserComponent::canSelectFiles |
                                (unsigned)juce::FileBrowserComponent::warnAboutOverwriting),
                          [safe = juce::Component::SafePointer<DrumMachineEditor> (this)] (const core::FileDialog& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              const auto r = safe->drums.saveKit (
                                  fc.getResult().withFileExtension (DrumMachineProcessor::kitExtension));
                              safe->message.setText (r.getErrorMessage(), juce::dontSendNotification);
                          });
                  });
    menu.addSeparator();
    menu.addItem ("Clear all pads", [this] { drums.clearAllPads(); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitButton));
}

void DrumMachineEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Drum Machine", 10, 6, 140, 24, juce::Justification::centredLeft);
}

void DrumMachineEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    top.removeFromLeft (140);
    kitButton.setBounds (top.removeFromLeft (juce::jmin (220, top.getWidth())));
    area.removeFromTop (8);

    // Pads on the left, square-ish; the bottom row is pads 1-4.
    const auto padSize = juce::jmin ((area.getWidth() - 300) / 4, area.getHeight() / 4);
    auto grid = area.removeFromLeft (padSize * 4);
    for (int i = 0; i < DrumMachineProcessor::numPads; ++i)
    {
        const int row = 3 - i / 4, column = i % 4;
        padButtons[(size_t)i]->setBounds (grid.getX() + column * padSize, grid.getY() + row * padSize, padSize,
                                          padSize);
    }

    area.removeFromLeft (14);
    padTitle.setBounds (area.removeFromTop (24));
    auto buttons = area.removeFromTop (28);
    loadButton.setBounds (buttons.removeFromLeft (110));
    buttons.removeFromLeft (6);
    clearButton.setBounds (buttons.removeFromLeft (80));
    message.setBounds (area.removeFromBottom (20));

    area.removeFromTop (6);
    auto row = area.removeFromTop (28);
    noteLabel.setBounds (row.removeFromLeft (36));
    note.setBounds (row.removeFromLeft (100).reduced (0, 2));
    row.removeFromLeft (8);
    choke.setBounds (row.removeFromLeft (juce::jmin (150, row.getWidth())).reduced (0, 2));
    row = area.removeFromTop (28);
    filter.setBounds (row.removeFromLeft (120).reduced (0, 2));
    row.removeFromLeft (8);
    oneShot.setBounds (row.removeFromLeft (100));

    area.removeFromTop (6);
    const int perRow = 4;
    const auto knobWidth = area.getWidth() / perRow;
    const auto knobHeight = juce::jmin (96, area.getHeight() / 3);
    for (size_t i = 0; i < knobs.size(); ++i)
        knobs[i]->setBounds (area.getX() + (int)(i % perRow) * knobWidth, area.getY() + (int)(i / perRow) * knobHeight,
                             knobWidth, knobHeight);
}

} // namespace spinus::dsp
