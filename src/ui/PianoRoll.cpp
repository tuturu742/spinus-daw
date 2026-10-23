#include "PianoRoll.h"
#include "model/Chords.h"
#include "model/NoteTools.h"
#include "engine/Beats.h"

#include "Theme.h"
#include "commands/CommandRegistry.h"
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

bool isBlackKey (int pitch)
{
    const auto n = pitch % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

/** FL's note names: MIDI 60 is C5. */
juce::String noteName (int pitch)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[pitch % 12]) + juce::String (pitch / 12);
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

bool byStartThenPitch (const model::Note& a, const model::Note& b)
{
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
}
} // namespace

//==============================================================================
class PianoRoll::Grid final : public juce::Component
{
public:
    explicit Grid (PianoRoll& r) : roll (r) { setWantsKeyboardFocus (true); }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto w = (float)getWidth(), h = (float)getHeight();

        // Key rows; with a scale highlighted, the rows outside it are darker and its root note tinted.
        const auto key = roll.getHighlightedKey();
        for (int pitch = 127; pitch >= 0; --pitch)
        {
            const auto y = roll.pitchToY (pitch);
            if (y > h || y + (float)roll.keyHeight < 0.0f)
                continue;
            const bool darker = key ? ! model::isInKey (*key, pitch) : isBlackKey (pitch);
            g.setColour (darker ? theme.get ("background") : theme.get ("panel"));
            g.fillRect (0.0f, y, w, (float)roll.keyHeight);
            if (key && darker)
            {
                g.setColour (juce::Colours::black.withAlpha (0.18f));
                g.fillRect (0.0f, y, w, (float)roll.keyHeight);
            }
            if (key && pitch % 12 == key->tonic)
            {
                g.setColour (theme.get ("accent").withAlpha (0.12f));
                g.fillRect (0.0f, y, w, (float)roll.keyHeight);
            }
            if (pitch % 12 == 0)
            {
                g.setColour (theme.get ("border"));
                g.drawHorizontalLine ((int)(y + (float)roll.keyHeight) - 1, 0.0f, w);
            }
        }

        // Beyond the pattern's end.
        const auto endX = roll.tickToX (roll.getPatternLength());
        if (endX < w)
        {
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.fillRect (juce::jmax (0.0f, endX), 0.0f, w - endX, h);
        }

        // Grid lines: snap (faint), beats, bars (strong).
        const auto sig = roll.getTimeSignature();
        // Faint lines at the snap when it is finer than a beat; with no snap, at the beats.
        const auto lineStep = roll.snap > 1 && roll.snap < sig.beatTicks() ? roll.snap : sig.beatTicks();
        const auto first = (roll.xToTick (0.0f) / lineStep) * lineStep;
        for (auto t = first; roll.tickToX (t) < w; t += lineStep)
        {
            const auto x = roll.tickToX (t);
            const bool bar = t % sig.barTicks() == 0;
            const bool beat = t % sig.beatTicks() == 0;
            g.setColour (theme.get ("border").withAlpha (bar ? 1.0f : beat ? 0.6f : 0.25f));
            g.drawVerticalLine ((int)x, 0.0f, h);
        }

        // The loop region (right-drag on the timeline), faintly across the notes.
        if (const auto region = roll.getShownLoop())
        {
            const auto a = roll.tickToX (region->start), b = roll.tickToX (region->end);
            g.setColour (theme.get ("accentAlt").withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (a, 0.0f, b - a, h));
            g.setColour (theme.get ("accentAlt").withAlpha (0.6f));
            g.drawVerticalLine ((int)a, 0.0f, h);
            g.drawVerticalLine ((int)b, 0.0f, h);
        }

        // Ghost notes: other channels' notes, faint, behind.
        for (auto& ghost : roll.ghosts)
            for (auto& n : ghost.notes)
            {
                const auto r = noteBounds (n);
                if (! r.intersects (getLocalBounds().toFloat()))
                    continue;
                g.setColour (ghost.colour.withAlpha (0.3f));
                g.fillRoundedRectangle (r, 2.0f);
                g.setColour (ghost.colour.withAlpha (0.55f));
                g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
            }

        // Notes; dimmed while a suggestion for them is shown.
        const auto* suggested = roll.getSuggestedNotes();
        for (size_t i = 0; i < roll.notes.size(); ++i)
        {
            const auto& n = roll.notes[i];
            const auto r = noteBounds (n);
            if (! r.intersects (getLocalBounds().toFloat()))
                continue;
            const bool erasing = mode == Mode::erase && erased.contains ((int)i);
            const bool selected = roll.isSelected ((int)i) && mode != Mode::paint;
            g.setColour (theme.get ("accent")
                             .withMultipliedBrightness ((0.55f + 0.45f * n.velocity) * (selected ? 1.3f : 1.0f))
                             .withMultipliedAlpha (suggested != nullptr || erasing ? 0.3f : 1.0f));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (selected ? theme.get ("text") : theme.get ("border"));
            g.drawRoundedRectangle (r, 2.0f, selected ? 1.6f : 1.0f);
            if (r.getWidth() > 26.0f)
            {
                g.setColour (theme.get ("background"));
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (noteName (n.pitch), r.reduced (3.0f, 0.0f), juce::Justification::centredLeft, false);
            }
        }

        // Chord names: above notes that start together and make a chord.
        {
            std::map<model::Ticks, std::vector<int>> together;
            for (auto& n : roll.notes)
                together[n.start].push_back (n.pitch);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            for (auto& [start, chord] : together)
            {
                const auto name = model::nameChord (chord);
                if (name.isEmpty())
                    continue;
                const auto top = *std::max_element (chord.begin(), chord.end());
                const auto label =
                    juce::Rectangle<float> (roll.tickToX (start), roll.pitchToY (top) - 15.0f, 70.0f, 14.0f);
                if (! label.intersects (getLocalBounds().toFloat()))
                    continue;
                g.setColour (theme.get ("text"));
                g.drawText (name, label, juce::Justification::bottomLeft, false);
            }
        }

        // The suggestion on top: translucent with a dashed outline.
        if (suggested != nullptr)
            for (auto& n : *suggested)
            {
                const auto r = noteBounds (n);
                if (! r.intersects (getLocalBounds().toFloat()))
                    continue;
                g.setColour (theme.get ("accentAlt").withAlpha (0.45f));
                g.fillRoundedRectangle (r, 2.0f);
                juce::Path outline, dashed;
                outline.addRoundedRectangle (r.reduced (0.5f), 2.0f);
                const float dashes[] = { 3.0f, 2.0f };
                juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
                g.setColour (theme.get ("accentAlt").brighter (0.4f));
                g.fillPath (dashed);
            }

        if (roll.playheadTicks >= 0.0)
        {
            g.setColour (theme.get ("text"));
            g.drawVerticalLine ((int)roll.tickToX ((model::Ticks)roll.playheadTicks), 0.0f, h);
        }

        if (mode == Mode::selectBox)
        {
            const auto box = juce::Rectangle<float> (startPosition, currentPosition);
            g.setColour (theme.get ("accentAlt").withAlpha (0.15f));
            g.fillRect (box);
            g.setColour (theme.get ("accentAlt"));
            g.drawRect (box, 1.0f);
        }
        else if (mode == Mode::slice)
        {
            const auto x = roll.tickToX (sliceTick (currentModifiers));
            g.setColour (theme.get ("danger"));
            g.drawLine (x, juce::jmin (startPosition.y, currentPosition.y) - 6.0f, x,
                        juce::jmax (startPosition.y, currentPosition.y) + 6.0f, 2.0f);
        }
    }

    juce::Rectangle<float> noteBounds (const model::Note& n) const
    {
        const auto x = roll.tickToX (n.start);
        return { x, roll.pitchToY (n.pitch) + 1.0f, juce::jmax (3.0f, roll.tickToX (n.start + n.length) - x - 1.0f),
                 (float)roll.keyHeight - 2.0f };
    }

    /** The channel of the shown ghost note under `p`, if any. */
    std::optional<model::ChannelId> ghostAt (juce::Point<float> p) const
    {
        if (! roll.areGhostsShown())
            return std::nullopt;
        for (auto& ghost : roll.ghosts)
            if (roll.isGhostChannelShown (ghost.channel))
                for (auto& n : ghost.notes)
                    if (noteBounds (n).contains (p))
                        return ghost.channel;
        return std::nullopt;
    }

    int noteAt (juce::Point<float> p) const
    {
        for (int i = (int)roll.notes.size(); --i >= 0;)
            if (noteBounds (roll.notes[(size_t)i]).contains (p))
                return i;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto i = noteAt (e.position);
        const bool edge = i >= 0 && noteBounds (roll.notes[(size_t)i]).getRight() - e.position.x < 6.0f;
        const auto tool = roll.getTool();
        setMouseCursor (edge                   ? juce::MouseCursor::LeftRightResizeCursor
                        : i >= 0               ? juce::MouseCursor::DraggingHandCursor
                        : tool == Tool::slice  ? juce::MouseCursor::IBeamCursor
                        : tool == Tool::select ? juce::MouseCursor::CrosshairCursor
                                               : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        mode = Mode::none;
        before = roll.notes;
        startPosition = currentPosition = e.position;
        currentModifiers = e.mods;
        const auto i = noteAt (e.position);
        const auto tool = roll.getTool();

        // Double-click on another channel's (ghost) note: edit that channel. The first click added a note
        // here: it goes again.
        if (e.getNumberOfClicks() == 2 && ! e.mods.isPopupMenu())
            if (const auto ghost = ghostAt (e.position))
            {
                auto& history = roll.session.getHistory();
                if (std::exchange (justAdded, false) &&
                    history.getUndoDescriptions().strings.getLast().startsWith ("Add "))
                    history.undo();
                roll.session.setSelectedChannel (*ghost);
                return;
            }
        justAdded = false;

        // Right button: delete every note the drag touches.
        if (e.mods.isPopupMenu())
        {
            mode = Mode::erase;
            erased.clear();
            if (i >= 0)
                erased.insert (i);
            repaint();
            return;
        }

        // Ctrl+drag (or the Select tool on empty space): a selection box; Shift keeps what is selected.
        if (e.mods.isCtrlDown() || (tool == Tool::select && i < 0))
        {
            mode = Mode::selectBox;
            return;
        }

        // Shift+click: a note in or out of the selection.
        if (e.mods.isShiftDown() && i >= 0)
        {
            auto selection = roll.getSelection();
            if (! selection.erase (i))
                selection.insert (i);
            roll.setSelection (selection);
            return;
        }

        if (! roll.session.getSelectedChannel().isValid() || ! roll.ensurePattern())
            return;
        before = roll.notes;

        if (tool == Tool::slice)
        {
            mode = Mode::slice;
            repaint();
            return;
        }

        if (i < 0)
        {
            roll.clearSelection();
            if (tool == Tool::paint)
            {
                mode = Mode::paint;
                pitches = { roll.yToPitch (e.position.y) };
                updatePaint();
                return;
            }
            const auto start =
                e.mods.isAltDown() ? roll.xToTick (e.position.x) : roll.snapTicks (roll.xToTick (e.position.x));
            // One note, or the chord stamp's chord on it: they move together while the mouse is down.
            const auto stamp = roll.stampPitches (roll.yToPitch (e.position.y));
            indices.clear();
            for (auto pitch : stamp)
            {
                roll.notes.push_back ({ juce::jmax<model::Ticks> (0, start),
                                        roll.lastLength,
                                        pitch,
                                        roll.lastVelocity,
                                        0.5f,
                                        0.0f,
                                        0.0f,
                                        {} });
                indices.push_back ((int)roll.notes.size() - 1);
            }
            anchor = 0;
            mode = Mode::move;
            description = stamp.size() > 1 ? "Add chord" : "Add note";
        }
        else
        {
            // A selected note takes the whole selection with it; another note goes alone.
            if (roll.isSelected (i))
                indices.assign (roll.getSelection().begin(), roll.getSelection().end());
            else
            {
                roll.clearSelection();
                indices = { i };
            }
            anchor = (int)std::distance (indices.begin(), std::find (indices.begin(), indices.end(), i));
            const bool edge = noteBounds (roll.notes[(size_t)i]).getRight() - e.position.x < 6.0f;
            mode = edge ? Mode::resize : Mode::move;
            const bool several = indices.size() > 1;
            description = edge ? (several ? "Resize notes" : "Resize note") : (several ? "Move notes" : "Move note");
        }

        originals.clear();
        for (auto index : indices)
            originals.push_back (roll.notes[(size_t)index]);
        // A note placed or picked up sounds while the mouse is down (FL).
        if (mode == Mode::move)
            previewHeldNotes();
        repaint();
    }

    void previewHeldNotes()
    {
        std::vector<int> held;
        held.reserve (indices.size());
        for (auto index : indices)
            held.push_back (roll.notes[(size_t)index].pitch);
        roll.previewPitches (held, originals.empty() ? roll.lastVelocity : originals[(size_t)anchor].velocity);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        currentPosition = e.position;
        currentModifiers = e.mods;
        switch (mode)
        {
        case Mode::none:
            return;
        case Mode::erase:
            if (const auto i = noteAt (e.position); i >= 0)
                erased.insert (i);
            break;
        case Mode::selectBox:
        case Mode::slice:
            break;
        case Mode::paint:
            if (const auto step = paintSteps(); step >= 0)
            {
                pitches.resize ((size_t)step + 1, pitches.back());
                pitches[(size_t)step] = roll.yToPitch (e.position.y);
            }
            updatePaint();
            break;
        case Mode::move:
        {
            const auto& lead = originals[(size_t)anchor];
            auto start = lead.start + roll.xToTick (e.position.x) - roll.xToTick (startPosition.x);
            start = e.mods.isAltDown() ? start : roll.snapTicks (start);
            auto deltaTicks = start - lead.start;
            auto deltaPitch = roll.yToPitch (e.position.y) - roll.yToPitch (startPosition.y);
            // The group stays inside the pattern's start and the 128 notes.
            model::Ticks earliest = std::numeric_limits<model::Ticks>::max();
            int lowest = 127, highest = 0;
            for (auto& n : originals)
            {
                earliest = juce::jmin (earliest, n.start);
                lowest = juce::jmin (lowest, n.pitch);
                highest = juce::jmax (highest, n.pitch);
            }
            deltaTicks = juce::jmax (deltaTicks, -earliest);
            deltaPitch = juce::jlimit (-lowest, 127 - highest, deltaPitch);
            for (size_t k = 0; k < indices.size(); ++k)
            {
                auto& n = roll.notes[(size_t)indices[k]];
                n.start = originals[k].start + deltaTicks;
                n.pitch = originals[k].pitch + deltaPitch;
            }
            previewHeldNotes(); // the new pitch, when it changed
            break;
        }
        case Mode::resize:
        {
            const auto& lead = originals[(size_t)anchor];
            const auto leadEnd = lead.start + lead.length;
            auto end = leadEnd + roll.xToTick (e.position.x) - roll.xToTick (startPosition.x);
            end = e.mods.isAltDown() ? end : roll.snapTicks (end);
            const auto minimum = e.mods.isAltDown() ? (model::Ticks)1 : roll.snap;
            for (size_t k = 0; k < indices.size(); ++k)
                roll.notes[(size_t)indices[k]].length =
                    juce::jmax<model::Ticks> (minimum, originals[k].length + end - leadEnd);
            break;
        }
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        currentPosition = e.position;
        roll.previewPitches ({});
        const auto finished = std::exchange (mode, Mode::none);
        switch (finished)
        {
        case Mode::none:
            return;
        case Mode::erase:
            if (! erased.empty())
                roll.deleteNotes (erased);
            erased.clear();
            break;
        case Mode::selectBox:
        {
            const auto a = roll.xToTick (juce::jmin (startPosition.x, currentPosition.x));
            const auto b = roll.xToTick (juce::jmax (startPosition.x, currentPosition.x));
            roll.selectArea (a, b, roll.yToPitch (juce::jmax (startPosition.y, currentPosition.y)),
                             roll.yToPitch (juce::jmin (startPosition.y, currentPosition.y)), e.mods.isShiftDown());
            break;
        }
        case Mode::slice:
            roll.slice (sliceTick (e.mods), roll.yToPitch (juce::jmax (startPosition.y, currentPosition.y)),
                        roll.yToPitch (juce::jmin (startPosition.y, currentPosition.y)));
            break;
        case Mode::paint:
        {
            const auto changed = roll.notes;
            roll.notes = before;
            if (! (changed == before))
                roll.commitKeepingSelection (changed, {}, "Paint notes");
            break;
        }
        case Mode::move:
        case Mode::resize:
        {
            const auto changed = roll.notes;
            const auto& edited = changed[(size_t)indices[(size_t)anchor]];
            std::vector<model::Note> selected;
            if (indices.size() > 1)
                for (auto index : indices)
                    selected.push_back (changed[(size_t)index]);
            roll.notes = before; // the command makes the change; the listener reloads
            if (description == "Add note" || ! (changed == before))
            {
                if (! description.startsWith ("Move"))
                    roll.lastLength = edited.length;
                roll.commitKeepingSelection (changed, selected, description);
                justAdded = description.startsWith ("Add ") && ! e.mouseWasDraggedSinceMouseDown();
            }
            break;
        }
        }
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            return roll.deleteSelection() || ! roll.getSelection().empty();
        if (key == juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0))
        {
            roll.selectAll();
            return true;
        }
        if (key == juce::KeyPress ('c', juce::ModifierKeys::ctrlModifier, 0))
        {
            roll.copySelection();
            return true;
        }
        if (key == juce::KeyPress ('x', juce::ModifierKeys::ctrlModifier, 0))
        {
            roll.cutSelection();
            return true;
        }
        if (key == juce::KeyPress ('v', juce::ModifierKeys::ctrlModifier, 0))
        {
            roll.paste();
            return true;
        }
        if (key == juce::KeyPress ('b', juce::ModifierKeys::ctrlModifier, 0))
        {
            roll.duplicateSelection();
            return true;
        }
        if (key == juce::KeyPress::escapeKey && ! roll.getSelection().empty())
        {
            roll.clearSelection();
            return true;
        }
        return false;
    }

private:
    using Tool = PianoRoll::Tool;
    enum class Mode
    {
        none,
        move,
        resize,
        erase,
        selectBox,
        slice,
        paint
    };

    model::Ticks sliceTick (const juce::ModifierKeys& mods) const
    {
        const auto at = roll.xToTick (startPosition.x);
        return mods.isAltDown() ? at : roll.snapTicks (at);
    }

    /** How many notes the paint drag has laid after the first (-1 before the start). */
    int paintSteps() const
    {
        const auto step = juce::jmax<model::Ticks> (1, roll.lastLength);
        const auto from = roll.snapTicks (roll.xToTick (startPosition.x));
        const auto to = roll.xToTick (currentPosition.x);
        return to < from ? -1 : (int)((to - from) / step);
    }

    void updatePaint()
    {
        roll.notes = before;
        for (auto& n : roll.paintedNotes (roll.snapTicks (roll.xToTick (startPosition.x)),
                                          roll.xToTick (currentPosition.x), pitches))
            roll.notes.push_back (n);
        repaint();
    }

    PianoRoll& roll;
    Mode mode = Mode::none;
    std::vector<int> indices; // the notes being moved or resized
    int anchor = 0;           // the one under the mouse, in `indices`
    std::vector<model::Note> originals, before;
    std::set<int> erased;
    bool justAdded = false;   // the last click added a note (a double-click on a ghost takes it back)
    std::vector<int> pitches; // painting: each painted note's pitch
    juce::Point<float> startPosition, currentPosition;
    juce::ModifierKeys currentModifiers;
    juce::String description;
};

//==============================================================================
class PianoRoll::Keyboard final : public juce::Component
{
public:
    explicit Keyboard (PianoRoll& r) : roll (r) {}

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        for (int pitch = 127; pitch >= 0; --pitch)
        {
            const auto y = roll.pitchToY (pitch);
            if (y > (float)getHeight() || y + (float)roll.keyHeight < 0.0f)
                continue;
            const auto r = juce::Rectangle<float> (0.0f, y, (float)getWidth(), (float)roll.keyHeight);
            g.setColour (isBlackKey (pitch) ? juce::Colour (0xff1a1a1a) : juce::Colour (0xffe8e8e8));
            g.fillRect (isBlackKey (pitch) ? r.withTrimmedRight ((float)getWidth() * 0.35f) : r);
            if (const auto key = roll.getHighlightedKey())
            {
                // A stripe on the right: the scale's notes, the root stronger.
                const auto stripe = r.withLeft (r.getRight() - 5.0f).reduced (0.0f, 1.0f);
                if (pitch % 12 == key->tonic)
                    g.setColour (themeOf (*this).get ("accent"));
                else if (model::isInKey (*key, pitch))
                    g.setColour (themeOf (*this).get ("accent").withAlpha (0.45f));
                else
                    g.setColour (juce::Colours::transparentBlack);
                g.fillRect (stripe);
            }
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.drawHorizontalLine ((int)r.getBottom() - 1, 0.0f, (float)getWidth());
            if (pitch % 12 == 0)
            {
                g.setColour (juce::Colours::black);
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (noteName (pitch), r.withTrimmedRight (3.0f), juce::Justification::centredRight, false);
            }
        }
        g.setColour (theme.get ("border"));
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float)getHeight());
    }

private:
    PianoRoll& roll;
};

//==============================================================================
class PianoRoll::Ruler final : public juce::Component
{
public:
    explicit Ruler (PianoRoll& r) : roll (r)
    {
        setHelpText ("Timeline: right-drag to loop part of the pattern; right-click to loop all of it again");
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panelHeader"));
        const auto barTicks = roll.getTimeSignature().barTicks();
        g.setFont (juce::FontOptions (11.0f));
        for (auto t = (roll.xToTick (0.0f) / barTicks) * barTicks; roll.tickToX (t) < (float)getWidth(); t += barTicks)
        {
            const auto x = roll.tickToX (t);
            g.setColour (theme.get ("textDim"));
            g.drawVerticalLine ((int)x, 0.0f, (float)getHeight());
            g.setColour (theme.get ("text"));
            g.drawText (juce::String (t / barTicks + 1),
                        juce::Rectangle<float> (x + 3.0f, 0.0f, 40.0f, (float)getHeight()),
                        juce::Justification::centredLeft, false);
        }
        g.setColour (theme.get ("border"));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float)getWidth());

        // The loop region (right-drag here): a band.
        if (const auto region = shownRegion())
        {
            const auto a = roll.tickToX (region->start), b = roll.tickToX (region->end);
            g.setColour (theme.get ("accentAlt").withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (a, 0.0f, b - a, (float)getHeight()));
            g.setColour (theme.get ("accentAlt"));
            g.drawVerticalLine ((int)a, 0.0f, (float)getHeight());
            g.drawVerticalLine ((int)b, 0.0f, (float)getHeight());
        }
    }

    // Right-drag: the pattern's loop region; a right-click without dragging clears it.
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = e.mods.isPopupMenu();
        if (dragging)
            mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        auto a = roll.snapTicks (roll.xToTick ((float)e.getMouseDownX()));
        auto b = roll.snapTicks (roll.xToTick (e.position.x));
        if (b < a)
            std::swap (a, b);
        roll.pendingLoop = Session::LoopRegion { juce::jmax<model::Ticks> (0, a), juce::jmax<model::Ticks> (0, b) };
        repaint();
        roll.grid->repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! std::exchange (dragging, false))
            return;
        const auto pending = std::exchange (roll.pendingLoop, std::nullopt);
        roll.session.setLoopRegion (
            Session::Mode::pattern,
            e.mouseWasDraggedSinceMouseDown() && pending && pending->end > pending->start ? pending : std::nullopt);
        repaint();
        roll.grid->repaint();
    }

private:
    std::optional<Session::LoopRegion> shownRegion() const { return roll.getShownLoop(); }

    PianoRoll& roll;
    bool dragging = false;
};

//==============================================================================
/** The lane under the grid: one bar per note for the chosen value. Pan and fine pitch grow from the
    middle line, the others from the bottom. */
class PianoRoll::Lane final : public juce::Component
{
public:
    explicit Lane (PianoRoll& r) : roll (r) {}

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("background"));
        g.setColour (theme.get ("border"));
        g.drawHorizontalLine (0, 0.0f, (float)getWidth());

        const auto mode = roll.laneMode;
        const auto range = getLaneRange (mode);
        const bool bipolar = range.getStart() < 0.0f;
        if (bipolar)
        {
            g.setColour (theme.get ("border").withAlpha (0.6f));
            g.drawHorizontalLine ((int)valueToY (0.0f), 0.0f, (float)getWidth());
        }

        // Which way is which, at the left edge.
        static const char* top[] = { "", "", "R", "+100", "" };
        static const char* bottom[] = { "", "", "L", "-100", "" };
        g.setColour (theme.get ("textDim"));
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (top[(int)mode], 4, 2, 40, 12, juce::Justification::topLeft, false);
        g.drawText (bottom[(int)mode], 4, getHeight() - 14, 40, 12, juce::Justification::bottomLeft, false);

        if (auto note = roll.getLaneNote(); note.isNotEmpty())
        {
            g.setColour (theme.get ("textDim"));
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (note, getLocalBounds().reduced (6, 4), juce::Justification::topRight, true);
        }

        for (auto& n : roll.notes)
        {
            const auto x = roll.tickToX (n.start);
            const auto y = valueToY (getNoteValue (n, mode));
            const auto base = bipolar ? valueToY (0.0f) : (float)getHeight();
            g.setColour (theme.get ("accent"));
            g.fillRect (x, juce::jmin (y, base), 3.0f, std::abs (base - y));
            g.fillEllipse (x - 2.0f, y - 3.0f, 7.0f, 7.0f);
        }
    }

    float valueToY (float value) const
    {
        const auto range = getLaneRange (roll.laneMode);
        const auto proportion = (value - range.getStart()) / range.getLength();
        return 3.0f + (1.0f - proportion) * (float)(getHeight() - 6);
    }

    float yToValue (float y) const
    {
        const auto range = getLaneRange (roll.laneMode);
        const auto proportion = juce::jlimit (0.0f, 1.0f, 1.0f - (y - 3.0f) / (float)(getHeight() - 6));
        return range.getStart() + proportion * range.getLength();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        index = -1;
        float best = 8.0f;
        for (size_t i = 0; i < roll.notes.size(); ++i)
            if (auto d = std::abs (roll.tickToX (roll.notes[i].start) - e.position.x); d < best)
            {
                best = d;
                index = (int)i;
            }
        before = roll.notes;
        resetting = e.mods.isPopupMenu();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (index < 0)
            return;
        const auto value = resetting ? getLaneDefault (roll.laneMode) : yToValue (e.position.y);
        setNoteValue (roll.notes[(size_t)index], roll.laneMode, value);
        repaint();
        roll.grid->repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (index < 0)
            return;
        const auto changed = roll.notes;
        roll.notes = before;
        if (! (changed == before))
        {
            if (roll.laneMode == LaneMode::velocity)
                roll.lastVelocity = changed[(size_t)index].velocity;
            roll.commit (changed, "Change " + getLaneName (roll.laneMode).toLowerCase());
        }
        index = -1;
    }

private:
    PianoRoll& roll;
    int index = -1;
    bool resetting = false;
    std::vector<model::Note> before;
};

//==============================================================================
/** The lane's name in the corner under the keyboard; a click picks another value to edit. */
class PianoRoll::LaneChoice final : public juce::Component
{
public:
    explicit LaneChoice (PianoRoll& r) : roll (r)
    {
        setHelpText ("Click to choose what the lane edits: velocity, release, pan, fine pitch or volume");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        auto area = getLocalBounds().reduced (2);
        g.setColour (theme.get (isMouseOver() ? "panel" : "background"));
        g.fillRoundedRectangle (area.toFloat(), 3.0f);
        g.setColour (theme.get ("text"));
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText (getLaneName (roll.laneMode), area.removeFromTop (30).reduced (2, 0),
                          juce::Justification::centred, 2);

        juce::Path arrow;
        const auto centre = area.toFloat().getCentre().withY ((float)area.getY() + 4.0f);
        arrow.addTriangle (centre.x - 4.0f, centre.y - 2.0f, centre.x + 4.0f, centre.y - 2.0f, centre.x,
                           centre.y + 2.0f);
        g.setColour (theme.get ("textDim"));
        g.fillPath (arrow);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        juce::PopupMenu menu;
        for (int i = 0; i < numLaneModes; ++i)
            menu.addItem (i + 1, getLaneName ((LaneMode)i), true, (int)roll.laneMode == i);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer (this)] (int result)
                            {
                                if (safe != nullptr && result > 0)
                                    safe->roll.setLaneMode ((LaneMode)(result - 1));
                            });
    }

private:
    PianoRoll& roll;
};

//==============================================================================
/** The corner above the keyboard: whose ghost notes are shown. */
class PianoRoll::GhostChoice final : public juce::Component
{
public:
    explicit GhostChoice (PianoRoll& r) : roll (r)
    {
        setHelpText ("Ghost notes: the other channels' notes in this pattern, shown faintly. Click to choose whose");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        auto area = getLocalBounds().reduced (2);
        g.setColour (theme.get (isMouseOver() ? "panel" : "background"));
        g.fillRoundedRectangle (area.toFloat(), 3.0f);
        g.setColour (theme.get (roll.ghostsShown ? "text" : "textDim"));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("Ghosts", area, juce::Justification::centred, false);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Show ghost notes", true, roll.ghostsShown);
        menu.addSectionHeader ("Channels");
        const auto& channels = roll.session.getProject().channels;
        for (size_t i = 0; i < channels.size(); ++i)
            if (channels[i].id != roll.session.getSelectedChannel())
                menu.addItem (100 + (int)i, channels[i].name, roll.ghostsShown,
                              roll.isGhostChannelShown (channels[i].id));
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer (this)] (int result)
                            {
                                if (safe == nullptr || result <= 0)
                                    return;
                                auto& r = safe->roll;
                                if (result == 1)
                                    return r.setGhostsShown (! r.ghostsShown);
                                const auto& list = r.session.getProject().channels;
                                if (const auto index = (size_t)(result - 100); index < list.size())
                                    r.setGhostChannelShown (list[index].id, ! r.isGhostChannelShown (list[index].id));
                            });
    }

private:
    PianoRoll& roll;
};

//==============================================================================
/** Over the top of the grid while a suggestion is shown: what it is, its variations, and the choices. */
class PianoRoll::SuggestionBar final : public juce::Component
{
public:
    explicit SuggestionBar (PianoRoll& r) : roll (r)
    {
        previous.setButtonText ("<");
        previous.setHelpText ("The previous variation");
        next.setButtonText (">");
        next.setHelpText ("The next variation");
        regenerate.setButtonText ("Regenerate");
        regenerate.setHelpText ("Ask for another suggestion");
        discard.setButtonText ("Discard");
        discard.setHelpText ("Hide the suggestion; your notes stay as they are");
        accept.setButtonText ("Accept");
        accept.setHelpText ("Write the suggested notes into the pattern (one undo step)");
        listen.setButtonText ("Listen");
        listen.setClickingTogglesState (true);
        listen.setHelpText ("Hear the suggestion: playback uses the blue notes instead of yours until you click "
                            "again, Accept or Discard");

        auto& s = roll.session.getSuggestions();
        previous.onClick = [&s] { s.showVariation (s.getVariationIndex() - 1); };
        next.onClick = [&s] { s.showVariation (s.getVariationIndex() + 1); };
        regenerate.onClick = [&s] { s.regenerate(); };
        listen.onClick = [this] { roll.listenToSuggestion (listen.getToggleState()); };
        discard.onClick = [&s] { s.discard(); };
        accept.onClick = [this, &s]
        {
            if (auto result = s.accept(); result.failed())
            {
                error = result.getErrorMessage();
                repaint();
            }
        };

        for (auto* b : { &previous, &next, &regenerate, &listen, &discard, &accept })
            addAndMakeVisible (b);
        setHelpText ("A suggestion: the blue notes. Accept writes them into the pattern; Discard hides them");
    }

    void update()
    {
        auto& s = roll.session.getSuggestions();
        const auto* current = s.getCurrent();
        const bool forThisPattern = current != nullptr && current->pattern == roll.session.getCurrentPattern();
        setVisible (forThisPattern || s.getWorkingMessage().isNotEmpty());
        error.clear();

        const bool showing = forThisPattern && s.getWorkingMessage().isEmpty();
        previous.setVisible (showing && s.getNumVariations() > 1);
        next.setVisible (showing && s.getNumVariations() > 1);
        previous.setEnabled (s.getVariationIndex() > 0);
        next.setEnabled (s.getVariationIndex() + 1 < s.getNumVariations());
        regenerate.setVisible (showing && s.getSource().regenerate != nullptr);
        listen.setVisible (showing);
        listen.setToggleState (s.isListening(), juce::dontSendNotification);
        accept.setVisible (showing);
        discard.setVisible (isVisible());
        resized();
        repaint();
    }

    juce::String getText() const
    {
        auto& s = roll.session.getSuggestions();
        if (error.isNotEmpty())
            return error;
        if (s.getWorkingMessage().isNotEmpty())
            return s.getWorkingMessage();
        const auto* current = s.getCurrent();
        if (current == nullptr)
            return {};

        auto text = (current->title.isNotEmpty() ? current->title : juce::String ("Suggestion"));
        if (s.getNumVariations() > 1)
            text << "  (" << (s.getVariationIndex() + 1) << " of " << s.getNumVariations() << ")";

        // Channels it changes other than the edited one (their notes show when you select them).
        juce::StringArray others;
        for (auto& [channel, notes] : current->notes)
            if (channel != roll.session.getSelectedChannel())
                if (const auto* c = roll.session.getProject().findChannel (channel))
                    others.add (c->name);
        if (! others.isEmpty())
            text << "  - also " << others.joinIntoString (", ");
        return text;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.setColour (theme.get ("panelHeader").withAlpha (0.95f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (theme.get ("accentAlt"));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
        g.setColour (error.isNotEmpty() ? theme.get ("danger") : theme.get ("text"));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (getText(), textArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 3);
        for (auto* b : { &accept, &discard, &listen, &regenerate, &next, &previous })
            if (b->isVisible())
            {
                const auto width = b == &previous || b == &next ? 24 : juce::jmax (64, b->getBestWidthForHeight (24));
                b->setBounds (area.removeFromRight (width));
                area.removeFromRight (4);
            }
        textArea = area.withTrimmedLeft (4);
    }

    juce::TextButton previous, next, regenerate, listen, discard, accept;

private:
    PianoRoll& roll;
    juce::Rectangle<int> textArea;
    juce::String error;
};

//==============================================================================
PianoRoll::PianoRoll (Session& s) : Panel ("pianoRoll", "Piano Roll"), session (s)
{
    setHelpText ("Piano Roll (F7): click to add a note, drag to move, drag the right edge to resize, right-click to "
                 "delete. Alt: no snap. Wheel: scroll, Ctrl+wheel: zoom");

    grid = std::make_unique<Grid> (*this);
    keyboard = std::make_unique<Keyboard> (*this);
    ruler = std::make_unique<Ruler> (*this);
    lane = std::make_unique<Lane> (*this);
    laneChoice = std::make_unique<LaneChoice> (*this);
    ghostChoice = std::make_unique<GhostChoice> (*this);
    suggestionBar = std::make_unique<SuggestionBar> (*this);
    for (auto* c : { (juce::Component*)grid.get(), (juce::Component*)keyboard.get(), (juce::Component*)ruler.get(),
                     (juce::Component*)lane.get(), laneChoice.get(), ghostChoice.get(), (juce::Component*)&horizontal,
                     (juce::Component*)&vertical })
        addAndMakeVisible (c);
    addChildComponent (*suggestionBar); // over the grid
    recordIdeaButton.setHelpText ("Record idea: sing, hum or beatbox over the pattern; the notes come as a suggestion");
    recordIdeaButton.onClick = [this]
    {
        if (onRecordIdea)
            onRecordIdea (recordIdeaButton);
    };
    addChildComponent (recordIdeaButton);
    generateButton.setHelpText ("Generate: the AI writes notes for this pattern from what you ask, or changes them");
    generateButton.onClick = [this]
    {
        if (onGenerate)
            onGenerate (generateButton);
    };
    addChildComponent (generateButton);
    scaleButton.setHelpText ("Scale: highlight the notes of a key (the song's key, or any other); notes outside it "
                             "get darker rows");
    scaleButton.onClick = [this] { showScaleMenu(); };
    addAndMakeVisible (scaleButton);

    const char* toolNames[] = { "Draw", "Paint", "Slice", "Select" };
    const char* toolHelp[] = {
        "Draw: click to add a note, drag notes to move them, drag their end to resize them",
        "Paint: drag across the grid to lay notes one after another (the last length you used)",
        "Slice: drag a line down across notes to cut them in two there",
        "Select: drag a box around notes to select them; then drag, resize or Delete them together",
    };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = toolButtons[i];
        b.setButtonText (toolNames[i]);
        b.setHelpText (toolHelp[i]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x5170);
        // Choosing a tool also "clicks" the one switched off: only the one switched on counts.
        b.onClick = [this, i]
        {
            if (toolButtons[i].getToggleState())
                setTool ((Tool)i);
        };
        addAndMakeVisible (b);
    }
    setTool (Tool::draw);
    setLaneMode (LaneMode::velocity);

    for (auto [choice, text] :
         { std::pair { Snap::step, "Step" }, std::pair { Snap::bar, "Bar" }, std::pair { Snap::beat, "Beat" },
           std::pair { Snap::eighth, "1/8" }, std::pair { Snap::sixteenth, "1/16" },
           std::pair { Snap::thirtySecond, "1/32" }, std::pair { Snap::eighthTriplet, "1/8 triplet" },
           std::pair { Snap::sixteenthTriplet, "1/16 triplet" }, std::pair { Snap::none, "None" } })
        snapBox.addItem (text, (int)choice);
    snapBox.setSelectedId ((int)Snap::step, juce::dontSendNotification);
    snapBox.setHelpText ("Snap: what notes line up to. Step follows the pattern's steps (sixteenths, or "
                         "triplets when the Channel Rack has Triplet steps on). Alt while dragging: no snap");
    snapBox.onChange = [this] { setSnapChoice ((Snap)snapBox.getSelectedId()); };
    addAndMakeVisible (snapBox);

    // The chord stamp: a click draws the whole chord.
    chordBox.addItem ("No chord", 1);
    chordBox.addItem ("Chord in key (3 notes)", 2);
    chordBox.addItem ("Chord in key (4 notes)", 3);
    chordBox.addSeparator();
    const auto& chordTypes = model::getChordTypes();
    for (size_t i = 0; i < chordTypes.size(); ++i)
        chordBox.addItem (chordTypes[i].name, 10 + (int)i);
    chordBox.setSelectedId (1, juce::dontSendNotification);
    chordBox.setHelpText ("Chord stamp: a click draws a whole chord on the note you click. In key: the song's key's "
                          "own chord there (or the scale chosen beside it)");
    chordBox.onChange = [this] { setChordStamp (chordBox.getSelectedId()); };
    addAndMakeVisible (chordBox);

    toolsButton.setHelpText (
        "Tools for the selected notes (or all of them): quantize, strum, arpeggiate, chop, legato, "
        "humanize");
    toolsButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (selection.empty() ? "All notes"
                                                 : juce::String ((int)selection.size()) + " selected notes");
        menu.addItem ("Quantize (to the snap)", [this] { applyNoteTool (NoteTool::quantize); });
        menu.addItem ("Strum up", [this] { applyNoteTool (NoteTool::strumUp); });
        menu.addItem ("Strum down", [this] { applyNoteTool (NoteTool::strumDown); });
        menu.addItem ("Arpeggiate (chords into notes, one per snap step)",
                      [this] { applyNoteTool (NoteTool::arpeggiate); });
        menu.addItem ("Chop (repeat each note every snap step)", [this] { applyNoteTool (NoteTool::chop); });
        menu.addItem ("Legato (each note lasts until the next)", [this] { applyNoteTool (NoteTool::legato); });
        menu.addItem ("Humanize velocities", [this] { applyNoteTool (NoteTool::humanize); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toolsButton));
    };
    addAndMakeVisible (toolsButton);

    emptyMessage.setText ("Add a channel in the Channel Rack (F6) to write notes for it.", juce::dontSendNotification);
    emptyMessage.setJustificationType (juce::Justification::centred);
    emptyMessage.setInterceptsMouseClicks (false, false);
    addChildComponent (emptyMessage);

    horizontal.setAutoHide (false);
    vertical.setAutoHide (false);
    horizontal.addListener (this);
    vertical.addListener (this);

    session.addChangeListener (this);
    session.getSuggestions().addChangeListener (this);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    reloadNotes();
    scrollToPitch (66);
    startTimerHz (30);
}

PianoRoll::~PianoRoll()
{
    cancelPendingUpdate();
    previewPitches ({});
    session.getHistory().removeListener (historyListener);
    session.getSuggestions().removeChangeListener (this);
    session.removeChangeListener (this);
}

//==============================================================================
float PianoRoll::tickToX (model::Ticks t) const
{
    return (float)(((double)t - scrollTicks) / (double)model::ticksPerQuarterNote * pixelsPerBeat);
}

model::Ticks PianoRoll::xToTick (float x) const
{
    return (model::Ticks)std::floor ((double)x / pixelsPerBeat * (double)model::ticksPerQuarterNote + scrollTicks);
}

float PianoRoll::pitchToY (int pitch) const { return (float)((127 - pitch) * keyHeight - scrollY); }

int PianoRoll::yToPitch (float y) const
{
    return juce::jlimit (0, 127, 127 - (int)std::floor (((double)y + scrollY) / keyHeight));
}

bool PianoRoll::applyNoteTool (NoteTool which)
{
    namespace nt = model::noteTools;
    // The selected notes, or all of them; the others stay as they are.
    std::vector<model::Note> chosen, rest;
    for (int i = 0; i < (int)notes.size(); ++i)
        (selection.empty() || selection.contains (i) ? chosen : rest).push_back (notes[(size_t)i]);
    if (chosen.empty())
        return false;

    std::vector<model::Note> result;
    juce::String description;
    switch (which)
    {
    case NoteTool::quantize:
        result = nt::quantize (chosen, snap), description = "Quantize notes";
        break;
    case NoteTool::strumUp:
        result = nt::strum (chosen, strumSpacing, true), description = "Strum up";
        break;
    case NoteTool::strumDown:
        result = nt::strum (chosen, strumSpacing, false), description = "Strum down";
        break;
    case NoteTool::arpeggiate:
        result = nt::arpeggiate (chosen, snap), description = "Arpeggiate";
        break;
    case NoteTool::chop:
        result = nt::chop (chosen, snap), description = "Chop notes";
        break;
    case NoteTool::legato:
        result = nt::legato (chosen), description = "Legato";
        break;
    case NoteTool::humanize:
    {
        juce::Random random;
        result = nt::humanizeVelocity (chosen, 0.15f, random);
        description = "Humanize velocities";
        break;
    }
    }
    auto changed = rest;
    changed.insert (changed.end(), result.begin(), result.end());
    return commitKeepingSelection (changed, result, description);
}

void PianoRoll::setChordStamp (int choice)
{
    chordStamp = choice;
    chordBox.setSelectedId (choice, juce::dontSendNotification);
}

std::vector<int> PianoRoll::stampPitches (int root) const
{
    if (chordStamp == 2 || chordStamp == 3)
        return model::chordInKey (root, getHighlightedKey().value_or (model::Key { 0, "major" }),
                                  chordStamp == 2 ? 3 : 4);
    const auto& types = model::getChordTypes();
    if (juce::isPositiveAndBelow (chordStamp - 10, (int)types.size()))
        return model::chordOn (root, types[(size_t)(chordStamp - 10)]);
    return { root };
}

void PianoRoll::setSnapChoice (Snap choice)
{
    snapChoice = choice;
    snapBox.setSelectedId ((int)choice, juce::dontSendNotification);
    updateSnap();
    grid->repaint();
}

void PianoRoll::updateSnap()
{
    const auto sig = getTimeSignature();
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    switch (snapChoice)
    {
    case Snap::step:
        snap = pattern != nullptr ? pattern->getStepTicks() : model::ticksPerQuarterNote / 4;
        break;
    case Snap::bar:
        snap = sig.barTicks();
        break;
    case Snap::beat:
        snap = sig.beatTicks();
        break;
    case Snap::eighth:
        snap = model::ticksPerQuarterNote / 2;
        break;
    case Snap::sixteenth:
        snap = model::ticksPerQuarterNote / 4;
        break;
    case Snap::thirtySecond:
        snap = model::ticksPerQuarterNote / 8;
        break;
    case Snap::eighthTriplet:
        snap = model::ticksPerQuarterNote / 3;
        break;
    case Snap::sixteenthTriplet:
        snap = model::ticksPerQuarterNote / 6;
        break;
    case Snap::none:
        snap = 1;
        break;
    }
}

model::Ticks PianoRoll::snapTicks (model::Ticks t) const
{
    return (model::Ticks)std::llround ((double)t / (double)snap) * snap;
}

void PianoRoll::scrollToPitch (int pitch)
{
    scrollY = juce::jmax (0.0, (127.0 - pitch) * keyHeight - grid->getHeight() / 2.0);
    updateScrollBars();
    repaint();
}

model::TimeSignature PianoRoll::getTimeSignature() const
{
    return session.getProject().getTimeSignature (session.getCurrentPattern());
}

model::Ticks PianoRoll::getBarLength() const { return getTimeSignature().barTicks(); }

model::Ticks PianoRoll::getPatternLength() const
{
    if (auto* pattern = session.getProject().findPattern (session.getCurrentPattern()))
        return pattern->length;
    return model::ticksPerQuarterNote * 4;
}

//==============================================================================
void PianoRoll::reloadNotes()
{
    updateSnap(); // the pattern (and its steps) may have changed
    const auto keep = selectAfterCommit ? *selectAfterCommit : getSelectedNotes();
    notes.clear();
    if (auto* pattern = session.getProject().findPattern (session.getCurrentPattern()))
        if (auto it = pattern->notes.find (session.getSelectedChannel()); it != pattern->notes.end())
            notes = it->second;

    std::sort (notes.begin(), notes.end(), byStartThenPitch);
    selectMatching (keep);

    suggestionBar->update();

    ghosts.clear();
    if (auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
        pattern != nullptr && ghostsShown)
        for (auto& channel : session.getProject().channels)
        {
            auto it = pattern->notes.find (channel.id);
            if (channel.id == session.getSelectedChannel() || hiddenGhosts.contains (channel.id) ||
                it == pattern->notes.end() || it->second.empty())
                continue;
            ghosts.push_back ({ channel.id, channel.name, juce::Colour (channel.colour), it->second });
        }

    const auto* channel = session.getProject().findChannel (session.getSelectedChannel());
    emptyMessage.setVisible (channel == nullptr);
    if (channel != nullptr)
        setHelpText ("Piano Roll (F7), " + channel->name +
                     ": click to add a note, drag to move, drag the right edge to "
                     "resize, right-click to delete. Alt: no snap. Wheel: scroll, Ctrl+wheel: zoom");

    updateScrollBars();
    repaint();
}

bool PianoRoll::ensurePattern()
{
    if (session.getCurrentPattern().isValid())
        return true;

    // Like the Channel Rack: the first note creates a one-bar pattern.
    const auto bar = session.getProject().settings.timeSignature.barTicks();
    juce::String error;
    auto command = commands::CommandRegistry::get().create ("pattern.add", object ({ { "length", (int)bar } }), error);
    return command != nullptr && session.getHistory().perform (std::move (command)).wasOk();
}

bool PianoRoll::commit (std::vector<model::Note> newNotes, const juce::String& description)
{
    if (! session.getSelectedChannel().isValid() || ! ensurePattern())
        return false;

    std::sort (newNotes.begin(), newNotes.end(), byStartThenPitch);

    juce::Array<juce::var> list;
    for (auto& n : newNotes)
        list.add (model::noteToJson (n));

    juce::String error;
    auto command =
        commands::CommandRegistry::get().create ("pattern.setNotes",
                                                 object ({ { "pattern", (int)session.getCurrentPattern().value },
                                                           { "channel", (int)session.getSelectedChannel().value },
                                                           { "notes", list },
                                                           { "description", description } }),
                                                 error);

    // Notes placed past the pattern's end make it longer, to the end of their bar (FL), and removing them
    // makes it shorter again: pattern.setNotes keeps the length fitted, in the same undo step.
    const bool ok = command != nullptr && session.getHistory().perform (std::move (command)).wasOk();
    reloadNotes(); // show the committed state now (tests and the release of a drag)
    return ok;
}

bool PianoRoll::commitKeepingSelection (std::vector<model::Note> newNotes, std::vector<model::Note> selected,
                                        const juce::String& description)
{
    selectAfterCommit = std::move (selected);
    const bool ok = commit (std::move (newNotes), description);
    selectAfterCommit.reset();
    return ok;
}

std::vector<model::Note> PianoRoll::getSelectedNotes() const
{
    std::vector<model::Note> selected;
    selected.reserve (selection.size());
    for (auto i : selection)
        if (i < (int)notes.size())
            selected.push_back (notes[(size_t)i]);
    return selected;
}

void PianoRoll::selectMatching (const std::vector<model::Note>& wanted)
{
    selection.clear();
    std::vector<bool> used (notes.size(), false);
    for (auto& w : wanted)
        for (size_t i = 0; i < notes.size(); ++i)
            if (! used[i] && notes[i] == w)
            {
                used[i] = true;
                selection.insert ((int)i);
                break;
            }
}

void PianoRoll::setSelection (std::set<int> newSelection)
{
    std::erase_if (newSelection, [this] (int i) { return i < 0 || i >= (int)notes.size(); });
    selection = std::move (newSelection);
    grid->repaint();
}

void PianoRoll::selectAll()
{
    std::set<int> all;
    for (int i = 0; i < (int)notes.size(); ++i)
        all.insert (i);
    setSelection (std::move (all));
}

void PianoRoll::selectArea (model::Ticks from, model::Ticks to, int lowPitch, int highPitch, bool add)
{
    auto chosen = add ? selection : std::set<int>();
    for (int i = 0; i < (int)notes.size(); ++i)
    {
        const auto& n = notes[(size_t)i];
        if (n.start <= to && n.start + n.length >= from && n.pitch >= lowPitch && n.pitch <= highPitch)
            chosen.insert (i);
    }
    setSelection (std::move (chosen));
}

bool PianoRoll::moveSelection (model::Ticks deltaTicks, int deltaPitch)
{
    if (selection.empty())
        return false;
    auto changed = notes;
    std::vector<model::Note> moved;
    for (auto i : selection)
    {
        auto& n = changed[(size_t)i];
        n.start = juce::jmax<model::Ticks> (0, n.start + deltaTicks);
        n.pitch = juce::jlimit (0, 127, n.pitch + deltaPitch);
        moved.push_back (n);
    }
    return commitKeepingSelection (changed, moved, selection.size() > 1 ? "Move notes" : "Move note");
}

bool PianoRoll::resizeSelection (model::Ticks deltaTicks, model::Ticks minimum)
{
    if (selection.empty())
        return false;
    auto changed = notes;
    std::vector<model::Note> resized;
    for (auto i : selection)
    {
        auto& n = changed[(size_t)i];
        n.length = juce::jmax (juce::jmax<model::Ticks> (1, minimum), n.length + deltaTicks);
        resized.push_back (n);
    }
    return commitKeepingSelection (changed, resized, selection.size() > 1 ? "Resize notes" : "Resize note");
}

bool PianoRoll::deleteSelection() { return deleteNotes (selection); }

namespace
{
std::vector<model::Note>& copiedNotes()
{
    static std::vector<model::Note> copied;
    return copied;
}
} // namespace

const std::vector<model::Note>& PianoRoll::getCopiedNotes() { return copiedNotes(); }

bool PianoRoll::copySelection()
{
    auto chosen = selection.empty() ? notes : getSelectedNotes();
    if (chosen.empty())
        return false;
    std::sort (chosen.begin(), chosen.end(), byStartThenPitch);
    copiedNotes() = std::move (chosen);
    return true;
}

bool PianoRoll::cutSelection()
{
    if (! copySelection())
        return false;
    if (selection.empty())
        selectAll();
    const auto count = selection.size();
    auto changed = notes;
    for (auto it = selection.rbegin(); it != selection.rend(); ++it)
        changed.erase (changed.begin() + *it);
    return commitKeepingSelection (changed, {}, count > 1 ? "Cut notes" : "Cut note");
}

namespace
{
/** How far to move notes so that they come right after themselves: their span, rounded up to whole bars
    (or beats, when shorter than a bar). */
model::Ticks spanAfter (const std::vector<model::Note>& placed, model::Ticks bar, model::Ticks beat)
{
    model::Ticks from = std::numeric_limits<model::Ticks>::max(), to = 0;
    for (const auto& n : placed)
    {
        from = std::min (from, n.start);
        to = std::max (to, n.start + n.length);
    }
    const auto span = juce::jmax<model::Ticks> (1, to - from);
    const auto unit = juce::jmax<model::Ticks> (1, span >= bar ? bar : beat);
    return (span + unit - 1) / unit * unit;
}
} // namespace

bool PianoRoll::paste()
{
    auto pasted = copiedNotes();
    if (pasted.empty())
        return false;
    const auto sig = session.getProject().getTimeSignature (session.getCurrentPattern());
    // On the very same notes (pasting back where they were copied from): right after them instead.
    const bool samePlace = std::all_of (pasted.begin(), pasted.end(), [this] (const model::Note& n)
                                        { return std::find (notes.begin(), notes.end(), n) != notes.end(); });
    if (samePlace)
    {
        const auto shift = spanAfter (pasted, sig.barTicks(), sig.beatTicks());
        for (auto& n : pasted)
            n.start += shift;
    }
    auto changed = notes;
    changed.insert (changed.end(), pasted.begin(), pasted.end());
    return commitKeepingSelection (changed, pasted, pasted.size() > 1 ? "Paste notes" : "Paste note");
}

bool PianoRoll::duplicateSelection()
{
    if (selection.empty())
        selectAll();
    auto copies = getSelectedNotes();
    if (copies.empty())
        return false;
    const auto sig = session.getProject().getTimeSignature (session.getCurrentPattern());
    const auto shift = spanAfter (copies, sig.barTicks(), sig.beatTicks());
    for (auto& n : copies)
        n.start += shift;
    auto changed = notes;
    changed.insert (changed.end(), copies.begin(), copies.end());
    return commitKeepingSelection (changed, copies, copies.size() > 1 ? "Duplicate notes" : "Duplicate note");
}

bool PianoRoll::deleteNotes (const std::set<int>& indices)
{
    auto changed = notes;
    int removed = 0;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        if (*it >= 0 && *it < (int)changed.size())
        {
            changed.erase (changed.begin() + *it);
            ++removed;
        }
    if (removed == 0)
        return false;
    return commitKeepingSelection (changed, {}, removed > 1 ? "Delete notes" : "Delete note");
}

bool PianoRoll::slice (model::Ticks at, int lowPitch, int highPitch)
{
    std::vector<model::Note> changed, selected;
    bool cut = false;
    for (int i = 0; i < (int)notes.size(); ++i)
    {
        const auto& n = notes[(size_t)i];
        if (n.start < at && at < n.start + n.length && n.pitch >= lowPitch && n.pitch <= highPitch)
        {
            auto first = n, second = n;
            first.length = at - n.start;
            second.start = at;
            second.length = n.start + n.length - at;
            changed.push_back (first);
            changed.push_back (second);
            if (isSelected (i))
                selected.insert (selected.end(), { first, second });
            cut = true;
        }
        else
        {
            changed.push_back (n);
            if (isSelected (i))
                selected.push_back (n);
        }
    }
    return cut && commitKeepingSelection (changed, selected, "Slice notes");
}

std::vector<model::Note> PianoRoll::paintedNotes (model::Ticks from, model::Ticks to,
                                                  const std::vector<int>& pitches) const
{
    std::vector<model::Note> painted;
    const auto step = juce::jmax<model::Ticks> (1, lastLength);
    if (pitches.empty())
        return painted;
    int k = 0;
    for (auto start = juce::jmax<model::Ticks> (0, from); start <= juce::jmax (from, to); start += step, ++k)
        painted.push_back ({ start,
                             lastLength,
                             juce::jlimit (0, 127, pitches[(size_t)juce::jmin (k, (int)pitches.size() - 1)]),
                             lastVelocity,
                             0.5f,
                             0.0f,
                             0.0f,
                             {} });
    return painted;
}

bool PianoRoll::paint (model::Ticks from, model::Ticks to, const std::vector<int>& pitches)
{
    if (! session.getSelectedChannel().isValid() || ! ensurePattern())
        return false;
    reloadNotes();
    auto painted = paintedNotes (from, to, pitches);
    if (painted.empty())
        return false;
    auto changed = notes;
    changed.insert (changed.end(), painted.begin(), painted.end());
    return commitKeepingSelection (changed, {}, "Paint notes");
}

void PianoRoll::previewPitches (const std::vector<int>& pitches, float velocity)
{
    // Not while recording (the preview would be recorded), nor for a big selection dragged along.
    auto wanted = pitches;
    if (session.isRecordArmed() || ! previewOn || wanted.size() > 12)
        wanted.clear();
    if (wanted == previewing)
        return;
    auto& recorder = session.getRecorder();
    for (auto pitch : previewing)
        recorder.playComputerKey (pitch, false);
    for (auto pitch : wanted)
        recorder.playComputerKey (pitch, true, juce::jlimit (0.05f, 1.0f, velocity));
    previewing = std::move (wanted);
}

void PianoRoll::setTool (Tool newTool)
{
    tool = newTool;
    for (int i = 0; i < 4; ++i)
        toolButtons[i].setToggleState (i == (int)tool, juce::dontSendNotification);
}

bool PianoRoll::addNote (int pitch, model::Ticks start, model::Ticks length, float velocity)
{
    if (! session.getSelectedChannel().isValid() || ! ensurePattern())
        return false;
    reloadNotes();
    auto changed = notes;
    changed.push_back ({ start, length, pitch, velocity, 0.5f, 0.0f, 0.0f, {} });
    return commit (changed, "Add note");
}

bool PianoRoll::moveNote (int index, model::Ticks deltaTicks, int deltaPitch)
{
    if (index < 0 || index >= (int)notes.size())
        return false;
    auto changed = notes;
    auto& n = changed[(size_t)index];
    n.start = juce::jmax<model::Ticks> (0, n.start + deltaTicks);
    n.pitch = juce::jlimit (0, 127, n.pitch + deltaPitch);
    return commit (changed, "Move note");
}

bool PianoRoll::resizeNote (int index, model::Ticks newLength)
{
    if (index < 0 || index >= (int)notes.size() || newLength <= 0)
        return false;
    auto changed = notes;
    changed[(size_t)index].length = newLength;
    return commit (changed, "Resize note");
}

bool PianoRoll::deleteNote (int index)
{
    if (index < 0 || index >= (int)notes.size())
        return false;
    auto changed = notes;
    changed.erase (changed.begin() + index);
    return commit (changed, "Delete note");
}

bool PianoRoll::setVelocity (int index, float velocity)
{
    if (index < 0 || index >= (int)notes.size())
        return false;
    auto changed = notes;
    changed[(size_t)index].velocity = juce::jlimit (0.0f, 1.0f, velocity);
    return commit (changed, "Change velocity");
}

bool PianoRoll::setNoteValue (int index, LaneMode mode, float value)
{
    if (index < 0 || index >= (int)notes.size())
        return false;
    auto changed = notes;
    setNoteValue (changed[(size_t)index], mode, value);
    return commit (changed, "Change " + getLaneName (mode).toLowerCase());
}

//==============================================================================
void PianoRoll::setGenerate (std::function<void (juce::Component&)> callback)
{
    onGenerate = std::move (callback);
    generateButton.setVisible (onGenerate != nullptr);
    resized();
}

void PianoRoll::setRecordIdea (std::function<void (juce::Component&)> callback)
{
    onRecordIdea = std::move (callback);
    recordIdeaButton.setVisible (onRecordIdea != nullptr);
    resized();
}

const std::vector<model::Note>* PianoRoll::getSuggestedNotes() const
{
    const auto* current = session.getSuggestions().getCurrent();
    if (current == nullptr || current->pattern != session.getCurrentPattern())
        return nullptr;
    auto it = current->notes.find (session.getSelectedChannel());
    return it != current->notes.end() ? &it->second : nullptr;
}

juce::Component& PianoRoll::getSuggestionBar() noexcept { return *suggestionBar; }

void PianoRoll::highlightSongKey()
{
    followSongKey = true;
    chosenKey.reset();
    resized();
    repaint();
}

void PianoRoll::highlightKey (std::optional<model::Key> key)
{
    followSongKey = false;
    chosenKey = std::move (key);
    resized();
    repaint();
}

std::optional<model::Key> PianoRoll::getHighlightedKey() const
{
    return followSongKey ? session.getProject().settings.key : chosenKey;
}

juce::String PianoRoll::getScaleButtonText() const
{
    const auto key = getHighlightedKey();
    if (! key)
        return "Scale: off";
    static const char* tonics[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return "Scale: " + juce::String (tonics[juce::jlimit (0, 11, key->tonic)]) + " " + key->scale;
}

void PianoRoll::showScaleMenu()
{
    static const char* tonics[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto songKey = session.getProject().settings.key;
    juce::PopupMenu menu;
    menu.addItem (1,
                  songKey ? "The song's key (" + juce::String (tonics[songKey->tonic]) + " " + songKey->scale + ")"
                          : juce::String ("The song's key (none set)"),
                  true, followSongKey);
    menu.addItem (2, "Off", true, ! followSongKey && ! chosenKey);
    menu.addSeparator();
    const auto& scales = model::getScaleNames();
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        juce::PopupMenu sub;
        for (int i = 0; i < scales.size(); ++i)
        {
            const model::Key key { tonic, scales[i] };
            sub.addItem (1000 + tonic * 100 + i, scales[i], true, ! followSongKey && chosenKey == key);
        }
        menu.addSubMenu (tonics[tonic], sub, true, nullptr, ! followSongKey && chosenKey && chosenKey->tonic == tonic);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scaleButton),
                        [safe = juce::Component::SafePointer (this)] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;
                            if (result == 1)
                                return safe->highlightSongKey();
                            if (result == 2)
                                return safe->highlightKey (std::nullopt);
                            const auto tonic = (result - 1000) / 100, scale = (result - 1000) % 100;
                            safe->highlightKey (model::Key { tonic, model::getScaleNames()[scale] });
                        });
}

void PianoRoll::listenToSuggestion (bool on)
{
    auto& s = session.getSuggestions();
    s.setListening (on);
    // Starting to listen while stopped plays the suggestion's pattern.
    if (s.isListening() && ! session.isPlaying())
    {
        session.setMode (Session::Mode::pattern);
        session.setCurrentPattern (s.getCurrent()->pattern);
        session.play();
    }
}

//==============================================================================
void PianoRoll::setGhostsShown (bool shown)
{
    ghostsShown = shown;
    ghostChoice->repaint();
    reloadNotes();
}

void PianoRoll::setGhostChannelShown (model::ChannelId channel, bool shown)
{
    if (shown)
        hiddenGhosts.erase (channel);
    else
        hiddenGhosts.insert (channel);
    reloadNotes();
}

//==============================================================================
void PianoRoll::setLaneMode (LaneMode mode)
{
    laneMode = mode;
    laneChoice->repaint();

    static const char* help[] = { "Velocity: how hard each note is played. Drag a bar; right-click resets it",
                                  "Release velocity: how fast each note is let go, for instruments that use it. "
                                  "Drag a bar; right-click resets it",
                                  "Pan: each note left or right. Drag a bar; right-click centres it",
                                  "Fine pitch: each note up or down by up to a semitone (100 cents). Drag a bar; "
                                  "right-click resets it",
                                  "Volume: each note's level, without changing how hard it is played. Drag a bar; "
                                  "right-click resets it" };
    lane->setHelpText (help[(int)mode]);
    lane->repaint();
}

juce::String PianoRoll::getLaneName (LaneMode mode)
{
    switch (mode)
    {
    case LaneMode::velocity:
        return "Velocity";
    case LaneMode::release:
        return "Release";
    case LaneMode::pan:
        return "Pan";
    case LaneMode::finePitch:
        return "Fine pitch";
    case LaneMode::volume:
        return "Volume";
    }
    return {};
}

juce::Range<float> PianoRoll::getLaneRange (LaneMode mode)
{
    switch (mode)
    {
    case LaneMode::pan:
        return { -1.0f, 1.0f };
    case LaneMode::finePitch:
        return { -100.0f, 100.0f };
    case LaneMode::velocity:
    case LaneMode::release:
    case LaneMode::volume:
        break;
    }
    return { 0.0f, 1.0f };
}

float PianoRoll::getLaneDefault (LaneMode mode)
{
    switch (mode)
    {
    case LaneMode::velocity:
        return 0.78f;
    case LaneMode::release:
        return 0.5f;
    case LaneMode::volume:
        return 1.0f;
    case LaneMode::pan:
    case LaneMode::finePitch:
        break;
    }
    return 0.0f;
}

float PianoRoll::getNoteValue (const model::Note& n, LaneMode mode)
{
    switch (mode)
    {
    case LaneMode::velocity:
        return n.velocity;
    case LaneMode::release:
        return n.releaseVelocity;
    case LaneMode::pan:
        return n.pan;
    case LaneMode::finePitch:
        return n.finePitchCents;
    case LaneMode::volume:
        return n.getVolume();
    }
    return 0.0f;
}

void PianoRoll::setNoteValue (model::Note& n, LaneMode mode, float value)
{
    value = getLaneRange (mode).clipValue (value);
    switch (mode)
    {
    case LaneMode::velocity:
        n.velocity = value;
        break;
    case LaneMode::release:
        n.releaseVelocity = value;
        break;
    case LaneMode::pan:
        n.pan = value;
        break;
    case LaneMode::finePitch:
        n.finePitchCents = value;
        break;
    case LaneMode::volume:
        n.setVolume (value);
        break;
    }
}

juce::String PianoRoll::getLaneNote() const
{
    if (laneMode == LaneMode::velocity || laneMode == LaneMode::release)
        return {};
    const auto& project = session.getProject();
    const auto* channel = project.findChannel (session.getSelectedChannel());
    if (channel == nullptr || ! channel->instrument ||
        engine::EngineAdapter::playsNoteExpressions (project, channel->id))
        return {};
    return "This instrument ignores per-note " + getLaneName (laneMode).toLowerCase() +
           " (Spinus instruments and CLAP plugins use it)";
}

//==============================================================================
void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (themeOf (*this).get ("panelHeader"));
    for (auto* c : { (juce::Component*)grid.get(), (juce::Component*)keyboard.get(), (juce::Component*)ruler.get(),
                     (juce::Component*)lane.get() })
        c->repaint();
}

void PianoRoll::resized()
{
    auto area = getLocalBounds();
    const int bar = 12;
    vertical.setBounds (
        area.removeFromRight (bar).withTrimmedTop (rulerHeight).withTrimmedBottom (velocityHeight + bar));
    horizontal.setBounds (area.removeFromBottom (bar).withTrimmedLeft (keyboardWidth));
    auto laneArea = area.removeFromBottom (velocityHeight);
    laneChoice->setBounds (laneArea.removeFromLeft (keyboardWidth).removeFromTop (44));
    lane->setBounds (laneArea);
    auto top = area.removeFromTop (rulerHeight);
    ghostChoice->setBounds (top.removeFromLeft (keyboardWidth));
    scaleButton.setButtonText (getScaleButtonText());
    scaleButton.setBounds (
        top.removeFromRight (juce::jmax (90, scaleButton.getBestWidthForHeight (top.getHeight()))).reduced (2, 1));
    for (int i = 3; i >= 0; --i)
        toolButtons[i].setBounds (top.removeFromRight (i == 3 ? 50 : 44).reduced (1, 1));
    snapBox.setBounds (top.removeFromRight (104).reduced (2, 1));
    chordBox.setBounds (top.removeFromRight (150).reduced (2, 1));
    toolsButton.setBounds (top.removeFromRight (64).reduced (2, 1));
    top.removeFromRight (4);
    if (recordIdeaButton.isVisible())
        recordIdeaButton.setBounds (top.removeFromRight (92).reduced (2, 1));
    if (generateButton.isVisible())
        generateButton.setBounds (top.removeFromRight (80).reduced (2, 1));
    ruler->setBounds (top);
    keyboard->setBounds (area.removeFromLeft (keyboardWidth));
    grid->setBounds (area);
    emptyMessage.setBounds (grid->getBounds());
    suggestionBar->setBounds (grid->getBounds().removeFromTop (32).reduced (6, 2));
    updateScrollBars();
}

void PianoRoll::updateScrollBars()
{
    model::Ticks end = getPatternLength();
    for (auto& n : notes)
        end = juce::jmax (end, n.start + n.length);
    const auto visibleTicks = grid->getWidth() / pixelsPerBeat * model::ticksPerQuarterNote;
    horizontal.setRangeLimits (0.0,
                               juce::jmax ((double)end + model::ticksPerQuarterNote * 8, scrollTicks + visibleTicks));
    horizontal.setCurrentRange (scrollTicks, visibleTicks, juce::dontSendNotification);
    vertical.setRangeLimits (0.0, 128.0 * keyHeight);
    vertical.setCurrentRange (scrollY, grid->getHeight(), juce::dontSendNotification);
}

void PianoRoll::zoom (float factor, bool verticalZoom)
{
    if (verticalZoom)
        keyHeight = juce::jlimit (6, 32, juce::roundToInt ((float)keyHeight * factor));
    else
        pixelsPerBeat = juce::jlimit (12.0, 800.0, pixelsPerBeat * factor);
    updateScrollBars();
    repaint();
}

void PianoRoll::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    (bar == &horizontal ? scrollTicks : scrollY) = start;
    repaint();
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
    {
        zoom (wheel.deltaY > 0 ? 1.15f : 1.0f / 1.15f, e.mods.isShiftDown());
        return;
    }

    if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        scrollTicks =
            juce::jmax (0.0, scrollTicks - (double)(wheel.deltaX + (e.mods.isShiftDown() ? wheel.deltaY : 0.0f)) * 4.0 *
                                               model::ticksPerQuarterNote);
    else
        scrollY = juce::jlimit (0.0, juce::jmax (0.0, 128.0 * keyHeight - grid->getHeight()),
                                scrollY - (double)wheel.deltaY * 12.0 * keyHeight);

    updateScrollBars();
    repaint();
}

void PianoRoll::timerCallback()
{
    double ticks = -1.0;
    if (session.isPlaying() && session.getMode() == Session::Mode::pattern)
    {
        auto& edit = session.getEdit();
        ticks = std::fmod (engine::timeToTicks (edit, edit.getTransport().getPosition()),
                           (double)juce::jmax<model::Ticks> (1, getPatternLength()));
    }

    if (std::abs (ticks - playheadTicks) > 1.0)
    {
        playheadTicks = ticks;
        grid->repaint();
    }
}

} // namespace spinus::ui
