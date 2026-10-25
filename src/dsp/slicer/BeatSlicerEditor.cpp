#include "BeatSlicerEditor.h"

#include "BeatSlicerProcessor.h"
#include "dsp/DeviceControls.h"
#include "dsp/sampler/SampleStorage.h"

namespace spinus::dsp
{

//==============================================================================
class BeatSlicerEditor::Waveform final : public juce::Component
{
public:
    explicit Waveform (BeatSlicerProcessor& s) : slicer (s) {}

    void refresh()
    {
        // Peaks per pixel column, so painting does not walk the whole loop.
        peaks.clear();
        const auto& loop = slicer.getLoop();
        clampView();
        if (loop.audio != nullptr && getWidth() > 0)
        {
            const auto& audio = *loop.audio;
            const auto length = audio.getNumSamples();
            for (int x = 0; x < getWidth(); ++x)
            {
                const auto from = juce::jlimit (0, juce::jmax (0, length - 1), (int)toSample ((float)x));
                const auto to = juce::jlimit (from + 1, juce::jmax (from + 1, length), (int)toSample ((float)(x + 1)));
                float peak = 0.0f;
                for (int c = 0; c < audio.getNumChannels(); ++c)
                    peak = juce::jmax (peak, audio.getMagnitude (c, from, juce::jmin (to, length) - from));
                peaks.push_back (peak);
            }
        }
        repaint();
    }

    void resized() override { refresh(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (look::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
        if (! slicer.hasLoop())
        {
            g.setColour (look::dim);
            g.drawText ("Drop a drum loop here, or click Load loop...", getLocalBounds(), juce::Justification::centred);
            return;
        }

        const auto mid = (float)getHeight() / 2.0f;
        g.setColour (look::accent.withAlpha (0.7f));
        for (size_t x = 0; x < peaks.size(); ++x)
        {
            const auto h = peaks[x] * mid * 0.95f;
            g.drawVerticalLine ((int)x, mid - h, mid + h);
        }

        const auto& points = slicer.getSlicePoints();
        g.setFont (juce::FontOptions (10.0f));
        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto x = toX (points[i]);
            g.setColour ((int)i == playing ? look::loop.withAlpha (0.25f) : juce::Colours::transparentBlack);
            const auto next = i + 1 < points.size() ? toX (points[i + 1]) : toX (slicer.getLoop().getLength());
            g.fillRect (x, 0.0f, next - x, (float)getHeight());
            g.setColour (look::text);
            g.drawVerticalLine ((int)x, 0.0f, (float)getHeight());
            g.drawText (juce::String ((int)i + 1), (int)x + 3, 2, 30, 12, juce::Justification::left);
            // The key it plays from, at the bottom (right-click the slice to choose another).
            if (next - x >= 22.0f)
            {
                g.setColour (look::dim);
                g.drawText (BeatSlicerProcessor::keyName (slicer.getSliceKey ((int)i)), (int)x + 3, getHeight() - 14,
                            (int)(next - x) - 4, 12, juce::Justification::left, false);
            }
        }
    }

    // The part of the loop shown: all of it, or (zoomed in with Ctrl+wheel) a part, as fractions of its length.
    float toX (juce::int64 sample) const
    {
        const auto length = std::max<juce::int64> (1, slicer.getLoop().getLength());
        return (float)((double)getWidth() * ((double)sample / (double)length - viewStart) / viewSize);
    }

    juce::int64 toSample (float x) const
    {
        const auto length = slicer.getLoop().getLength();
        const auto fraction = viewStart + (double)x / juce::jmax (1, getWidth()) * viewSize;
        return std::clamp<juce::int64> ((juce::int64)(fraction * (double)length), 0,
                                        std::max<juce::int64> (0, length - 1));
    }

    /** Ctrl+wheel zooms around the mouse (up to 64 times); the wheel then scrolls. */
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const auto delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
        if (e.mods.isCtrlDown())
            zoom (delta > 0.0f ? 1.25 : 1.0 / 1.25, e.position.x);
        else if (viewSize < 1.0)
        {
            viewStart -= (double)delta * viewSize * 0.25;
            refresh();
        }
        else
            juce::Component::mouseWheelMove (e, wheel);
    }

    void mouseMagnify (const juce::MouseEvent& e, float scale) override { zoom (scale, e.position.x); }

    void zoom (double factor, float aroundX)
    {
        const auto at = viewStart + (double)aroundX / juce::jmax (1, getWidth()) * viewSize;
        viewSize = juce::jlimit (1.0 / 64.0, 1.0, viewSize / factor);
        viewStart = at - (double)aroundX / juce::jmax (1, getWidth()) * viewSize;
        refresh();
    }

    void clampView() { viewStart = juce::jlimit (0.0, 1.0 - viewSize, viewStart); }

    /** The marker under x (within a few pixels), or -1. The first marker cannot be moved. */
    int markerAt (float x) const
    {
        const auto& points = slicer.getSlicePoints();
        for (size_t i = 1; i < points.size(); ++i)
            if (std::abs (toX (points[i]) - x) < 5.0f)
                return (int)i;
        return -1;
    }

    int sliceAt (float x) const
    {
        const auto& points = slicer.getSlicePoints();
        const auto sample = toSample (x);
        int slice = 0;
        for (size_t i = 0; i < points.size(); ++i)
            if (points[i] <= sample)
                slice = (int)i;
        return slice;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (markerAt (e.position.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                     : juce::MouseCursor::PointingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! slicer.hasLoop())
            return;
        dragging = markerAt (e.position.x);
        if (e.mods.isPopupMenu())
        {
            if (dragging > 0)
                slicer.removeSlicePoint (dragging);
            else
                showKeyMenu (sliceAt (e.position.x));
            dragging = -1;
            return;
        }
        if (dragging < 0)
        {
            playing = sliceAt (e.position.x);
            slicer.audition (playing, true);
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging > 0)
            slicer.moveSlicePoint (dragging, toSample (e.position.x));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (playing >= 0)
            slicer.audition (playing, false);
        playing = dragging = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (slicer.hasLoop() && markerAt (e.position.x) < 0)
            slicer.addSlicePoint (toSample (e.position.x));
    }

    /** Right-click a slice: the key it plays from (a slice already on that key swaps with it). */
    void showKeyMenu (int slice)
    {
        const auto current = slicer.getSliceKey (slice);
        const auto& keys = slicer.getSliceKeys();
        juce::PopupMenu menu, octaves;
        menu.addSectionHeader ("Slice " + juce::String (slice + 1) + " plays from " +
                               BeatSlicerProcessor::keyName (current));
        for (int octave = 1; octave <= 9; ++octave)
        {
            juce::PopupMenu notes;
            for (int key = octave * 12; key < octave * 12 + 12 && key < 128; ++key)
            {
                const auto holder = std::find (keys.begin(), keys.end(), key);
                const auto text = BeatSlicerProcessor::keyName (key) +
                                  (holder != keys.end() && key != current
                                       ? "  (slice " + juce::String ((int)(holder - keys.begin()) + 1) + ": swaps)"
                                       : juce::String());
                notes.addItem (text, true, key == current, [this, slice, key] { slicer.setSliceKey (slice, key); });
            }
            octaves.addSubMenu (BeatSlicerProcessor::keyName (octave * 12) + " to " +
                                    BeatSlicerProcessor::keyName (juce::jmin (127, octave * 12 + 11)),
                                notes, true, nullptr, current / 12 == octave);
        }
        menu.addSubMenu ("Play from key", octaves);
        menu.addItem ("Next free key up",
                      [this, slice, current]
                      {
                          const auto& k = slicer.getSliceKeys();
                          for (int key = current + 1; key < 128; ++key)
                              if (std::find (k.begin(), k.end(), key) == k.end())
                              {
                                  slicer.setSliceKey (slice, key);
                                  return;
                              }
                      });
        menu.addSeparator();
        menu.addItem ("Reset all keys (slice 1 on C5, then up)", [this] { slicer.resetSliceKeys(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

private:
    BeatSlicerProcessor& slicer;
    std::vector<float> peaks;
    int dragging = -1, playing = -1;
    double viewStart = 0.0, viewSize = 1.0;
};

//==============================================================================
BeatSlicerEditor::BeatSlicerEditor (BeatSlicerProcessor& p) : juce::AudioProcessorEditor (p), slicer (p)
{
    waveform = std::make_unique<Waveform> (p);
    addAndMakeVisible (*waveform);

    load.onClick = [this] { chooseFile(); };
    addAndMakeVisible (load);
    toPattern.setTooltip ("Writes a note for each slice into the pattern, so it plays the loop back");
    toPattern.onClick = [this]
    {
        if (slicer.onSlicesToPattern)
            slicer.onSlicesToPattern();
    };
    addChildComponent (toPattern);

    method.addItemList ({ "Slice at hits", "Slice by grid" }, 1);
    method.onChange = [this] { reslice(); };
    for (auto beats : BeatSlicerProcessor::gridLengths)
        grid.addItem ("Every " + BeatSlicerProcessor::describeBeats (beats), grid.getNumItems() + 1);
    grid.onChange = [this] { reslice(); };
    for (auto beats : BeatSlicerProcessor::loopLengths)
        length.addItem (BeatSlicerProcessor::describeBeats (beats), length.getNumItems() + 1);
    length.onChange = [this]
    {
        const auto index = length.getSelectedItemIndex();
        if (juce::isPositiveAndBelow (index, (int)BeatSlicerProcessor::loopLengths.size()))
            slicer.setLoopBeats (BeatSlicerProcessor::loopLengths[(size_t)index]);
    };
    sensitivity.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivity.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sensitivity.setRange (0.0, 1.0);
    sensitivity.setTooltip ("Sensitivity: further right finds quieter hits");
    sensitivity.onDragEnd = [this] { reslice(); };
    methodLabel.setText ("Slices", juce::dontSendNotification);
    lengthLabel.setText ("Loop length", juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &method, &grid, &length, &sensitivity, &methodLabel,
                                                             &lengthLabel, &message })
        addAndMakeVisible (c);
    for (auto* l : { &methodLabel, &lengthLabel, &message })
        l->setColour (juce::Label::textColourId, look::dim);

    auto& state = p.getParameterState();
    for (auto [id, name] :
         { std::pair { "gain", "Volume" }, std::pair { "coarse", "Pitch" }, std::pair { "fine", "Fine" },
           std::pair { "attack", "Attack" }, std::pair { "release", "Release" } })
    {
        knobs.push_back (std::make_unique<ParameterKnob> (state, id, name));
        addAndMakeVisible (*knobs.back());
    }
    playToEnd.setColour (juce::ToggleButton::textColourId, look::text);
    playToEndAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, "playToEnd", playToEnd);
    addAndMakeVisible (playToEnd);

    slicer.addChangeListener (this);
    setResizable (true, true);
    setResizeLimits (640, 420, 1600, 1000);
    setSize (760, 460);
    refresh();
}

BeatSlicerEditor::~BeatSlicerEditor() { slicer.removeChangeListener (this); }

void BeatSlicerEditor::refresh()
{
    toPattern.setVisible (slicer.onSlicesToPattern != nullptr);
    toPattern.setEnabled (slicer.hasLoop());

    method.setSelectedItemIndex (slicer.getMethod() == BeatSlicerProcessor::Method::grid ? 1 : 0,
                                 juce::dontSendNotification);
    const bool byGrid = slicer.getMethod() == BeatSlicerProcessor::Method::grid;
    grid.setVisible (byGrid);
    sensitivity.setVisible (! byGrid);
    sensitivity.setValue (slicer.getSensitivity(), juce::dontSendNotification);
    for (size_t i = 0; i < BeatSlicerProcessor::gridLengths.size(); ++i)
        if (juce::approximatelyEqual (BeatSlicerProcessor::gridLengths[i], slicer.getGridBeats()))
            grid.setSelectedItemIndex ((int)i, juce::dontSendNotification);
    length.setSelectedItemIndex (-1, juce::dontSendNotification);
    for (size_t i = 0; i < BeatSlicerProcessor::loopLengths.size(); ++i)
        if (juce::approximatelyEqual (BeatSlicerProcessor::loopLengths[i], slicer.getLoopBeats()))
            length.setSelectedItemIndex ((int)i, juce::dontSendNotification);
    if (length.getSelectedItemIndex() < 0)
        length.setText (BeatSlicerProcessor::describeBeats (slicer.getLoopBeats()), juce::dontSendNotification);

    const auto& loop = slicer.getLoop();
    message.setText (slicer.hasLoop()
                         ? loop.name + ": " + juce::String ((int)slicer.getSlicePoints().size()) +
                               " slices, each on the key shown at its bottom. Click a slice to hear "
                               "it; right-click it to choose its key. Drag a marker to move it, "
                               "double-click to add one, right-click a marker to remove it. Ctrl+wheel zooms."
                         : "No loop yet.",
                     juce::dontSendNotification);
    waveform->refresh();
}

void BeatSlicerEditor::reslice()
{
    const auto index = grid.getSelectedItemIndex();
    const auto gridBeats = juce::isPositiveAndBelow (index, (int)BeatSlicerProcessor::gridLengths.size())
                               ? BeatSlicerProcessor::gridLengths[(size_t)index]
                               : slicer.getGridBeats();
    slicer.slice (method.getSelectedItemIndex() == 1 ? BeatSlicerProcessor::Method::grid
                                                     : BeatSlicerProcessor::Method::transients,
                  (float)sensitivity.getValue(), gridBeats);
}

void BeatSlicerEditor::chooseFile()
{
    chooser =
        std::make_unique<core::FileDialog> ("Load a loop", juce::File(), getAudioFormats().getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                          {
                              if (safe == nullptr || c.getResult() == juce::File())
                                  return;
                              if (auto r = safe->slicer.loadFile (c.getResult()); r.failed())
                                  safe->message.setText (r.getErrorMessage(), juce::dontSendNotification);
                          });
}

bool BeatSlicerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && getAudioFormats().findFormatForFileExtension (juce::File (files[0]).getFileExtension());
}

void BeatSlicerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (auto r = slicer.loadFile (juce::File (files[0])); r.failed())
        message.setText (r.getErrorMessage(), juce::dontSendNotification);
}

void BeatSlicerEditor::paint (juce::Graphics& g)
{
    g.fillAll (look::background);
    g.setColour (look::accent);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Beat Slicer", 10, 6, 160, 24, juce::Justification::centredLeft);
}

void BeatSlicerEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto top = area.removeFromTop (30);
    top.removeFromLeft (150);
    toPattern.setBounds (top.removeFromRight (140).reduced (0, 2));
    top.removeFromRight (8);
    load.setBounds (top.removeFromRight (110).reduced (0, 2));

    auto bottom = area.removeFromBottom (110);
    playToEnd.setBounds (bottom.removeFromRight (150).withSizeKeepingCentre (150, 28));
    const auto w = bottom.getWidth() / (int)knobs.size();
    for (auto& k : knobs)
        k->setBounds (bottom.removeFromLeft (w));

    area.removeFromBottom (6);
    auto row = area.removeFromBottom (28);
    methodLabel.setBounds (row.removeFromLeft (50));
    method.setBounds (row.removeFromLeft (140).reduced (0, 2));
    row.removeFromLeft (8);
    grid.setBounds (row.removeFromLeft (150).reduced (0, 2));
    sensitivity.setBounds (grid.getBounds());
    row.removeFromLeft (16);
    lengthLabel.setBounds (row.removeFromLeft (80));
    length.setBounds (row.removeFromLeft (110).reduced (0, 2));

    message.setBounds (area.removeFromBottom (36));
    area.removeFromTop (6);
    waveform->setBounds (area);
}

} // namespace spinus::dsp
