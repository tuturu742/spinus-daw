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

constexpr int keyboardWidth = 52;
constexpr int bottomBarHeight = 28;

/** The melody channel picked for each clip while Spinus runs. */
std::map<juce::uint32, juce::uint32>& melodyChoices()
{
    static std::map<juce::uint32, juce::uint32> choices;
    return choices;
}
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
    zoomControls.onZoom = [this] (double factor) { zoom (factor); };
    zoomControls.onFit = [this] { fit(); };
    addAndMakeVisible (zoomControls);

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
    for (auto [slider, label, text, low, high, unit] :
         { std::tuple { &volumeSlider, &volumeLabel, "Volume", -24.0, 12.0, " dB" },
           std::tuple { &formantSlider, &formantLabel, "Formant", -12.0, 12.0, " st" } })
    {
        slider->setRange (low, high, 0.1);
        slider->setValue (0.0, juce::dontSendNotification);
        slider->setDoubleClickReturnValue (true, 0.0);
        slider->setTextValueSuffix (unit);
        slider->setSliderStyle (juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 20);
        label->setText (text, juce::dontSendNotification);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
    }
    volumeSlider.setTooltip ("How loud the selected notes (or all) are, in dB: 0 = as sung. Double-click: back to 0");
    formantSlider.setTooltip ("The voice's character in the selected notes (or all), in semitones: up sounds "
                              "smaller and brighter, down bigger and darker; 0 = as sung. Double-click: back to 0");
    volumeSlider.onDragEnd = [this] { setVolume (selectedOrAll(), (float)volumeSlider.getValue()); };
    formantSlider.onDragEnd = [this] { setFormant (selectedOrAll(), (float)formantSlider.getValue()); };
    // A double-click (back to 0) or typing a value is not a drag.
    volumeSlider.onValueChange = [this]
    {
        if (! volumeSlider.isMouseButtonDown())
            setVolume (selectedOrAll(), (float)volumeSlider.getValue());
    };
    formantSlider.onValueChange = [this]
    {
        if (! formantSlider.isMouseButtonDown())
            setFormant (selectedOrAll(), (float)formantSlider.getValue());
    };
    addAndMakeVisible (status);

    melodyBox.setTooltip ("A channel whose notes are the melody the vocal should sing: shown as ghosts behind the "
                          "sung notes, and used by Match melody");
    melodyBox.onChange = [this]
    {
        const auto id = melodyBox.getSelectedId();
        setMelodyChannel (model::ChannelId { id > 1 ? (juce::uint32)(id - 1) : 0u });
    };
    matchButton.setTooltip ("Move the selected notes (or all) onto the melody's notes; with Timing too, also onto "
                            "where they start and how long they are");
    matchButton.onClick = [this] { matchToMelody (selectedOrAll(), timingToggle.getToggleState()); };
    timingToggle.setToggleState (true, juce::dontSendNotification);
    timingToggle.setTooltip ("Match melody also moves notes in time and changes their length to the melody's");
    for (auto* control : std::initializer_list<juce::Component*> { &melodyBox, &matchButton, &timingToggle })
        addAndMakeVisible (control);
    if (auto it = melodyChoices().find (clip.value); it != melodyChoices().end())
        melodyChannel = model::ChannelId { it->second };
    else if (const auto* vocal = session.getProject().findClip (clip))
        if (const auto* audio = std::get_if<model::AudioClip> (&vocal->content))
            if (const auto* channel = session.getProject().findChannel (audio->channel))
                if (const auto* insert = session.getProject().findInsert (channel->targetInsert))
                    melodyChannel = insert->notesFrom;

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

    notes = session.getTuner().getNotes (clip);
    if (session.getProject().findChannel (melodyChannel) == nullptr)
        melodyChannel = {};
    melody = MelodyMatch::melodyFor (session.getProject(), clip, melodyChannel);
    fillMelodyBox();

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

void SpinusTuneEditor::fillMelodyBox()
{
    melodyBox.clear (juce::dontSendNotification);
    melodyBox.addItem ("Melody: none", 1);
    for (auto& c : session.getProject().channels)
        if (! c.audioTrack)
            melodyBox.addItem ("Melody: " + c.name, (int)c.id.value + 1);
    melodyBox.setSelectedId (melodyChannel ? (int)melodyChannel.value + 1 : 1, juce::dontSendNotification);
    matchButton.setEnabled (! melody.empty() && ! notes.empty());
}

void SpinusTuneEditor::setMelodyChannel (model::ChannelId channel)
{
    melodyChannel = session.getProject().findChannel (channel) != nullptr ? channel : model::ChannelId {};
    melodyChoices()[clip.value] = melodyChannel.value;
    notice.clear();
    load();
}

bool SpinusTuneEditor::matchToMelody (const std::vector<int>& indices, bool timing)
{
    const auto result = MelodyMatch::match (notes, melody, timing, indices, duration);
    if (result.matched == 0)
    {
        notice = melody.empty() ? "Pick a melody channel first (the menu at the bottom left)."
                                : "No sung note overlaps a melody note.";
        timerCallback();
        return false;
    }
    const auto changed = commit (result.notes, "Match to melody");
    notice = "Matched " + juce::String (result.matched) + (result.matched == 1 ? " note" : " notes") +
             (timing ? " (" + juce::String (result.timed) + " in time)" : juce::String()) +
             (result.unmatched.empty() ? juce::String (".")
                                       : "; " + juce::String ((int)result.unmatched.size()) +
                                             " left as sung (no melody note clearly theirs).");
    timerCallback();
    return changed;
}

juce::String SpinusTuneEditor::getStatusText() const
{
    if (problem.isNotEmpty())
        return problem;
    if (! track)
        return "Listening to the vocal...";
    const auto withNotice = [this] (const juce::String& text) { return notice.isEmpty() ? text : notice + " " + text; };
    switch (const auto s = session.getTuner().getStatus (clip); s.state)
    {
    case Tuner::State::rendering:
        return withNotice ("Rendering the tuned vocal... " + juce::String (juce::roundToInt (s.progress * 100.0f)) +
                           "%");
    case Tuner::State::failed:
        return s.message;
    case Tuner::State::ready:
        return withNotice ("Tuned. Play the song to hear it.");
    case Tuner::State::needsAnalysis:
    case Tuner::State::none:
        break;
    }
    return "Drag a note up or down (Alt: freely) or sideways to move it, or by an edge to change its length; "
           "double-click to split; select notes for Drift, Vibrato, Volume and Formant.";
}

//==============================================================================
bool SpinusTuneEditor::commit (const std::vector<model::TuneNote>& changed, const juce::String& description)
{
    notice.clear();
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

std::vector<model::TuneNote> SpinusTuneEditor::withPitchMoved (std::vector<model::TuneNote> changed,
                                                               const std::vector<int>& indices, float semitones,
                                                               bool snap) const
{
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
        {
            auto& n = changed[(size_t)i];
            auto target = n.pitch + n.shift + semitones;
            if (snap)
                target = (float)juce::roundToInt (target);
            n.shift = juce::jlimit (-24.0f, 24.0f, target - n.pitch);
        }
    return changed;
}

std::vector<model::TuneNote> SpinusTuneEditor::withTimeMoved (std::vector<model::TuneNote> changed,
                                                              const std::vector<int>& indices, double seconds) const
{
    // As far as the nearest notes left in place, and the clip's ends, allow.
    const std::set<int> chosen (indices.begin(), indices.end());
    const auto count = (int)changed.size();
    auto low = -duration, high = duration;
    for (auto i : chosen)
    {
        if (! juce::isPositiveAndBelow (i, count))
            continue;
        const auto& n = changed[(size_t)i];
        if (! chosen.contains (i - 1))
            low = juce::jmax (low, (i > 0 ? changed[(size_t)i - 1].outputEnd() : 0.0) - n.outputStart());
        if (! chosen.contains (i + 1))
            high = juce::jmin (high, (i + 1 < count ? changed[(size_t)i + 1].outputStart() : duration) - n.outputEnd());
    }
    if (low > high)
        return changed;
    const auto by = juce::jlimit (juce::jmin (0.0, low), juce::jmax (0.0, high), seconds);
    for (auto i : chosen)
        if (juce::isPositiveAndBelow (i, count))
        {
            auto& n = changed[(size_t)i];
            n.move += by;
            if (std::abs (n.move) < 0.0005) // back where it was sung
                n.move = 0.0;
        }
    return changed;
}

std::vector<model::TuneNote> SpinusTuneEditor::withResized (std::vector<model::TuneNote> changed, int index,
                                                            double outputStart, double outputEnd) const
{
    if (! juce::isPositiveAndBelow (index, (int)changed.size()))
        return changed;
    auto& n = changed[(size_t)index];
    const auto movesStart = std::abs (outputStart - n.outputStart()) > 1.0e-9;
    // Within its neighbours and the clip, and 0.25 to 4 times as long as sung.
    outputStart = juce::jmax (outputStart, index > 0 ? changed[(size_t)index - 1].outputEnd() : 0.0);
    outputEnd =
        juce::jmin (outputEnd, index + 1 < (int)changed.size() ? changed[(size_t)index + 1].outputStart() : duration);
    const auto length = n.end - n.start;
    auto stretch = juce::jlimit (0.25, 4.0, (outputEnd - outputStart) / length);
    if (std::abs (stretch - 1.0) < 0.01) // as long as sung
        stretch = 1.0;
    if (movesStart)
        outputStart = outputEnd - length * stretch;
    n.stretch = stretch;
    n.move = outputStart - n.start;
    if (std::abs (n.move) < 0.0005)
        n.move = 0.0;
    return changed;
}

bool SpinusTuneEditor::moveNotes (const std::vector<int>& indices, float semitones, bool snap)
{
    return commit (withPitchMoved (notes, indices, semitones, snap), indices.size() > 1 ? "Move notes" : "Move note");
}

bool SpinusTuneEditor::moveInTime (const std::vector<int>& indices, double seconds)
{
    return commit (withTimeMoved (notes, indices, seconds),
                   indices.size() > 1 ? "Move notes in time" : "Move note in time");
}

bool SpinusTuneEditor::resizeNote (int index, double outputStart, double outputEnd)
{
    return commit (withResized (notes, index, outputStart, outputEnd), "Change note length");
}

bool SpinusTuneEditor::splitNote (int index, double seconds)
{
    if (! juce::isPositiveAndBelow (index, (int)notes.size()))
        return false;
    const auto& n = notes[(size_t)index];
    // `seconds` is where it sounds: the moment of the file sung there.
    const auto at = n.start + (seconds - n.outputStart()) / n.stretch;
    if (at <= n.start + 0.02 || at >= n.end - 0.02)
        return false;
    auto changed = notes;
    auto second = n;
    changed[(size_t)index].end = at;
    second.start = at;
    second.move = n.outputStart() + (at - n.start) * n.stretch - at;
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
    // From where the first starts to where the second ends, as they sound.
    if (a.isMovedInTime() || b.isMovedInTime())
        a.stretch = juce::jlimit (0.25, 4.0, (b.outputEnd() - a.outputStart()) / (b.end - a.start));
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

bool SpinusTuneEditor::setVolume (const std::vector<int>& indices, float gainDb)
{
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
            changed[(size_t)i].gainDb = juce::jlimit (-24.0f, 12.0f, gainDb);
    return commit (changed, "Change note volume");
}

bool SpinusTuneEditor::setFormant (const std::vector<int>& indices, float semitones)
{
    auto changed = notes;
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)changed.size()))
            changed[(size_t)i].formant = juce::jlimit (-12.0f, 12.0f, semitones);
    return commit (changed, "Change formant");
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
            n.gainDb = 0.0f;
            n.formant = 0.0f;
            n.move = 0.0;
            n.stretch = 1.0;
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
        volumeSlider.setValue (n.gainDb, juce::dontSendNotification);
        formantSlider.setValue (n.formant, juce::dontSendNotification);
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
    return juce::Rectangle<float>::leftTopRightBottom (timeToX (n.outputStart()), y - rowHeight * 0.45f,
                                                       timeToX (n.outputEnd()), y + rowHeight * 0.45f);
}

int SpinusTuneEditor::edgeAt (int index, juce::Point<float> p) const
{
    if (! juce::isPositiveAndBelow (index, (int)notes.size()))
        return 0;
    const auto r = noteBounds (notes[(size_t)index]);
    const auto grip = juce::jmin (5.0f, r.getWidth() / 4.0f);
    if (p.x < r.getX() + grip)
        return -1;
    if (p.x > r.getRight() - grip)
        return 1;
    return 0;
}

std::vector<model::TuneNote> SpinusTuneEditor::shownNotes() const
{
    if (! drag.active)
        return notes;
    if (drag.edge != 0 && juce::isPositiveAndBelow (drag.note, (int)notes.size()))
    {
        const auto& n = notes[(size_t)drag.note];
        return withResized (notes, drag.note, n.outputStart() + (drag.edge < 0 ? drag.seconds : 0.0),
                            n.outputEnd() + (drag.edge > 0 ? drag.seconds : 0.0));
    }
    return withPitchMoved (withTimeMoved (notes, getSelection(), drag.seconds), getSelection(), drag.semitones, false);
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
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (4, 3);
    resetButton.setBounds (toolbar.removeFromRight (70));
    toolbar.removeFromRight (6);
    snapButton.setBounds (toolbar.removeFromRight (100));
    toolbar.removeFromRight (6);
    joinButton.setBounds (toolbar.removeFromRight (60));
    toolbar.removeFromRight (12);
    for (auto [slider, label, labelWidth] :
         { std::tuple { &formantSlider, &formantLabel, 58 }, std::tuple { &volumeSlider, &volumeLabel, 54 },
           std::tuple { &vibratoSlider, &vibratoLabel, 56 }, std::tuple { &driftSlider, &driftLabel, 42 } })
    {
        slider->setBounds (toolbar.removeFromRight (130));
        label->setBounds (toolbar.removeFromRight (labelWidth));
    }
    zoomControls.setBounds (toolbar.removeFromLeft (zoomControls.getPreferredWidth()));
    auto bottom = area.removeFromBottom (bottomBarHeight).reduced (4, 3);
    melodyBox.setBounds (bottom.removeFromLeft (190));
    bottom.removeFromLeft (6);
    matchButton.setBounds (bottom.removeFromLeft (110));
    bottom.removeFromLeft (6);
    timingToggle.setBounds (bottom.removeFromLeft (100));
    status.setBounds (bottom.reduced (6, 0));
    keyboard = area.removeFromLeft (keyboardWidth);
    grid = area;
    if (pixelsPerSecond * duration < grid.getWidth())
        pixelsPerSecond = juce::jmax (20.0, (double)grid.getWidth() / duration);
}

void SpinusTuneEditor::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    // The toolbar's own strip, as in the Piano Roll and the Playlist.
    g.setColour (theme.get ("panelHeader"));
    g.fillRect (getLocalBounds().removeFromTop (toolbarHeight));

    // Rows and keys, kept out of the toolbar and the status line (the top and bottom rows are only partly in view).
    juce::Graphics::ScopedSaveState clipToRows (g);
    g.reduceClipRegion (keyboard.getUnion (grid));
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
        const auto shown = shownNotes();
        const auto corrections = engine::TuneRender::corrections (pitchTrack, shown, duration);
        const auto timing = engine::TimeMap::of (shown, duration);

        juce::Path sung, sounds;
        bool inSung = false, inSounds = false;
        for (size_t i = 0; i < midi.size(); ++i)
        {
            const auto x = timeToX (timing.toOutput ((double)i * frameSeconds));
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

        // The melody's notes as ghosts, in the octave the vocal is sung in.
        if (! melody.empty() && ! notes.empty())
        {
            double sungPitch = 0.0, writtenPitch = 0.0;
            for (auto& n : notes)
                sungPitch += n.pitch;
            for (auto& m : melody)
                writtenPitch += m.pitch;
            const auto octave =
                12.0f *
                (float)std::round ((sungPitch / (double)notes.size() - writtenPitch / (double)melody.size()) / 12.0);
            for (auto& m : melody)
            {
                const auto y = pitchToY ((float)m.pitch + octave);
                const auto r = juce::Rectangle<float>::leftTopRightBottom (timeToX (m.start), y - rowHeight * 0.5f,
                                                                           timeToX (m.end), y + rowHeight * 0.5f);
                g.setColour (theme.get ("text").withAlpha (0.08f));
                g.fillRect (r);
                g.setColour (theme.get ("text").withAlpha (0.35f));
                g.drawRect (r, 1.0f);
            }
        }

        for (int i = 0; i < (int)shown.size(); ++i)
        {
            const auto r = noteBounds (shown[(size_t)i]);
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
void SpinusTuneEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto i = noteAt (e.position);
    setMouseCursor (i >= 0 && edgeAt (i, e.position) != 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                          : juce::MouseCursor::NormalCursor);
}

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
    const auto edge = e.mods.isShiftDown() ? 0 : edgeAt (i, e.position);
    if (edge != 0)
        select ({ i });
    drag = { true, e.position, 0.0f, 0.0, i, edge };
}

void SpinusTuneEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag.active)
        return;
    // A few pixels before it moves, so a drag up or down stays in time and one sideways on its note.
    const auto offset = e.position - drag.start;
    drag.seconds = drag.edge != 0 || std::abs (offset.x) >= 4.0f ? offset.x / pixelsPerSecond : 0.0;
    drag.semitones = drag.edge == 0 && std::abs (offset.y) >= 3.0f ? -offset.y / rowHeight : 0.0f;
    repaint();
}

void SpinusTuneEditor::mouseUp (const juce::MouseEvent& e)
{
    const auto finished = drag;
    drag = {};
    if (! finished.active)
        return;
    if (finished.edge != 0 && juce::isPositiveAndBelow (finished.note, (int)notes.size()))
    {
        const auto& n = notes[(size_t)finished.note];
        if (! juce::exactlyEqual (finished.seconds, 0.0))
            resizeNote (finished.note, n.outputStart() + (finished.edge < 0 ? finished.seconds : 0.0),
                        n.outputEnd() + (finished.edge > 0 ? finished.seconds : 0.0));
        repaint();
        return;
    }
    // Up or down and sideways at once: one undo step.
    const auto chosen = getSelection();
    const auto inTime = withTimeMoved (notes, chosen, finished.seconds);
    const auto upOrDown = std::abs (finished.semitones) > 0.05f;
    const auto plural = chosen.size() > 1;
    if (! commit (upOrDown ? withPitchMoved (inTime, chosen, finished.semitones, ! e.mods.isAltDown()) : inTime,
                  upOrDown ? (plural ? "Move notes" : "Move note")
                           : (plural ? "Move notes in time" : "Move note in time")))
        repaint();
}

void SpinusTuneEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto i = noteAt (e.position); i >= 0)
        splitNote (i, xToTime (e.position.x));
}

void SpinusTuneEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown() && ! e.mods.isShiftDown())
    {
        zoom (timeline::wheelZoomFactor (wheel), e.position.x - (float)grid.getX());
        return;
    }
    if (e.mods.isCtrlDown())
        rowHeight = juce::jlimit (6.0f, 40.0f, rowHeight * (float)timeline::wheelZoomFactor (wheel));
    else if (e.mods.isShiftDown())
        viewStart =
            juce::jlimit (0.0, juce::jmax (0.0, duration - 0.5), viewStart - wheel.deltaY * 200.0 / pixelsPerSecond);
    else
        lowestPitch = juce::jlimit (0.0f, 110.0f, lowestPitch + (wheel.deltaY > 0 ? 1.0f : -1.0f));
    repaint();
}

void SpinusTuneEditor::mouseMagnify (const juce::MouseEvent& e, float scale)
{
    zoom (scale, e.position.x - (float)grid.getX());
}

void SpinusTuneEditor::zoom (double factor, std::optional<float> x)
{
    // Not smaller than the whole clip across the grid.
    const auto fitting = juce::jmax (20.0, (double)grid.getWidth() / juce::jmax (0.01, duration));
    const auto at = (double)x.value_or ((float)grid.getWidth() * 0.5f);
    const auto time = viewStart + at / pixelsPerSecond;
    pixelsPerSecond = juce::jlimit (juce::jmin (fitting, 2000.0), 2000.0, pixelsPerSecond * factor);
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, duration - 0.5), time - at / pixelsPerSecond);
    repaint();
}

void SpinusTuneEditor::fit()
{
    pixelsPerSecond = juce::jlimit (20.0, 2000.0, (double)grid.getWidth() / juce::jmax (0.01, duration));
    viewStart = 0.0;
    repaint();
}

bool SpinusTuneEditor::keyPressed (const juce::KeyPress& key)
{
    switch (timeline::zoomKeyOf (key))
    {
    case timeline::ZoomKey::in:
        zoom (timeline::zoomStep);
        return true;
    case timeline::ZoomKey::out:
        zoom (1.0 / timeline::zoomStep);
        return true;
    case timeline::ZoomKey::fit:
        fit();
        return true;
    case timeline::ZoomKey::none:
        break;
    }
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
    content->setSize (1180, 520);
    setContentOwned (content.release(), true);
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    setResizeLimits (SpinusTuneEditor::minimumWidth, 320, 10000, 10000);
    centreWithSize (getWidth(), getHeight());
    shortcuts::addTo (*this);
}

void SpinusTuneWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace spinus::ui
