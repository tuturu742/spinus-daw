#include "SpinusTune.h"

#include "Shortcuts.h"
#include "Theme.h"
#include "commands/CommandRegistry.h"
#include "engine/TuneRender.h"
#include "model/ModelJson.h"
#include "model/Scales.h"

namespace spinus::ui
{

namespace
{
const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

/** FL's note names: MIDI 60 is C5. */
juce::String noteName (int pitch)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((pitch % 12) + 12) % 12]) + juce::String (pitch / 12);
}

constexpr int keyboardWidth = 52, toolbarHeight = 34;
} // namespace

SpinusTuneEditor::SpinusTuneEditor (Session& s, model::ClipId c, ai::AiService* ai)
    : session (s), clip (c), service (ai)
{
    setWantsKeyboardFocus (true);
    for (auto* b : { &joinButton, &snapButton, &resetButton })
        addAndMakeVisible (b);
    joinButton.setTooltip ("Join the selected note with the next one (J)");
    snapButton.setTooltip ("Move the selected notes (or all) onto the nearest notes of the song's key");
    resetButton.setTooltip ("Undo every correction of the selected notes (or all)");
    joinButton.onClick = [this]
    {
        const auto chosen = getSelection();
        if (! chosen.empty())
            joinNotes (chosen.front(), chosen.front() + 1);
    };
    snapButton.onClick = [this] { snapToKey (selectedOrAll()); };
    resetButton.onClick = [this] { reset (selectedOrAll()); };

    for (auto [slider, label, text, top] : { std::tuple { &driftSlider, &driftLabel, "Drift", 1.0 },
                                             std::tuple { &vibratoSlider, &vibratoLabel, "Vibrato", 2.0 } })
    {
        slider->setRange (0.0, top, 0.01);
        slider->setValue (1.0, juce::dontSendNotification);
        slider->setSliderStyle (juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        label->setText (text, juce::dontSendNotification);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
    }
    driftSlider.setTooltip ("How much of the slow wandering around each selected note stays: 0 = straight on the note");
    vibratoSlider.setTooltip ("How wide the selected notes' vibrato is: 0 = none, 1 = as sung, 2 = twice as wide");
    driftSlider.onDragEnd = [this] { setDrift (selectedOrAll(), (float)driftSlider.getValue()); };
    vibratoSlider.onDragEnd = [this] { setVibrato (selectedOrAll(), (float)vibratoSlider.getValue()); };
    addAndMakeVisible (status);

    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { load(); });
    session.getTuner().addChangeListener (this);
    if (service != nullptr)
        aiListener = service->addListener (
            [this] (const juce::String& method, const juce::var& params)
            {
                if (method != "ai.v1.job.update" || job.isEmpty() || params["job"].toString() != job)
                    return;
                const auto kind = params["kind"].toString();
                if (kind == "done")
                {
                    job.clear();
                    session.getTuner().storeAnalysis (sourceFile(), params["result"]);
                    load();
                }
                else if (kind == "error")
                {
                    job.clear();
                    problem = params["message"].toString();
                    repaint();
                }
            });

    load();
    if (! track)
        analyse();
    startTimerHz (10);
}

SpinusTuneEditor::~SpinusTuneEditor()
{
    session.getHistory().removeListener (historyListener);
    session.getTuner().removeChangeListener (this);
    if (service != nullptr)
        service->removeListener (aiListener);
}

juce::File SpinusTuneEditor::sourceFile() const
{
    const auto* c = session.getProject().findClip (clip);
    const auto* audio = c != nullptr ? std::get_if<model::AudioClip> (&c->content) : nullptr;
    return audio != nullptr ? session.resolveAudioFile (audio->file) : juce::File();
}

void SpinusTuneEditor::load()
{
    const auto* c = session.getProject().findClip (clip);
    const auto* audio = c != nullptr ? std::get_if<model::AudioClip> (&c->content) : nullptr;
    if (audio == nullptr)
        return;

    const auto analysis = session.getTuner().getAnalysis (sourceFile());
    if (analysis.isObject())
    {
        const auto pitch = engine::PitchTrack::fromJson (analysis["track"]);
        frameSeconds = pitch.frameSeconds;
        std::vector<float> midi;
        midi.reserve (pitch.hz.size());
        for (auto hz : pitch.hz)
            midi.push_back (hz > 0.0f ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 0.0f);
        track = std::move (midi);
        duration = juce::jmax (0.1, (double)analysis["duration"]);
    }

    notes = audio->tune;
    if (notes.empty() && analysis.isObject())
        if (auto* found = analysis["notes"].getArray())
            for (auto& n : *found)
                notes.push_back ({ (double)n["start"], (double)n["end"], (float)(double)n["pitch"], 0.0f, 1.0f, 1.0f });

    // Show the notes' range, with some room.
    if (! notes.empty())
    {
        auto low = 127.0f, high = 0.0f;
        for (auto& n : notes)
        {
            low = juce::jmin (low, n.pitch + juce::jmin (0.0f, n.shift));
            high = juce::jmax (high, n.pitch + juce::jmax (0.0f, n.shift));
        }
        lowestPitch = std::floor (low) - 5.0f;
        rowHeight = juce::jlimit (8.0f, 22.0f, (float)juce::jmax (1, grid.getHeight()) / (high - low + 11.0f));
    }
    std::erase_if (selected, [this] (int i) { return i >= (int)notes.size(); });
    repaint();
}

void SpinusTuneEditor::analyse()
{
    if (service == nullptr || ! service->isEnabled())
    {
        problem = "Spinus Tune listens to the vocal once with Spinus's AI service on this computer. Turn AI on in "
                  "Options › AI.";
        return;
    }
    problem.clear();
    service->call ("ai.v1.listen.vocal", object ({ { "file", sourceFile().getFullPathName() } }),
                   [this] (const juce::var& result, const juce::String& error)
                   {
                       if (error.isNotEmpty())
                       {
                           problem = error;
                           repaint();
                           return;
                       }
                       job = result["job"].toString();
                   });
}

juce::String SpinusTuneEditor::getStatusText() const
{
    if (problem.isNotEmpty())
        return problem;
    if (! track)
        return "Listening to the vocal...";
    switch (const auto s = session.getTuner().getStatus (clip); s.state)
    {
    case Tuner::State::rendering:
        return "Rendering the tuned vocal... " + juce::String (juce::roundToInt (s.progress * 100.0f)) + "%";
    case Tuner::State::failed:
        return s.message;
    case Tuner::State::ready:
        return "Tuned. Play the song to hear it.";
    case Tuner::State::needsAnalysis:
    case Tuner::State::none:
        break;
    }
    return "Drag a note up or down to move it (Alt: freely); double-click to split; select notes for Drift and "
           "Vibrato.";
}

//==============================================================================
bool SpinusTuneEditor::commit (const std::vector<model::TuneNote>& changed, const juce::String& description)
{
    if (changed == notes)
        return false;
    juce::Array<juce::var> list;
    for (auto& n : changed)
        list.add (model::tuneNoteToJson (n));
    juce::String error;
    auto command = commands::CommandRegistry::get().create (
        "clip.setTune", object ({ { "clip", (int)clip.value }, { "notes", list }, { "description", description } }),
        error);
    if (command == nullptr || session.getHistory().perform (std::move (command)).failed())
        return false;
    load();
    return true;
}

bool SpinusTuneEditor::moveNotes (const std::vector<int>& indices, float semitones, bool snap)
{
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
        {
            auto& n = changed[(size_t)i];
            auto target = n.pitch + n.shift + semitones;
            if (snap)
                target = (float)juce::roundToInt (target);
            n.shift = juce::jlimit (-24.0f, 24.0f, target - n.pitch);
        }
    return commit (changed, indices.size() > 1 ? "Move notes" : "Move note");
}

bool SpinusTuneEditor::splitNote (int index, double seconds)
{
    if (! juce::isPositiveAndBelow (index, (int)notes.size()))
        return false;
    const auto& n = notes[(size_t)index];
    if (seconds <= n.start + 0.02 || seconds >= n.end - 0.02)
        return false;
    auto changed = notes;
    auto second = n;
    changed[(size_t)index].end = seconds;
    second.start = seconds;
    changed.insert (changed.begin() + index + 1, second);
    return commit (changed, "Split note");
}

bool SpinusTuneEditor::joinNotes (int first, int second)
{
    if (! juce::isPositiveAndBelow (first, (int)notes.size()) || second != first + 1 ||
        ! juce::isPositiveAndBelow (second, (int)notes.size()))
        return false;
    auto changed = notes;
    auto& a = changed[(size_t)first];
    const auto& b = changed[(size_t)second];
    // The joined note sits at the length-weighted average of the two, and keeps the first's correction.
    const auto la = a.end - a.start, lb = b.end - b.start;
    const auto target = a.pitch + a.shift;
    a.pitch = (float)((a.pitch * la + b.pitch * lb) / (la + lb));
    a.shift = target - a.pitch;
    a.end = b.end;
    changed.erase (changed.begin() + second);
    selected = { first };
    return commit (changed, "Join notes");
}

bool SpinusTuneEditor::setDrift (const std::vector<int>& indices, float drift)
{
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
            changed[(size_t)i].drift = juce::jlimit (0.0f, 1.0f, drift);
    return commit (changed, "Straighten drift");
}

bool SpinusTuneEditor::setVibrato (const std::vector<int>& indices, float vibrato)
{
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
            changed[(size_t)i].vibrato = juce::jlimit (0.0f, 2.0f, vibrato);
    return commit (changed, "Change vibrato");
}

bool SpinusTuneEditor::snapToKey (const std::vector<int>& indices)
{
    const auto key = session.getProject().settings.key.value_or (model::Key { 0, "chromatic" });
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
        {
            auto& n = changed[(size_t)i];
            n.shift = (float)model::nearestNoteInKey (key, n.pitch) - n.pitch;
        }
    return commit (changed, "Snap notes to the key");
}

bool SpinusTuneEditor::reset (const std::vector<int>& indices)
{
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
        {
            auto& n = changed[(size_t)i];
            n.shift = 0.0f;
            n.drift = 1.0f;
            n.vibrato = 1.0f;
        }
    return commit (changed, "Reset notes");
}

std::vector<int> SpinusTuneEditor::getSelection() const { return { selected.begin(), selected.end() }; }

void SpinusTuneEditor::select (std::vector<int> indices)
{
    selected = { indices.begin(), indices.end() };
    if (! selected.empty() && *selected.begin() < (int)notes.size())
    {
        const auto& n = notes[(size_t)*selected.begin()];
        driftSlider.setValue (n.drift, juce::dontSendNotification);
        vibratoSlider.setValue (n.vibrato, juce::dontSendNotification);
    }
    repaint();
}

std::vector<int> SpinusTuneEditor::selectedOrAll() const
{
    if (! selected.empty())
        return getSelection();
    std::vector<int> all ((size_t)notes.size());
    std::iota (all.begin(), all.end(), 0);
    return all;
}

//==============================================================================
float SpinusTuneEditor::timeToX (double seconds) const
{
    return (float)grid.getX() + (float)((seconds - viewStart) * pixelsPerSecond);
}

double SpinusTuneEditor::xToTime (float x) const { return viewStart + (x - (float)grid.getX()) / pixelsPerSecond; }

float SpinusTuneEditor::pitchToY (float pitch) const
{
    return (float)grid.getBottom() - (pitch - lowestPitch + 0.5f) * rowHeight;
}

juce::Rectangle<float> SpinusTuneEditor::noteBounds (const model::TuneNote& n) const
{
    const auto y = pitchToY (n.pitch + n.shift);
    return juce::Rectangle<float>::leftTopRightBottom (timeToX (n.start), y - rowHeight * 0.45f, timeToX (n.end),
                                                       y + rowHeight * 0.45f);
}

int SpinusTuneEditor::noteAt (juce::Point<float> p) const
{
    for (int i = (int)notes.size(); --i >= 0;)
        if (noteBounds (notes[(size_t)i]).expanded (0.0f, 2.0f).contains (p))
            return i;
    return -1;
}

void SpinusTuneEditor::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (6, 4);
    resetButton.setBounds (toolbar.removeFromRight (70));
    toolbar.removeFromRight (6);
    snapButton.setBounds (toolbar.removeFromRight (100));
    toolbar.removeFromRight (6);
    joinButton.setBounds (toolbar.removeFromRight (60));
    toolbar.removeFromRight (12);
    vibratoSlider.setBounds (toolbar.removeFromRight (150));
    vibratoLabel.setBounds (toolbar.removeFromRight (56));
    driftSlider.setBounds (toolbar.removeFromRight (150));
    driftLabel.setBounds (toolbar.removeFromRight (42));
    status.setBounds (area.removeFromBottom (24).reduced (6, 2));
    keyboard = area.removeFromLeft (keyboardWidth);
    grid = area;
    if (pixelsPerSecond * duration < grid.getWidth())
        pixelsPerSecond = juce::jmax (20.0, (double)grid.getWidth() / duration);
}

void SpinusTuneEditor::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));

    // Rows and keys.
    for (auto pitch = (int)lowestPitch; pitchToY ((float)pitch) > (float)grid.getY() - rowHeight; ++pitch)
    {
        const auto y = pitchToY ((float)pitch) - rowHeight * 0.5f;
        const bool black = juce::String ("01010010101").substring (pitch % 12, pitch % 12 + 1) == "1";
        g.setColour (black ? theme.get ("background") : theme.get ("panel"));
        g.fillRect (juce::Rectangle<float> ((float)grid.getX(), y, (float)grid.getWidth(), rowHeight));
        g.setColour (black ? juce::Colour (0xff1a1a1a) : juce::Colour (0xffe8e8e8));
        g.fillRect (
            juce::Rectangle<float> ((float)keyboard.getX(), y, (float)keyboard.getWidth() - 1.0f, rowHeight - 1.0f));
        if (pitch % 12 == 0)
        {
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (noteName (pitch), keyboard.withY ((int)y).withHeight ((int)rowHeight).reduced (3, 0),
                        juce::Justification::centredRight);
        }
    }

    juce::Graphics::ScopedSaveState clipToGrid (g);
    g.reduceClipRegion (grid);

    // The pitch as sung (grey) and as it will sound (yellow).
    if (track)
    {
        const auto& midi = *track;
        engine::PitchTrack pitchTrack;
        pitchTrack.frameSeconds = frameSeconds;
        for (auto m : midi)
            pitchTrack.hz.push_back (m > 0.0f ? 440.0f * std::pow (2.0f, (m - 69.0f) / 12.0f) : 0.0f);
        const auto corrections = engine::TuneRender::corrections (pitchTrack, notes, duration);

        juce::Path sung, sounds;
        bool inSung = false, inSounds = false;
        for (size_t i = 0; i < midi.size(); ++i)
        {
            const auto t = (double)i * frameSeconds;
            const auto x = timeToX (t);
            if (x < (float)grid.getX() - 4.0f || x > (float)grid.getRight() + 4.0f || midi[i] <= 0.0f)
            {
                inSung = inSounds = false;
                continue;
            }
            const auto corrected = midi[i] + (i < corrections.size() ? corrections[i] : 0.0f);
            if (! inSung)
                sung.startNewSubPath (x, pitchToY (midi[i]));
            else
                sung.lineTo (x, pitchToY (midi[i]));
            if (! inSounds)
                sounds.startNewSubPath (x, pitchToY (corrected));
            else
                sounds.lineTo (x, pitchToY (corrected));
            inSung = inSounds = true;
        }
        g.setColour (theme.get ("textDim").withAlpha (0.7f));
        g.strokePath (sung, juce::PathStrokeType (1.2f));

        for (int i = 0; i < (int)notes.size(); ++i)
        {
            const auto r = noteBounds (notes[(size_t)i]);
            g.setColour (theme.get ("accentAlt").withAlpha (selected.contains (i) ? 0.7f : 0.4f));
            g.fillRoundedRectangle (r, rowHeight * 0.4f);
            g.setColour (selected.contains (i) ? theme.get ("text") : theme.get ("accentAlt"));
            g.drawRoundedRectangle (r, rowHeight * 0.4f, selected.contains (i) ? 2.0f : 1.0f);
        }

        g.setColour (theme.get ("accent"));
        g.strokePath (sounds, juce::PathStrokeType (1.8f));
    }
    else
    {
        g.setColour (theme.get ("textDim"));
        g.drawFittedText (getStatusText(), grid.reduced (20), juce::Justification::centred, 3);
    }
}

void SpinusTuneEditor::timerCallback()
{
    const auto text = getStatusText();
    if (status.getText() != text)
        status.setText (text, juce::dontSendNotification);
}

void SpinusTuneEditor::changeListenerCallback (juce::ChangeBroadcaster*) { timerCallback(); }

//==============================================================================
void SpinusTuneEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = {};
    const auto i = noteAt (e.position);
    if (i < 0)
    {
        select ({});
        return;
    }
    if (e.mods.isShiftDown())
    {
        auto chosen = getSelection();
        chosen.push_back (i);
        select (chosen);
    }
    else if (! selected.contains (i))
        select ({ i });
    drag = { true, e.position.y, 0.0f };
}

void SpinusTuneEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag.active)
        return;
    drag.semitones = (drag.startY - e.position.y) / rowHeight;
    repaint();
}

void SpinusTuneEditor::mouseUp (const juce::MouseEvent& e)
{
    if (drag.active && std::abs (drag.semitones) > 0.05f)
        moveNotes (getSelection(), drag.semitones, ! e.mods.isAltDown());
    drag = {};
}

void SpinusTuneEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto i = noteAt (e.position); i >= 0)
        splitNote (i, xToTime (e.position.x));
}

void SpinusTuneEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
        pixelsPerSecond = juce::jlimit (20.0, 2000.0, pixelsPerSecond * (wheel.deltaY > 0 ? 1.15 : 1.0 / 1.15));
    else if (e.mods.isShiftDown())
        viewStart =
            juce::jlimit (0.0, juce::jmax (0.0, duration - 0.5), viewStart - wheel.deltaY * 200.0 / pixelsPerSecond);
    else
        lowestPitch = juce::jlimit (0.0f, 110.0f, lowestPitch + (wheel.deltaY > 0 ? 1.0f : -1.0f));
    repaint();
}

bool SpinusTuneEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getTextCharacter() == 'j' || key.getTextCharacter() == 'J')
    {
        const auto chosen = getSelection();
        return ! chosen.empty() && joinNotes (chosen.front(), chosen.front() + 1);
    }
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        return moveNotes (getSelection(), key == juce::KeyPress::upKey ? 1.0f : -1.0f);
    return false;
}

//==============================================================================
SpinusTuneWindow::SpinusTuneWindow (Session& session, model::ClipId clip, ai::AiService* service)
    : juce::DocumentWindow ("Spinus Tune", juce::Colours::black, juce::DocumentWindow::allButtons)
{
    auto content = std::make_unique<SpinusTuneEditor> (session, clip, service);
    editor = content.get();
    content->setSize (900, 480);
    setContentOwned (content.release(), true);
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    centreWithSize (getWidth(), getHeight());
    shortcuts::addTo (*this);
}

void SpinusTuneWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace spinus::ui
