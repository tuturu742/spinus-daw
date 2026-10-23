#include "Playlist.h"
#include "AskForName.h"
#include "ColourMenu.h"
#include "engine/Beats.h"

#include "dsp/sampler/SamplerProcessor.h"
#include "model/Automation.h"
#include "model/ModelJson.h"
#include "project/AudioImport.h"
#include "project/ProjectFiles.h"

#include "Browser.h"
#include "Theme.h"
#include "commands/CommandRegistry.h"

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

model::Ticks barTicks (const model::Project& p)
{
    const auto& sig = p.settings.timeSignature;
    return model::ticksPerQuarterNote * 4 * sig.numerator / juce::jmax (1, sig.denominator);
}

/** The grid choices: name and ticks (-1: the song's bar, 0: none). */
const std::pair<const char*, model::Ticks> snapChoices[] = {
    { "Bar", -1 },
    { "Beat", model::ticksPerQuarterNote },
    { "1/2 beat", model::ticksPerQuarterNote / 2 },
    { "1/4 beat (step)", model::ticksPerQuarterNote / 4 },
    { "1/3 beat (triplet)", model::ticksPerQuarterNote / 3 },
    { "No grid", 0 },
};
} // namespace

//==============================================================================
class Playlist::Lanes final : public juce::Component
{
public:
    explicit Lanes (Playlist& p) : playlist (p) { setWantsKeyboardFocus (true); }

    juce::Rectangle<float> clipBounds (int row, model::Ticks start, model::Ticks length) const
    {
        const auto x = playlist.tickToX (start);
        return { x, (float)(row * playlist.rowHeight - playlist.scrollY) + 1.0f,
                 juce::jmax (4.0f, playlist.tickToX (start + length) - x - 1.0f), (float)playlist.rowHeight - 2.0f };
    }

    /** For a lane in a folded group: the row of the folded group it is shown in, or -1. */
    int rowOfFoldedGroup (model::LaneId lane) const
    {
        const auto& project = playlist.session.getProject();
        int depth = 0;
        for (const auto* l = project.findLane (lane); l != nullptr && l->group.isValid() && depth < 64;
             l = project.findLane (l->group), ++depth)
            if (const auto row = rowOfLane (l->group); row >= 0)
                return row;
        return -1;
    }

    /** The row a lane is shown on; -1 for one in a folded group. */
    int rowOfLane (model::LaneId lane) const
    {
        const auto& lanes = playlist.session.getProject().playlist.lanes;
        for (size_t i = 0; i < lanes.size(); ++i)
            if (lanes[i].id == lane)
                return playlist.rowOfLaneIndex ((int)i);
        return -1;
    }

    const model::Clip* clipAt (juce::Point<float> p) const
    {
        const auto& clips = playlist.session.getProject().playlist.clips;
        for (auto it = clips.rbegin(); it != clips.rend(); ++it)
            if (clipBounds (rowOfLane (it->lane), it->start, it->length).contains (p))
                return &*it;
        return nullptr;
    }

    void paintClip (juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> r, bool ghost)
    {
        if (std::holds_alternative<model::AudioClip> (clip.content))
            paintAudioClip (g, clip, r, ghost);
        else if (std::holds_alternative<model::AutomationClip> (clip.content))
            paintAutomationClip (g, clip, r, ghost);
        else if (std::holds_alternative<model::PatternClip> (clip.content))
            paintPatternClip (g, clip, r, ghost);
    }

    void paintAudioClip (juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> r, bool ghost)
    {
        const auto& project = playlist.session.getProject();
        const auto& audio = std::get<model::AudioClip> (clip.content);
        const auto* channel = project.findChannel (audio.channel);
        const auto* lane = project.findLane (clip.lane);
        const bool muted = clip.muted || (lane != nullptr && project.isLaneMuted (lane->id));
        const auto colour = channel != nullptr ? juce::Colour (channel->colour) : juce::Colour (0xff5c8dd6);
        const auto file = playlist.session.resolveAudioFile (audio.file);
        const bool missing = ! file.existsAsFile();

        g.setColour ((missing ? juce::Colour (0xff8a3030) : colour).withAlpha (ghost ? 0.4f : muted ? 0.3f : 0.75f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour.darker (0.6f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        const auto name = file.getFileNameWithoutExtension().isNotEmpty() ? file.getFileNameWithoutExtension()
                                                                          : juce::String ("Audio");
        if (! missing)
            if (auto* thumbnail = playlist.getThumbnail (file);
                thumbnail != nullptr && thumbnail->getTotalLength() > 0.0)
            {
                // The part of the file the clip plays: from its offset, for its length, at its speed; a reversed
                // clip plays the file from the end, so it is drawn mirrored.
                const auto secondsPerTick = 60.0 / (project.settings.tempoBpm * (double)model::ticksPerQuarterNote);
                const auto speed = model::getPlaybackSpeed (audio);
                const auto fileLength = thumbnail->getTotalLength();
                auto fileStart = (double)audio.offset * secondsPerTick * speed;
                auto fileEnd = fileStart + (double)clip.length * secondsPerTick * speed;
                if (audio.reversed)
                    std::tie (fileStart, fileEnd) = std::pair { fileLength - fileEnd, fileLength - fileStart };
                const auto area = r.reduced (1.0f, 14.0f).withTrimmedBottom (-12.0f);
                juce::Graphics::ScopedSaveState state (g);
                if (audio.reversed)
                    g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f, area.getCentreX(), 0.0f));
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                thumbnail->drawChannels (g, area.toNearestInt(), fileStart, fileEnd,
                                         juce::Decibels::decibelsToGain (audio.gainDb));
            }

        // What is changed, after the name: "Vocal  <<  +5  x2".
        juce::StringArray changes;
        if (audio.reversed)
            changes.add (juce::String::fromUTF8 ("\u25c0"));
        if (audio.stretchMode != "off" && audio.stretchMode != "tempo-sync")
        {
            if (const auto st = juce::roundToInt (audio.pitchSemitones); st != 0)
                changes.add ((st > 0 ? "+" : "") + juce::String (st));
            if (std::abs (audio.speed - 1.0) > 0.001)
                changes.add (juce::String::fromUTF8 ("\u00d7") +
                             juce::String (audio.speed, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
        }
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ((missing ? "Missing: " + name : name) +
                        (changes.isEmpty() ? "" : "   " + changes.joinIntoString ("  ")),
                    r.reduced (4.0f, 1.0f).withHeight (13.0f), juce::Justification::topLeft, true);
    }

    /** Where an automation point is drawn inside its clip's rectangle. */
    juce::Point<float> pointPosition (const model::Clip& clip, const model::AutomationPoint& point,
                                      juce::Rectangle<float> r) const
    {
        const auto area = r.reduced (2.0f, 4.0f).withTrimmedTop (12.0f);
        return { playlist.tickToX (clip.start + point.time), area.getBottom() - point.value * area.getHeight() };
    }

    std::pair<model::Ticks, float> pointAt (const model::Clip& clip, juce::Point<float> p, juce::Rectangle<float> r,
                                            bool free) const
    {
        const auto area = r.reduced (2.0f, 4.0f).withTrimmedTop (12.0f);
        auto time = playlist.xToTick (p.x) - clip.start;
        if (! free)
            time = playlist.snapTicks (time);
        return { juce::jlimit<model::Ticks> (0, clip.length, time),
                 juce::jlimit (0.0f, 1.0f, (area.getBottom() - p.y) / juce::jmax (1.0f, area.getHeight())) };
    }

    /** The point under the mouse (within 6 pixels), or -1. */
    int pointIndexAt (const model::Clip& clip, juce::Point<float> p) const
    {
        const auto* automation = std::get_if<model::AutomationClip> (&clip.content);
        if (automation == nullptr)
            return -1;
        const auto r = clipBounds (rowOfLane (clip.lane), clip.start, clip.length);
        for (size_t i = 0; i < automation->points.size(); ++i)
            if (pointPosition (clip, automation->points[i], r).getDistanceFrom (p) < 6.0f)
                return (int)i;
        return -1;
    }

    void paintAutomationClip (juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> r, bool ghost)
    {
        const auto& project = playlist.session.getProject();
        const auto& automation = std::get<model::AutomationClip> (clip.content);
        const auto* lane = project.findLane (clip.lane);
        const bool muted = clip.muted || (lane != nullptr && project.isLaneMuted (lane->id));
        const auto colour = juce::Colour (0xff9a7ad6);

        g.setColour (colour.withAlpha (ghost ? 0.3f : muted ? 0.2f : 0.45f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour.darker (0.5f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        // The curve as it plays (bends included): held flat before the first point and after the last.
        const auto& points = automation.points;
        if (! points.empty())
        {
            juce::Path curve;
            const auto area = r.reduced (2.0f, 4.0f).withTrimmedTop (12.0f);
            const auto steps = juce::jmax (1, (int)(r.getWidth() / 2.0f));
            for (int i = 0; i <= steps; ++i)
            {
                const auto x = r.getX() + r.getWidth() * (float)i / (float)steps;
                const auto t = (double)(playlist.xToTick (x) - clip.start);
                const auto y = area.getBottom() - model::automationValueAt (points, t) * area.getHeight();
                if (i == 0)
                    curve.startNewSubPath (x, y);
                else
                    curve.lineTo (x, y);
            }

            g.setColour (juce::Colours::white.withAlpha (muted ? 0.4f : 0.9f));
            g.strokePath (curve, juce::PathStrokeType (1.5f));
            for (auto& point : points)
                if (point.time <= clip.length)
                    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (pointPosition (clip, point, r)));
        }

        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (playlist.session.describeTarget (automation.target), r.reduced (4.0f, 1.0f).withHeight (13.0f),
                    juce::Justification::topLeft, true);
    }

    void paintPatternClip (juce::Graphics& g, const model::Clip& clip, juce::Rectangle<float> r, bool ghost)
    {
        const auto& theme = themeOf (*this);
        const auto& project = playlist.session.getProject();
        const auto& pc = std::get<model::PatternClip> (clip.content);
        const auto* pattern = project.findPattern (pc.pattern);
        const auto* lane = project.findLane (clip.lane);
        const bool muted = clip.muted || (lane != nullptr && project.isLaneMuted (lane->id));
        const auto colour = pattern != nullptr ? juce::Colour (pattern->colour) : theme.get ("accentAlt");

        g.setColour (colour.withAlpha (ghost ? 0.5f : muted ? 0.35f : 0.85f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour.darker (0.6f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);

        if (pattern == nullptr)
            return;

        // A small picture of the notes, so patterns can be told apart at a glance.
        int low = 127, high = 0;
        for (auto& [channel, notes] : pattern->notes)
            for (auto& n : notes)
            {
                low = juce::jmin (low, n.pitch);
                high = juce::jmax (high, n.pitch);
            }

        const auto notesArea = r.reduced (3.0f).withTrimmedTop (14.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        for (auto& [channel, notes] : pattern->notes)
            for (auto& n : notes)
            {
                if (n.start < pc.offset || n.start >= pc.offset + clip.length)
                    continue;
                const auto x = playlist.tickToX (clip.start + n.start - pc.offset);
                const auto w = juce::jmax (1.5f, playlist.tickToX (clip.start + n.start - pc.offset + n.length) - x);
                const auto y =
                    notesArea.getBottom() - 2.0f -
                    (high > low ? (float)(n.pitch - low) / (float)(high - low) : 0.5f) * (notesArea.getHeight() - 2.0f);
                g.fillRect (x, y, juce::jmin (w, r.getRight() - x), 2.0f);
            }

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (pattern->name, r.reduced (4.0f, 1.0f).withHeight (13.0f), juce::Justification::topLeft, true);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto& project = playlist.session.getProject();
        const auto w = (float)getWidth(), h = (float)getHeight();

        for (int row = 0; row < playlist.getNumRows(); ++row)
        {
            const auto y = (float)(row * playlist.rowHeight - playlist.scrollY);
            // Each track faintly in its colour.
            auto background = row % 2 == 0 ? theme.get ("panel") : theme.get ("panel").brighter (0.04f);
            if (const auto index = playlist.laneIndexOfRow (row); index < (int)project.playlist.lanes.size())
                background =
                    ColourMenu::tint (background, juce::Colour (project.playlist.lanes[(size_t)index].colour), 0.10f);
            g.setColour (background);
            g.fillRect (0.0f, y, w, (float)playlist.rowHeight);
            g.setColour (theme.get ("border"));
            g.drawHorizontalLine ((int)(y + (float)playlist.rowHeight) - 1, 0.0f, w);
        }

        const auto bar = barTicks (project);
        const auto grid = playlist.getGridStep();
        for (auto t = (playlist.xToTick (0.0f) / grid) * grid; playlist.tickToX (t) < w; t += grid)
        {
            g.setColour (theme.get ("border").withAlpha (t % bar == 0 ? 1.0f : 0.35f));
            g.drawVerticalLine ((int)playlist.tickToX (t), 0.0f, h);
        }

        // The loop region (right-drag on the ruler), faintly across the tracks.
        if (const auto region = playlist.getShownLoop())
        {
            const auto a = playlist.tickToX (region->start), b = playlist.tickToX (region->end);
            g.setColour (theme.get ("accentAlt").withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (a, 0.0f, b - a, h));
            g.setColour (theme.get ("accentAlt").withAlpha (0.6f));
            g.drawVerticalLine ((int)a, 0.0f, h);
            g.drawVerticalLine ((int)b, 0.0f, h);
        }

        // Markers (on the ruler), as faint lines across the tracks.
        g.setColour (theme.get ("accent").withAlpha (0.35f));
        for (auto& marker : project.playlist.markers)
            g.drawVerticalLine ((int)playlist.tickToX (marker.at), 0.0f, h);

        const bool dragging = mode == Mode::move || mode == Mode::resize;
        for (auto& clip : project.playlist.clips)
        {
            if ((dragging && dragged.contains (clip.id)) || (mode == Mode::erase && erased.contains (clip.id)))
                continue;
            const auto& shown = pointDrag.active && clip.id == pointDrag.id ? pointDrag.clip : clip;
            const auto row = rowOfLane (clip.lane);
            if (row < 0)
            {
                // In a folded group: faintly on the group's row, so the song can still be read.
                if (const auto groupRow = rowOfFoldedGroup (clip.lane); groupRow >= 0)
                    paintClip (g, shown, clipBounds (groupRow, clip.start, clip.length), true);
                continue;
            }
            const auto r = clipBounds (row, clip.start, clip.length);
            paintClip (g, shown, r, false);
            if (playlist.selection.contains (clip.id))
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 2.0f);
            }
        }

        // What the mouse is doing: the clips being moved or resized, a new clip, painted clips.
        for (auto& ghost : ghosts())
        {
            const auto r = clipBounds (ghost.row, ghost.clip.start, ghost.clip.length);
            paintClip (g, ghost.clip, r, true);
            if (playlist.selection.contains (ghost.clip.id))
            {
                g.setColour (juce::Colours::white.withAlpha (0.8f));
                g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 2.0f);
            }
        }

        if (mode == Mode::selectBox)
        {
            g.setColour (theme.get ("accent").withAlpha (0.15f));
            g.fillRect (box);
            g.setColour (theme.get ("accent"));
            g.drawRect (box, 1.0f);
        }

        if (mode == Mode::slice)
        {
            const auto [first, last] = sliceRows();
            const auto x = playlist.tickToX (sliceAt);
            g.setColour (theme.get ("danger"));
            g.drawLine (x, (float)(first * playlist.rowHeight - playlist.scrollY), x,
                        (float)((last + 1) * playlist.rowHeight - playlist.scrollY), 2.0f);
        }

        if (playlist.playheadTicks >= 0.0)
        {
            g.setColour (theme.get ("text"));
            g.drawVerticalLine ((int)playlist.tickToX ((model::Ticks)playlist.playheadTicks), 0.0f, h);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto* clip = clipAt (e.position);
        const bool edge =
            clip != nullptr &&
            clipBounds (rowOfLane (clip->lane), clip->start, clip->length).getRight() - e.position.x < 6.0f;
        setMouseCursor (playlist.tool == Tool::slice ? juce::MouseCursor::IBeamCursor
                        : edge                       ? juce::MouseCursor::LeftRightResizeCursor
                        : clip != nullptr            ? juce::MouseCursor::DraggingHandCursor
                                                     : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        mode = Mode::none;
        from = e.position;
        delta = {};
        const auto* clip = clipAt (e.position);

        // FL: a left click on a pattern clip makes its pattern the current one, whatever the tool, and the
        // click still does what the tool does (move, resize, slice, select). The toolbar's pattern list, the
        // Channel Rack and the Piano Roll follow it. It is view state, not an undo step.
        if (clip != nullptr && ! e.mods.isPopupMenu())
            if (auto* pc = std::get_if<model::PatternClip> (&clip->content))
                playlist.session.setCurrentPattern (pc->pattern);

        // Automation clips: double-click opens the Automation editor; right-click removes a point, drag
        // moves one.
        if (clip != nullptr && std::holds_alternative<model::AutomationClip> (clip->content) && ! e.mods.isCtrlDown() &&
            ! e.mods.isShiftDown() && playlist.tool != Tool::slice)
        {
            const auto index = pointIndexAt (*clip, e.position);
            if (e.mods.isPopupMenu() && index >= 0)
            {
                playlist.removeAutomationPoint (clip->id, index);
                return;
            }
            if (! e.mods.isPopupMenu() && e.getNumberOfClicks() > 1)
            {
                if (playlist.onOpenAutomationClip)
                    playlist.onOpenAutomationClip (clip->id);
                return;
            }
            if (! e.mods.isPopupMenu() && index >= 0)
            {
                pointDrag = { true, clip->id, index, *clip };
                return;
            }
        }

        // Shift+right-click: the clip's menu.
        if (e.mods.isPopupMenu() && e.mods.isShiftDown() && clip != nullptr)
        {
            showClipMenu (*clip);
            return;
        }

        // The right button deletes every clip it touches (FL).
        if (e.mods.isPopupMenu())
        {
            mode = Mode::erase;
            erased.clear();
            eraseAt (e.position);
            return;
        }

        if (clip != nullptr && e.getNumberOfClicks() > 1 &&
            ! std::holds_alternative<model::AutomationClip> (clip->content))
        {
            // A pattern clip's pattern is current already (above); an audio clip selects the channel it
            // plays through and opens it.
            if (auto* audio = std::get_if<model::AudioClip> (&clip->content))
            {
                playlist.session.setSelectedChannel (audio->channel);
                if (playlist.onOpenAudioClip)
                    playlist.onOpenAudioClip (clip->id);
            }
            return;
        }

        if (e.mods.isCtrlDown() || (playlist.tool == Tool::select && clip == nullptr))
        {
            mode = Mode::selectBox;
            box = {};
            return;
        }

        if (clip != nullptr && e.mods.isShiftDown())
        {
            auto selection = playlist.selection;
            if (! selection.erase (clip->id))
                selection.insert (clip->id);
            playlist.setSelection (selection);
            return;
        }

        if (playlist.tool == Tool::slice)
        {
            mode = Mode::slice;
            sliceAt = snapped (playlist.xToTick (e.position.x), e.mods);
            sliceTo = e.position;
            repaint();
            return;
        }

        if (clip != nullptr)
        {
            // A clip that is not selected moves on its own; a selected one moves the whole selection.
            if (! playlist.selection.contains (clip->id))
                playlist.setSelection ({ clip->id });
            dragged = playlist.selection;
            anchor = *clip;
            mode = clipBounds (rowOfLane (clip->lane), clip->start, clip->length).getRight() - e.position.x < 6.0f
                       ? Mode::resize
                       : Mode::move;
            stretching = mode == Mode::resize && e.mods.isAltDown(); // FL: Alt+drag the end stretches audio
            return;
        }

        // Empty space: the current pattern, placed here (Draw: one, dragged into place; Paint: one after
        // another as far as the mouse goes).
        const auto* pattern = playlist.session.getProject().findPattern (playlist.session.getCurrentPattern());
        if (pattern == nullptr)
            return;
        playlist.clearSelection();
        mode = playlist.tool == Tool::paint ? Mode::paint : Mode::add;
        addRow = playlist.rowAtY (e.position.y);
        addStart = juce::jmax<model::Ticks> (0, snapped (playlist.xToTick (e.position.x), e.mods));
        addEnd = addStart;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pointDrag.active)
        {
            // Shown at once; written as one undo step on release.
            auto& automation = std::get<model::AutomationClip> (pointDrag.clip.content);
            const auto& original =
                std::get<model::AutomationClip> (playlist.session.getProject().findClip (pointDrag.id)->content);
            automation.points = original.points;
            const auto [time, value] =
                pointAt (pointDrag.clip, e.position,
                         clipBounds (rowOfLane (pointDrag.clip.lane), pointDrag.clip.start, pointDrag.clip.length),
                         e.mods.isAltDown());
            automation.points[(size_t)pointDrag.index] = { time, value,
                                                           automation.points[(size_t)pointDrag.index].curve };
            pointDrag.moved = true;
            repaint();
            return;
        }

        const auto ticks = playlist.xToTick (e.position.x) - playlist.xToTick (from.x);
        switch (mode)
        {
        case Mode::move:
        {
            // The clip under the mouse lands on the grid; the others keep their distance to it.
            auto deltaTicks = snapped (anchor.start + ticks, e.mods) - anchor.start;
            auto deltaRows = playlist.rowAtY (e.position.y) - playlist.rowAtY (from.y);
            for (auto& clip : playlist.session.getProject().playlist.clips)
                if (dragged.contains (clip.id))
                {
                    deltaTicks = juce::jmax (deltaTicks, -clip.start);
                    deltaRows = juce::jmax (deltaRows, -rowOfLane (clip.lane));
                }
            delta = { deltaTicks, deltaRows };
            break;
        }
        case Mode::resize:
        {
            const auto end = snapped (anchor.start + anchor.length + ticks, e.mods);
            auto deltaTicks = end - (anchor.start + anchor.length);
            const auto minimum = e.mods.isAltDown() ? (model::Ticks)1 : playlist.getGridStep();
            for (auto& clip : playlist.session.getProject().playlist.clips)
                if (dragged.contains (clip.id))
                    deltaTicks = juce::jmax (deltaTicks, juce::jmin<model::Ticks> (0, minimum - clip.length));
            delta = { deltaTicks, 0 };
            break;
        }
        case Mode::add:
            addRow = playlist.rowAtY (e.position.y);
            addStart = juce::jmax<model::Ticks> (0, snapped (playlist.xToTick (e.position.x), e.mods));
            break;
        case Mode::paint:
            addEnd = juce::jmax<model::Ticks> (addStart, playlist.xToTick (e.position.x));
            break;
        case Mode::erase:
            eraseAt (e.position);
            break;
        case Mode::selectBox:
            box = juce::Rectangle<float> (from, e.position);
            break;
        case Mode::slice:
            sliceTo = e.position;
            break;
        case Mode::none:
            return;
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (pointDrag.active)
        {
            const auto d = pointDrag;
            pointDrag = {};
            if (d.moved)
            {
                const auto& point = std::get<model::AutomationClip> (d.clip.content).points[(size_t)d.index];
                playlist.moveAutomationPoint (d.id, d.index, point.time, point.value);
            }
            repaint();
            return;
        }

        const auto m = std::exchange (mode, Mode::none);
        repaint();
        switch (m)
        {
        case Mode::add:
            playlist.placePattern (addRow, addStart);
            break;
        case Mode::paint:
            playlist.paintPattern (addRow, addStart, addEnd);
            break;
        case Mode::move:
            if (delta.ticks != 0 || delta.rows != 0)
                playlist.moveClips (dragged, delta.ticks, delta.rows);
            else if (e.mods.isAltDown() && playlist.hasOtherTakes (anchor.id))
                playlist.useTake (anchor.id); // comping: Alt+click a take
            break;
        case Mode::resize:
            if (delta.ticks != 0 && stretching)
                playlist.stretchClips (dragged, delta.ticks);
            else if (delta.ticks != 0)
                playlist.resizeClips (dragged, delta.ticks);
            break;
        case Mode::erase:
            playlist.deleteClips (erased);
            break;
        case Mode::selectBox:
        {
            const auto b = juce::Rectangle<float> (from, e.position);
            playlist.selectArea (playlist.xToTick (b.getX()), playlist.xToTick (b.getRight()),
                                 playlist.rowAtY (b.getY()), playlist.rowAtY (b.getBottom()), e.mods.isShiftDown());
            break;
        }
        case Mode::slice:
        {
            sliceTo = e.position;
            const auto [first, last] = sliceRows();
            playlist.slice (sliceAt, first, last);
            break;
        }
        case Mode::none:
            break;
        }
        dragged.clear();
        erased.clear();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            return playlist.deleteSelection() || ! playlist.selection.empty();
        if (key == juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0))
        {
            playlist.selectAll();
            return true;
        }
        if (key == juce::KeyPress::escapeKey && ! playlist.selection.empty())
        {
            playlist.clearSelection();
            return true;
        }
        return false;
    }

private:
    using Tool = Playlist::Tool;
    enum class Mode
    {
        none,
        add,
        paint,
        move,
        resize,
        erase,
        selectBox,
        slice
    };

    struct Ghost
    {
        model::Clip clip;
        int row = 0;
    };

    model::Ticks snapped (model::Ticks t, const juce::ModifierKeys& mods) const
    {
        return mods.isAltDown() ? t : playlist.snapTicks (t);
    }

    /** The rows the slice line crosses: from where the mouse went down to where it is. */
    std::pair<int, int> sliceRows() const
    {
        const auto a = playlist.rowAtY (from.y), b = playlist.rowAtY (sliceTo.y < 0.0f ? from.y : sliceTo.y);
        return { juce::jmin (a, b), juce::jmax (a, b) };
    }

    std::vector<Ghost> ghosts() const
    {
        std::vector<Ghost> result;
        const auto& project = playlist.session.getProject();
        if (mode == Mode::move || mode == Mode::resize)
            for (auto& clip : project.playlist.clips)
                if (dragged.contains (clip.id))
                {
                    auto ghost = clip;
                    if (mode == Mode::move)
                        ghost.start += delta.ticks;
                    else
                        ghost.length += delta.ticks;
                    result.push_back ({ ghost, rowOfLane (clip.lane) + delta.rows });
                }

        if (mode == Mode::add || mode == Mode::paint)
            if (const auto* pattern = project.findPattern (playlist.session.getCurrentPattern()))
                for (auto start : playlist.paintPositions (addStart, mode == Mode::paint ? addEnd : addStart))
                    result.push_back (
                        { { {}, {}, start, pattern->length, false, model::PatternClip { pattern->id, 0 } }, addRow });
        return result;
    }

    void showClipMenu (const model::Clip& clip)
    {
        juce::PopupMenu menu;
        const auto id = clip.id;
        auto safe = juce::Component::SafePointer<Playlist> (&playlist);
        if (const auto* pc = std::get_if<model::PatternClip> (&clip.content))
        {
            const auto* pattern = playlist.session.getProject().findPattern (pc->pattern);
            menu.addSectionHeader (pattern != nullptr ? pattern->name : juce::String ("Pattern"));
            menu.addItem ("Make unique (a copy of the pattern for this clip)",
                          [safe, id]
                          {
                              if (safe != nullptr)
                                  safe->makeClipUnique (id);
                          });
        }
        else if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
        {
            addAudioItems (menu, *audio, id);
        }
        menu.addItem ("Delete clip",
                      [safe, id]
                      {
                          if (safe != nullptr)
                              safe->deleteClip (id);
                      });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    /** Reverse, pitch and speed for an audio clip. */
    void addAudioItems (juce::PopupMenu& menu, const model::AudioClip& audio, model::ClipId id)
    {
        auto safe = juce::Component::SafePointer<Playlist> (&playlist);
        auto set = [safe, id] (const juce::NamedValueSet& changes)
        {
            if (safe != nullptr)
                safe->setAudioPlayback (id, changes);
        };
        menu.addSectionHeader (juce::File (audio.file).getFileNameWithoutExtension());
        if (playlist.hasOtherTakes (id))
            menu.addItem ("Use this take here (Alt+click)",
                          [safe, id]
                          {
                              if (safe != nullptr)
                                  safe->useTake (id);
                          });
        menu.addItem ("Reverse", true, audio.reversed,
                      [set, reversed = audio.reversed] { set ({ { "reversed", ! reversed } }); });

        juce::PopupMenu pitch;
        for (int semitones : { 12, 7, 5, 2, 1, 0, -1, -2, -5, -7, -12 })
            pitch.addItem (semitones == 0 ? juce::String ("Original pitch")
                                          : (semitones > 0 ? "+" : "") + juce::String (semitones) +
                                                (std::abs (semitones) == 1 ? " semitone" : " semitones"),
                           true, juce::roundToInt (audio.pitchSemitones) == semitones,
                           [set, semitones] { set ({ { "pitch", semitones }, { "fit", true } }); });
        menu.addSubMenu ("Pitch", pitch);

        juce::PopupMenu speed;
        for (auto [label, value] :
             { std::pair { "Half speed (twice as long)", 0.5 }, std::pair { "Slower (3/4)", 0.75 },
               std::pair { "Original speed", 1.0 }, std::pair { "Faster (1.5x)", 1.5 },
               std::pair { "Double speed (half as long)", 2.0 } })
            speed.addItem (label, true, std::abs (audio.speed - value) < 0.001,
                           [set, value] { set ({ { "speed", value }, { "fit", true } }); });
        speed.addSeparator();
        speed.addItem ("Tip: Alt+drag the clip's end to stretch it to any length", false, false, nullptr);
        menu.addSubMenu ("Speed", speed);

        juce::PopupMenu how;
        for (auto [stretchMode, label] : { std::pair { "stretch", "Pitch and speed apart (stretch)" },
                                           std::pair { "stretch-drums", "Pitch and speed apart, for drums and hits" },
                                           std::pair { "resample", "Together, like a record (resample)" },
                                           std::pair { "off", "Off: as recorded" } })
        {
            const bool current = audio.stretchMode == stretchMode ||
                                 (audio.stretchMode == "tempo-sync" && juce::String (stretchMode) == "off");
            how.addItem (label, true, current,
                         [set, m = juce::String (stretchMode)] { set ({ { "stretch", m }, { "fit", true } }); });
        }
        menu.addSubMenu ("Pitch and speed", how);
        menu.addSeparator();
    }

    void eraseAt (juce::Point<float> p)
    {
        if (const auto* clip = clipAt (p))
            erased.insert (clip->id);
        repaint();
    }

    struct Delta
    {
        model::Ticks ticks = 0;
        int rows = 0;
    };

    struct PointDrag
    {
        bool active = false;
        model::ClipId id;
        int index = -1;
        model::Clip clip; // a copy, with the point where the mouse is
        bool moved = false;
    };

    Playlist& playlist;
    Mode mode = Mode::none;
    juce::Point<float> from, sliceTo { -1.0f, -1.0f };
    model::Clip anchor; // the clip under the mouse when a move or resize started
    bool stretching = false;
    std::set<model::ClipId> dragged, erased;
    Delta delta;
    int addRow = 0;
    model::Ticks addStart = 0, addEnd = 0, sliceAt = 0;
    juce::Rectangle<float> box;
    PointDrag pointDrag;
};

//==============================================================================
class Playlist::Headers final : public juce::Component
{
public:
    explicit Headers (Playlist& p) : playlist (p)
    {
        setHelpText ("Playlist tracks: click the light to mute a track, the arrow to fold a group; right-click a "
                     "track to rename, colour, group or delete it");
    }

    /** How far a track's arrow and name are indented: 12 pixels per group it is in. */
    static float indentOf (int depth) { return 24.0f + 12.0f * (float)depth; }

    int countGroupedUnder (model::LaneId id) const
    {
        const auto& project = playlist.session.getProject();
        return (int)std::count_if (project.playlist.lanes.begin(), project.playlist.lanes.end(),
                                   [&] (const model::Lane& l) { return project.isLaneGroupedUnder (l.id, id); });
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto& project = playlist.session.getProject();
        const auto& lanes = project.playlist.lanes;
        g.fillAll (theme.get ("panelHeader"));

        for (int row = 0; row < playlist.getNumRows(); ++row)
        {
            const auto r = juce::Rectangle<float> (0.0f, (float)(row * playlist.rowHeight - playlist.scrollY),
                                                   (float)getWidth(), (float)playlist.rowHeight);
            const auto index = playlist.laneIndexOfRow (row);
            const bool real = index < (int)lanes.size();
            const auto* lane = real ? &lanes[(size_t)index] : nullptr;
            const bool muted = lane != nullptr && lane->muted;
            const bool mutedByGroup = lane != nullptr && ! muted && project.isLaneMuted (lane->id);
            g.setColour (real ? (muted || mutedByGroup ? theme.get ("textDim").withAlpha (muted ? 0.3f : 0.55f)
                                                       : juce::Colour (0xff6ccf6a))
                              : theme.get ("textDim").withAlpha (0.15f));
            g.fillEllipse (8.0f, r.getCentreY() - 5.0f, 10.0f, 10.0f);

            auto x = 24.0f;
            juce::String name = "Track " + juce::String (row + 1);
            auto textColour = real ? theme.get ("text") : theme.get ("textDim");
            if (lane != nullptr)
            {
                // The track's header in its colour (right of the mute light); the group's line beside a
                // grouped track.
                const auto fill = ColourMenu::tint (theme.get ("panelHeader"), juce::Colour (lane->colour));
                g.setColour (fill);
                g.fillRect (r.withTrimmedLeft (22.0f).withTrimmedBottom (1.0f));
                textColour = ColourMenu::textOn (fill);
                const auto depth = project.getLaneDepth (lane->id);
                x = indentOf (depth);
                if (depth > 0)
                {
                    g.setColour (textColour.withAlpha (0.35f));
                    g.fillRect (juce::Rectangle<float> (x - 8.0f, r.getY(), 1.0f, r.getHeight()));
                }
                name = lane->name;
                if (const auto grouped = countGroupedUnder (lane->id); grouped > 0)
                {
                    // A group: an arrow that folds it.
                    juce::Path arrow;
                    const auto c = juce::Point<float> (x + 4.0f, r.getCentreY());
                    if (lane->collapsed)
                        arrow.addTriangle (c.translated (-3.0f, -4.0f), c.translated (-3.0f, 4.0f),
                                           c.translated (4.0f, 0.0f));
                    else
                        arrow.addTriangle (c.translated (-4.0f, -3.0f), c.translated (4.0f, -3.0f),
                                           c.translated (0.0f, 4.0f));
                    g.setColour (textColour);
                    g.fillPath (arrow);
                    if (lane->collapsed)
                        name << "  (" << grouped << ")";
                }
                x += 14.0f;
            }
            g.setColour (textColour);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (name, r.withTrimmedLeft (x), juce::Justification::centredLeft, true);
            g.setColour (theme.get ("border"));
            g.drawHorizontalLine ((int)r.getBottom() - 1, 0.0f, (float)getWidth());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto row = playlist.rowAtY (e.position.y);
        const auto& project = playlist.session.getProject();
        const auto index = playlist.laneIndexOfRow (row);
        if (index >= (int)project.playlist.lanes.size())
            return;
        if (e.mods.isPopupMenu())
            return showMenu (row);
        const auto& lane = project.playlist.lanes[(size_t)index];
        if (e.x < 22)
            playlist.setLaneMuted (row, ! lane.muted);
        else if (const auto arrowX = indentOf (project.getLaneDepth (lane.id));
                 countGroupedUnder (lane.id) > 0 && e.position.x >= arrowX - 2.0f && e.position.x <= arrowX + 12.0f)
            playlist.setLaneCollapsed (row, ! lane.collapsed);
    }

    void showMenu (int row)
    {
        const auto& project = playlist.session.getProject();
        const auto& lane = project.playlist.lanes[(size_t)playlist.laneIndexOfRow (row)];
        int clips = 0;
        for (const auto& clip : playlist.session.getProject().playlist.clips)
            clips += clip.lane == lane.id ? 1 : 0;
        const auto name = lane.name;
        auto safe = juce::Component::SafePointer<Playlist> (&playlist);
        juce::PopupMenu menu;
        menu.addSectionHeader (name);
        menu.addItem ("Rename...",
                      [safe, row, name]
                      {
                          askForName ("Rename track", name,
                                      [safe, row] (const juce::String& n)
                                      {
                                          if (safe != nullptr)
                                              safe->renameLane (row, n);
                                      });
                      });
        ColourMenu::addTo (menu, lane.colour,
                           [safe, row] (juce::uint32 c)
                           {
                               if (safe != nullptr)
                                   safe->setLaneColour (row, c);
                           });
        menu.addSeparator();
        if (row > 0)
            menu.addItem ("Group with the track above",
                          [safe, row]
                          {
                              if (safe != nullptr)
                                  safe->groupLaneWithAbove (row);
                          });
        if (lane.group.isValid())
            menu.addItem ("Ungroup",
                          [safe, row]
                          {
                              if (safe != nullptr)
                                  safe->ungroupLane (row);
                          });
        if (countGroupedUnder (lane.id) > 0)
            menu.addItem (lane.collapsed ? "Unfold group" : "Fold group",
                          [safe, row, folded = lane.collapsed]
                          {
                              if (safe != nullptr)
                                  safe->setLaneCollapsed (row, ! folded);
                          });
        menu.addSeparator();
        menu.addItem ("Delete track" + (clips > 0 ? " (and its " + juce::String (clips) + " clips)" : juce::String()),
                      [safe, row]
                      {
                          if (safe != nullptr)
                              safe->removeLane (row);
                      });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

private:
    Playlist& playlist;
};

//==============================================================================
class Playlist::Ruler final : public juce::Component
{
public:
    explicit Ruler (Playlist& p) : playlist (p)
    {
        setHelpText ("Timeline: click or drag to move the song position; right-drag to loop a part of the song, "
                     "right-click to stop looping. Double-click to add a marker; click a marker to jump to it, drag "
                     "it to move it, right-click it to rename or remove it");
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panelHeader"));
        const auto bar = barTicks (playlist.session.getProject());
        g.setFont (juce::FontOptions (11.0f));
        const auto step =
            bar * juce::jmax<model::Ticks> (
                      1, (model::Ticks)(40.0 / (playlist.pixelsPerBeat * (double)bar / model::ticksPerQuarterNote)));
        for (auto t = (playlist.xToTick (0.0f) / step) * step; playlist.tickToX (t) < (float)getWidth(); t += step)
        {
            const auto x = playlist.tickToX (t);
            g.setColour (theme.get ("textDim"));
            g.drawVerticalLine ((int)x, 0.0f, (float)getHeight());
            g.setColour (theme.get ("text"));
            g.drawText (juce::String (t / bar + 1),
                        juce::Rectangle<float> (x + 3.0f, 0.0f, 40.0f, (float)numbersHeight),
                        juce::Justification::centredLeft, false);
        }

        // Markers: a flag with the name along the bottom.
        for (auto& marker : playlist.session.getProject().playlist.markers)
        {
            const auto flag = markerBounds (marker);
            const bool dragged = draggedMarker && *draggedMarker == marker.id;
            g.setColour (theme.get ("accent").withAlpha (dragged ? 0.6f : 1.0f));
            g.drawVerticalLine ((int)flag.getX(), (float)numbersHeight, (float)getHeight());
            g.fillRoundedRectangle (flag.toFloat(), 2.0f);
            g.setColour (theme.get ("panel"));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (marker.name, flag.reduced (4, 0), juce::Justification::centredLeft, true);
        }

        if (playlist.playheadTicks >= 0.0)
        {
            const auto x = playlist.tickToX ((model::Ticks)playlist.playheadTicks);
            juce::Path marker;
            marker.addTriangle (x - 5.0f, 0.0f, x + 5.0f, 0.0f, x, 8.0f);
            g.setColour (theme.get ("accent"));
            g.fillPath (marker);
        }

        // The loop region (right-drag here): a band.
        if (const auto region = playlist.getShownLoop())
        {
            const auto a = playlist.tickToX (region->start), b = playlist.tickToX (region->end);
            g.setColour (theme.get ("accentAlt").withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (a, 0.0f, b - a, (float)getHeight()));
            g.setColour (theme.get ("accentAlt"));
            g.drawVerticalLine ((int)a, 0.0f, (float)getHeight());
            g.drawVerticalLine ((int)b, 0.0f, (float)getHeight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (const auto* marker = markerAt (e.position))
        {
            if (e.mods.isPopupMenu())
                return showMarkerMenu (*marker);
            draggedMarker = marker->id;
            draggedTo = marker->at;
            return;
        }
        settingLoop = e.mods.isPopupMenu();
        mouseDrag (e);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (markerAt (e.position) == nullptr && ! e.mods.isPopupMenu())
            playlist.addMarker (juce::jmax<model::Ticks> (0, playlist.snapTicks (playlist.xToTick (e.position.x))));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (auto marker = std::exchange (draggedMarker, std::nullopt))
        {
            // Dragged: moved there. Clicked: the song jumps to it.
            const auto* m = playlist.findMarker (*marker);
            if (m != nullptr && e.mouseWasDraggedSinceMouseDown() && draggedTo != m->at)
                playlist.moveMarker (*marker, draggedTo);
            else if (m != nullptr)
                playlist.jumpTo (m->at);
            repaint();
            return;
        }
        if (! std::exchange (settingLoop, false))
            return;
        const auto pending = std::exchange (playlist.pendingLoop, std::nullopt);
        playlist.session.setLoopRegion (
            Session::Mode::song,
            e.mouseWasDraggedSinceMouseDown() && pending && pending->end > pending->start ? pending : std::nullopt);
        repaint();
        playlist.lanes->repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggedMarker)
        {
            draggedTo = juce::jmax<model::Ticks> (0, playlist.snapTicks (playlist.xToTick (e.position.x)));
            repaint();
            return;
        }
        if (settingLoop)
        {
            auto a = playlist.snapTicks (playlist.xToTick ((float)e.getMouseDownX()));
            auto b = playlist.snapTicks (playlist.xToTick (e.position.x));
            if (b < a)
                std::swap (a, b);
            playlist.pendingLoop =
                Session::LoopRegion { juce::jmax<model::Ticks> (0, a), juce::jmax<model::Ticks> (0, b) };
            repaint();
            playlist.lanes->repaint();
            return;
        }
        auto& edit = playlist.session.getEdit();
        const auto tick = juce::jmax<model::Ticks> (0, playlist.snapTicks (playlist.xToTick (e.position.x)));
        edit.getTransport().setPosition (engine::ticksToTime (edit, (double)tick));
        playlist.timerCallback();
    }

private:
    static constexpr int numbersHeight = 16;

    juce::Rectangle<int> markerBounds (const model::Marker& marker) const
    {
        const auto at = draggedMarker && *draggedMarker == marker.id ? draggedTo : marker.at;
        const auto x = (int)playlist.tickToX (at);
        const auto width = juce::jmin (
            140,
            juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (11.0f, juce::Font::bold), marker.name) + 10);
        return { x, numbersHeight + 1, width, getHeight() - numbersHeight - 2 };
    }

    const model::Marker* markerAt (juce::Point<float> position) const
    {
        const auto& markers = playlist.session.getProject().playlist.markers;
        for (auto it = markers.rbegin(); it != markers.rend(); ++it) // the later one is drawn on top
            if (markerBounds (*it).toFloat().contains (position))
                return &*it;
        return nullptr;
    }

    void showMarkerMenu (const model::Marker& marker)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (marker.name);
        auto safe = juce::Component::SafePointer<Playlist> (&playlist);
        menu.addItem ("Rename...",
                      [safe, id = marker.id, name = marker.name]
                      {
                          askForName ("Rename marker", name,
                                      [safe, id] (const juce::String& n)
                                      {
                                          if (safe != nullptr)
                                              safe->renameMarker (id, n);
                                      });
                      });
        menu.addItem ("Remove marker",
                      [safe, id = marker.id]
                      {
                          if (safe != nullptr)
                              safe->removeMarker (id);
                      });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    Playlist& playlist;
    bool settingLoop = false;
    std::optional<model::MarkerId> draggedMarker;
    model::Ticks draggedTo = 0;
};

//==============================================================================
Playlist::Playlist (Session& s) : Panel ("playlist", "Playlist"), session (s)
{
    setHelpText ("Playlist (F5): click to place the current pattern, click a pattern clip to make it current, drag "
                 "to move, drag the right edge to resize, right-click to delete, double-click to open a clip (audio "
                 "or automation). Wheel: scroll, Ctrl+wheel: zoom");

    lanes = std::make_unique<Lanes> (*this);
    headers = std::make_unique<Headers> (*this);
    ruler = std::make_unique<Ruler> (*this);
    for (auto* c : { (juce::Component*)lanes.get(), (juce::Component*)headers.get(), (juce::Component*)ruler.get(),
                     (juce::Component*)&horizontal, (juce::Component*)&vertical })
        addAndMakeVisible (c);

    horizontal.setAutoHide (false);
    vertical.setAutoHide (false);
    horizontal.addListener (this);
    vertical.addListener (this);

    const char* toolNames[] = { "Draw", "Paint", "Slice", "Select" };
    const char* toolHelp[] = {
        "Draw: click to place the current pattern, drag clips to move them, drag their end to resize them",
        "Paint: drag along a track to place the current pattern again and again",
        "Slice: drag a line down across clips to cut them in two there",
        "Select: drag a box around clips to select them; then drag, resize or Delete them together",
    };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = toolButtons[i];
        b.setButtonText (toolNames[i]);
        b.setHelpText (toolHelp[i]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x5172);
        // Choosing a tool also "clicks" the one switched off: only the one switched on counts.
        b.onClick = [this, i]
        {
            if (toolButtons[i].getToggleState())
                setTool ((Tool)i);
        };
        addAndMakeVisible (b);
    }
    setTool (Tool::draw);

    for (int i = 0; i < (int)std::size (snapChoices); ++i)
        snapChoice.addItem (snapChoices[i].first, i + 1);
    snapChoice.setSelectedId (2, juce::dontSendNotification); // a beat
    snapChoice.setTooltip ("The grid that clips snap to (hold Alt to place freely)");
    snapChoice.onChange = [this]
    {
        const auto i = snapChoice.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (i, (int)std::size (snapChoices)))
            setSnap (snapChoices[i].second < 0 ? barTicks (session.getProject()) : snapChoices[i].second);
    };
    addAndMakeVisible (snapChoice);

    session.addChangeListener (this);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    startTimerHz (30);
}

Playlist::~Playlist()
{
    cancelPendingUpdate();
    session.getHistory().removeListener (historyListener);
    session.removeChangeListener (this);
}

//==============================================================================
int Playlist::getNumRows() const
{
    return juce::jmax (minimumRows, (int)getVisibleLanes().size() + 4,
                       (int)((scrollY + (lanes != nullptr ? lanes->getHeight() : 0)) / rowHeight) + 1);
}

//==============================================================================
juce::AudioThumbnail* Playlist::getThumbnail (const juce::File& file)
{
    auto& thumbnail = thumbnails[file.getFullPathName()];
    if (thumbnail == nullptr)
    {
        thumbnail = std::make_unique<juce::AudioThumbnail> (512, dsp::SamplerProcessor::getFormats(), thumbnailCache);
        thumbnail->setSource (new juce::FileInputSource (file));
        thumbnail->addChangeListener (this);
    }
    return thumbnail.get();
}

bool Playlist::addRecordedTake (const juce::File& file, double startSeconds, std::optional<model::ChannelId> into)
{
    const auto& project = session.getProject();
    const auto ticksPerSecond = project.settings.tempoBpm / 60.0 * (double)model::ticksPerQuarterNote;
    const auto at = (model::Ticks)std::llround (startSeconds * ticksPerSecond);
    const auto start = juce::jmax<model::Ticks> (0, at), offset = juce::jmax<model::Ticks> (0, -at);
    if (! into || project.findChannel (*into) == nullptr)
        return addAudioFile (getFirstFreeRow(), start, file, offset);

    // Into an audio track: as another take where it already has audio, otherwise on its first take's track.
    juce::String path;
    model::Ticks ticks = 0;
    if (! importAudio (file, path, ticks))
        return false;
    const auto end = start + juce::jmax<model::Ticks> (1, ticks - offset);
    const model::Clip* first = nullptr;
    bool overlaps = false;
    for (const auto& clip : project.playlist.clips)
        if (const auto* audio = std::get_if<model::AudioClip> (&clip.content);
            audio != nullptr && audio->channel == *into)
        {
            if (first == nullptr || clip.id.value < first->id.value)
                first = &clip;
            overlaps = overlaps || (clip.start < end && start < clip.start + clip.length);
        }
    auto params = object ({ { "channel", (int)into->value },
                            { "file", path },
                            { "start", (juce::int64)start },
                            { "offset", (juce::int64)offset },
                            { "length", (juce::int64)(end - start) } });
    if (overlaps)
        return perform ("clip.addTake", params);
    const auto lane = first != nullptr ? first->lane : ensureLane (getFirstFreeRow());
    if (! lane.isValid())
        return false;
    params.getDynamicObject()->setProperty ("lane", (int)lane.value);
    return perform ("clip.addAudio", params);
}

bool Playlist::useTake (model::ClipId clip)
{
    return perform ("clip.useTake", object ({ { "clip", (int)clip.value } }));
}

bool Playlist::hasOtherTakes (model::ClipId id) const
{
    const auto& project = session.getProject();
    const auto* clip = project.findClip (id);
    const auto* audio = clip != nullptr ? std::get_if<model::AudioClip> (&clip->content) : nullptr;
    if (audio == nullptr)
        return false;
    return std::any_of (project.playlist.clips.begin(), project.playlist.clips.end(),
                        [&] (const model::Clip& other)
                        {
                            const auto* a = std::get_if<model::AudioClip> (&other.content);
                            return other.id != id && a != nullptr && a->channel == audio->channel &&
                                   other.start < clip->start + clip->length && clip->start < other.start + other.length;
                        });
}

bool Playlist::importAudio (const juce::File& file, juce::String& path, model::Ticks& ticks)
{
    // Into the project first (decoded to FLAC if the engine cannot play it as it is).
    const auto folder = session.getProjectFolder();
    if (! project::isImportableAudio (file))
        return false;
    if (auto result = project::importAudio (file, folder, path); result.failed())
    {
        if (onProblem)
            onProblem (result.getErrorMessage());
        return false;
    }
    const auto imported = folder != juce::File() && ! juce::File::isAbsolutePath (path)
                              ? folder.getChildFile ("audio").getChildFile (path)
                              : juce::File (path);
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::SamplerProcessor::getFormats().createReaderFor (imported));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return false;

    // The clip's length: the file's length at the project tempo (audio plays at its own speed).
    const auto seconds = (double)reader->lengthInSamples / reader->sampleRate;
    ticks = (model::Ticks)std::llround (seconds * session.getProject().settings.tempoBpm / 60.0 *
                                        (double)model::ticksPerQuarterNote);
    return true;
}

bool Playlist::addAudioFile (int row, model::Ticks start, const juce::File& file, model::Ticks offset)
{
    juce::String path;
    model::Ticks ticks = 0;
    if (! importAudio (file, path, ticks))
        return false;

    const auto lane = ensureLane (row);
    if (! lane.isValid())
        return false;
    return perform ("clip.addAudio",
                    object ({ { "lane", (int)lane.value },
                              { "file", path },
                              { "start", (juce::int64)juce::jmax<model::Ticks> (0, start) },
                              { "offset", (juce::int64)offset },
                              { "length", (juce::int64)juce::jmax<model::Ticks> (1, ticks - offset) } }));
}

bool Playlist::setPoints (model::ClipId id, const std::vector<model::AutomationPoint>& points)
{
    juce::Array<juce::var> list;
    for (auto& p : points)
        list.add (model::pointToJson (p));
    return perform ("clip.setPoints", object ({ { "clip", (int)id.value }, { "points", list } }));
}

static const std::vector<model::AutomationPoint>* pointsOf (const model::Project& p, model::ClipId id)
{
    auto* clip = p.findClip (id);
    auto* automation = clip != nullptr ? std::get_if<model::AutomationClip> (&clip->content) : nullptr;
    return automation != nullptr ? &automation->points : nullptr;
}

bool Playlist::addAutomationPoint (model::ClipId id, model::Ticks time, float value)
{
    auto* points = pointsOf (session.getProject(), id);
    if (points == nullptr)
        return false;
    auto changed = *points;
    changed.push_back ({ time, value, 0.0f });
    return setPoints (id, changed);
}

bool Playlist::moveAutomationPoint (model::ClipId id, int index, model::Ticks time, float value)
{
    auto* points = pointsOf (session.getProject(), id);
    if (points == nullptr || ! juce::isPositiveAndBelow (index, (int)points->size()))
        return false;
    auto changed = *points;
    changed[(size_t)index].time = time;
    changed[(size_t)index].value = value;
    return setPoints (id, changed);
}

bool Playlist::removeAutomationPoint (model::ClipId id, int index)
{
    auto* points = pointsOf (session.getProject(), id);
    // The last point stays: a clip without points does nothing.
    if (points == nullptr || points->size() < 2 || ! juce::isPositiveAndBelow (index, (int)points->size()))
        return false;
    auto changed = *points;
    changed.erase (changed.begin() + index);
    return setPoints (id, changed);
}

bool Playlist::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (project::isImportableAudio (juce::File (f)))
            return true;
    return false;
}

void Playlist::filesDropped (const juce::StringArray& files, int x, int y)
{
    // Several files go on consecutive tracks, all starting where they were dropped.
    const auto p = lanes->getLocalPoint (this, juce::Point<int> (x, y)).toFloat();
    addAudioFiles (files, rowAtY (p.y), juce::jmax<model::Ticks> (0, snapTicks (xToTick (p.x))));
}

bool Playlist::isInterestedInDragSource (const SourceDetails& details)
{
    return Browser::sampleFromDrag (details.description).has_value();
}

void Playlist::itemDropped (const SourceDetails& details)
{
    if (const auto sample = Browser::sampleFromDrag (details.description))
        filesDropped ({ sample->getFullPathName() }, details.localPosition.x, details.localPosition.y);
}

int Playlist::addAudioFiles (const juce::StringArray& files, int row, model::Ticks start)
{
    int added = 0;
    juce::MouseCursor::showWaitCursor(); // MP3s, Opus and others are decoded first: a second or two each
    for (auto& f : files)
        if (addAudioFile (row + added, start, juce::File (f)))
            ++added;
    juce::MouseCursor::hideWaitCursor();
    return added;
}

int Playlist::getFirstFreeRow() const
{
    const auto& project = session.getProject();
    const auto visible = getVisibleLanes();
    int free = 0;
    for (size_t row = 0; row < visible.size(); ++row)
        if (std::any_of (project.playlist.clips.begin(), project.playlist.clips.end(),
                         [&] (auto& clip) { return clip.lane == project.playlist.lanes[(size_t)visible[row]].id; }))
            free = (int)row + 1;
    return free;
}

float Playlist::tickToX (model::Ticks t) const
{
    return (float)(((double)t - scrollTicks) / model::ticksPerQuarterNote * pixelsPerBeat);
}

model::Ticks Playlist::xToTick (float x) const
{
    return (model::Ticks)std::floor ((double)x / pixelsPerBeat * model::ticksPerQuarterNote + scrollTicks);
}

int Playlist::rowAtY (float y) const { return juce::jmax (0, (int)std::floor (((double)y + scrollY) / rowHeight)); }

model::Ticks Playlist::snapTicks (model::Ticks t) const
{
    return snap > 0 ? (model::Ticks)std::llround ((double)t / (double)snap) * snap : t;
}

void Playlist::setSnap (model::Ticks s)
{
    snap = juce::jmax<model::Ticks> (0, s);
    lanes->repaint();
}

void Playlist::setTool (Tool t)
{
    tool = t;
    toolButtons[(int)t].setToggleState (true, juce::dontSendNotification);
}

//==============================================================================
void Playlist::setSelection (std::set<model::ClipId> s)
{
    std::erase_if (s, [this] (model::ClipId id) { return session.getProject().findClip (id) == nullptr; });
    selection = std::move (s);
    lanes->repaint();
}

void Playlist::selectAll()
{
    std::set<model::ClipId> all;
    for (auto& clip : session.getProject().playlist.clips)
        all.insert (clip.id);
    setSelection (std::move (all));
}

void Playlist::selectArea (model::Ticks from, model::Ticks to, int firstRow, int lastRow, bool add)
{
    auto s = add ? selection : std::set<model::ClipId>();
    const auto& lanesList = session.getProject().playlist.lanes;
    for (auto& clip : session.getProject().playlist.clips)
    {
        const auto row =
            (int)std::distance (lanesList.begin(), std::find_if (lanesList.begin(), lanesList.end(),
                                                                 [&] (auto& lane) { return lane.id == clip.lane; }));
        if (row >= firstRow && row <= lastRow && clip.start < to && clip.start + clip.length > from)
            s.insert (clip.id);
    }
    setSelection (std::move (s));
}

//==============================================================================
bool Playlist::performAll (const juce::String& description, const juce::Array<juce::var>& steps)
{
    if (steps.isEmpty())
        return false;
    juce::String error;
    auto transaction = commands::CommandRegistry::get().createTransaction (
        object ({ { "description", description }, { "commands", steps } }), error);
    return transaction != nullptr && session.getHistory().perform (std::move (transaction)).wasOk();
}

static juce::var step (const juce::String& command, const juce::var& parameters)
{
    return object ({ { "command", command }, { "parameters", parameters } });
}

bool Playlist::moveClips (const std::set<model::ClipId>& ids, model::Ticks deltaTicks, int deltaRows)
{
    const auto rowOf = [this] (model::LaneId lane)
    {
        const auto& existing = session.getProject().playlist.lanes;
        for (size_t i = 0; i < existing.size(); ++i)
            if (existing[i].id == lane)
                return rowOfLaneIndex ((int)i);
        return -1;
    };

    // Where each clip goes; the tracks below the last one are made first.
    std::vector<std::tuple<model::ClipId, int, model::Ticks>> moves;
    int lowest = 0;
    for (auto& clip : session.getProject().playlist.clips)
        if (ids.contains (clip.id))
        {
            const auto row = rowOf (clip.lane) + deltaRows;
            const auto start = clip.start + deltaTicks;
            if (row < 0 || start < 0)
                return false;
            moves.emplace_back (clip.id, row, start);
            lowest = juce::jmax (lowest, row);
        }
    if (moves.empty() || ! ensureLane (lowest).isValid())
        return false;

    juce::Array<juce::var> steps;
    for (auto& [id, row, start] : moves)
        steps.add (
            step ("clip.set",
                  object ({ { "clip", (int)id.value },
                            { "lane", (int)session.getProject().playlist.lanes[(size_t)laneIndexOfRow (row)].id.value },
                            { "start", (juce::int64)start } })));
    return performAll (moves.size() == 1 ? "Move clip" : "Move clips", steps);
}

bool Playlist::resizeClips (const std::set<model::ClipId>& ids, model::Ticks deltaTicks)
{
    juce::Array<juce::var> steps;
    for (auto& clip : session.getProject().playlist.clips)
        if (ids.contains (clip.id))
        {
            if (clip.length + deltaTicks <= 0)
                return false;
            steps.add (step ("clip.set", object ({ { "clip", (int)clip.id.value },
                                                   { "length", (juce::int64)(clip.length + deltaTicks) } })));
        }
    return performAll (steps.size() == 1 ? "Resize clip" : "Resize clips", steps);
}

bool Playlist::setAudioPlayback (model::ClipId clip, const juce::NamedValueSet& changes)
{
    auto* params = new juce::DynamicObject();
    params->setProperty ("clip", (int)clip.value);
    for (const auto& change : changes)
        params->setProperty (change.name, change.value);
    return perform ("clip.setAudio", juce::var (params));
}

bool Playlist::stretchClips (const std::set<model::ClipId>& ids, model::Ticks deltaTicks)
{
    juce::Array<juce::var> steps;
    for (auto& clip : session.getProject().playlist.clips)
        if (ids.contains (clip.id))
        {
            if (clip.length + deltaTicks <= 0)
                return false;
            if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
            {
                // Faster to fit a shorter clip; resampled clips get higher too, like a record.
                const auto speed =
                    juce::jlimit (0.25, 4.0, audio->speed * (double)clip.length / (double)(clip.length + deltaTicks));
                steps.add (step ("clip.setAudio",
                                 object ({ { "clip", (int)clip.id.value }, { "speed", speed }, { "fit", true } })));
            }
            else
            {
                steps.add (step ("clip.set", object ({ { "clip", (int)clip.id.value },
                                                       { "length", (juce::int64)(clip.length + deltaTicks) } })));
            }
        }
    return performAll (steps.size() == 1 ? "Stretch clip" : "Stretch clips", steps);
}

bool Playlist::deleteClips (const std::set<model::ClipId>& ids)
{
    juce::Array<juce::var> steps;
    for (auto& clip : session.getProject().playlist.clips)
        if (ids.contains (clip.id))
            steps.add (step ("clip.remove", object ({ { "clip", (int)clip.id.value } })));
    return performAll (steps.size() == 1 ? "Delete clip" : "Delete clips", steps);
}

bool Playlist::slice (model::Ticks at, int firstRow, int lastRow)
{
    const auto& project = session.getProject();
    juce::Array<juce::var> steps;
    for (int row = juce::jmax (0, firstRow);
         row <= lastRow && laneIndexOfRow (row) < (int)project.playlist.lanes.size(); ++row)
        for (auto& clip : project.playlist.clips)
            if (clip.lane == project.playlist.lanes[(size_t)laneIndexOfRow (row)].id && clip.start < at &&
                at < clip.start + clip.length)
                steps.add (step ("clip.split", object ({ { "clip", (int)clip.id.value }, { "at", (juce::int64)at } })));
    return performAll (steps.size() == 1 ? "Slice clip" : "Slice clips", steps);
}

std::vector<model::Ticks> Playlist::paintPositions (model::Ticks from, model::Ticks to) const
{
    std::vector<model::Ticks> positions;
    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    if (pattern == nullptr || pattern->length <= 0)
        return positions;
    for (auto start = from; positions.empty() || start < to; start += pattern->length)
        positions.push_back (start);
    return positions;
}

bool Playlist::paintPattern (int row, model::Ticks from, model::Ticks to)
{
    const auto pattern = session.getCurrentPattern();
    const auto positions = paintPositions (from, to);
    if (positions.empty() || row < 0)
        return false;
    if (positions.size() == 1)
        return placePattern (row, from);

    const auto lane = ensureLane (row);
    if (! lane.isValid())
        return false;
    juce::Array<juce::var> steps;
    for (auto start : positions)
        steps.add (step ("clip.addPattern", object ({ { "lane", (int)lane.value },
                                                      { "pattern", (int)pattern.value },
                                                      { "start", (juce::int64)start } })));
    return performAll ("Paint pattern", steps);
}

//==============================================================================
bool Playlist::perform (const juce::String& command, const juce::var& parameters)
{
    juce::String error;
    auto c = commands::CommandRegistry::get().create (command, parameters, error);
    return c != nullptr && session.getHistory().perform (std::move (c)).wasOk();
}

model::LaneId Playlist::ensureLane (int row)
{
    const auto& existing = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row);
    if (index < (int)existing.size())
        return existing[(size_t)index].id;

    // Rows below the last lane are empty tracks waiting to be used: create them (one undo step).
    juce::Array<juce::var> steps;
    for (auto i = (int)existing.size(); i <= index; ++i)
        steps.add (object ({ { "command", "lane.add" }, { "parameters", object ({}) } }));

    juce::String error;
    auto transaction = commands::CommandRegistry::get().createTransaction (
        object ({ { "description", "Add playlist tracks" }, { "commands", steps } }), error);
    if (transaction == nullptr || session.getHistory().perform (std::move (transaction)).failed())
        return {};

    return session.getProject().playlist.lanes[(size_t)index].id;
}

bool Playlist::placePattern (int row, model::Ticks start)
{
    const auto pattern = session.getCurrentPattern();
    if (! pattern.isValid() || row < 0)
        return false;

    const auto lane = ensureLane (row);
    return lane.isValid() && perform ("clip.addPattern", object ({ { "lane", (int)lane.value },
                                                                   { "pattern", (int)pattern.value },
                                                                   { "start", (juce::int64)start } }));
}

bool Playlist::moveClip (model::ClipId clip, int row, model::Ticks start)
{
    const auto lane = ensureLane (row);
    return lane.isValid() &&
           perform (
               "clip.set",
               object ({ { "clip", (int)clip.value }, { "lane", (int)lane.value }, { "start", (juce::int64)start } }));
}

bool Playlist::resizeClip (model::ClipId clip, model::Ticks length)
{
    return perform ("clip.set", object ({ { "clip", (int)clip.value }, { "length", (juce::int64)length } }));
}

bool Playlist::addMarker (model::Ticks at, const juce::String& name)
{
    auto params = object ({ { "at", (juce::int64)at } });
    if (name.isNotEmpty())
        params.getDynamicObject()->setProperty ("name", name);
    return perform ("marker.add", params);
}

bool Playlist::renameMarker (model::MarkerId id, const juce::String& name)
{
    return perform ("marker.set", object ({ { "marker", (int)id.value }, { "name", name } }));
}

bool Playlist::moveMarker (model::MarkerId id, model::Ticks at)
{
    return perform ("marker.set", object ({ { "marker", (int)id.value }, { "at", (juce::int64)at } }));
}

bool Playlist::removeMarker (model::MarkerId id)
{
    return perform ("marker.remove", object ({ { "marker", (int)id.value } }));
}

const model::Marker* Playlist::findMarker (model::MarkerId id) const
{
    for (auto& m : session.getProject().playlist.markers)
        if (m.id == id)
            return &m;
    return nullptr;
}

void Playlist::jumpTo (model::Ticks at)
{
    auto& edit = session.getEdit();
    edit.getTransport().setPosition (engine::ticksToTime (edit, (double)at));
    timerCallback();
}

bool Playlist::makeClipUnique (model::ClipId clip)
{
    juce::String error;
    auto command =
        commands::CommandRegistry::get().create ("clip.makeUnique", object ({ { "clip", (int)clip.value } }), error);
    auto* raw = command.get();
    if (command == nullptr || ! session.getHistory().perform (std::move (command)).wasOk())
        return false;
    // The copy is what the Piano Roll and Channel Rack edit now.
    session.setCurrentPattern (model::PatternId { (uint32_t)(int)raw->getResult()["pattern"] });
    return true;
}

bool Playlist::deleteClip (model::ClipId clip)
{
    return perform ("clip.remove", object ({ { "clip", (int)clip.value } }));
}

bool Playlist::setLaneMuted (int row, bool muted)
{
    const auto& existing = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row);
    return row >= 0 && index < (int)existing.size() &&
           perform ("lane.set", object ({ { "lane", (int)existing[(size_t)index].id.value }, { "muted", muted } }));
}

bool Playlist::renameLane (int row, const juce::String& newName)
{
    const auto& existing = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row);
    return row >= 0 && index < (int)existing.size() &&
           perform ("lane.set", object ({ { "lane", (int)existing[(size_t)index].id.value }, { "name", newName } }));
}

bool Playlist::setLaneColour (int row, juce::uint32 argb)
{
    const auto& existing = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row);
    return row >= 0 && index < (int)existing.size() &&
           perform ("lane.set", object ({ { "lane", (int)existing[(size_t)index].id.value },
                                          { "colour", ColourMenu::toText (argb) } }));
}

bool Playlist::removeLane (int row)
{
    const auto& existing = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row);
    return row >= 0 && index < (int)existing.size() &&
           perform ("lane.remove", object ({ { "lane", (int)existing[(size_t)index].id.value } }));
}

std::vector<int> Playlist::getVisibleLanes() const
{
    const auto& project = session.getProject();
    std::vector<int> visible;
    for (size_t i = 0; i < project.playlist.lanes.size(); ++i)
        if (! project.isLaneHidden (project.playlist.lanes[i].id))
            visible.push_back ((int)i);
    return visible;
}

int Playlist::laneIndexOfRow (int row) const
{
    const auto visible = getVisibleLanes();
    if (row < (int)visible.size())
        return visible[(size_t)juce::jmax (0, row)];
    return (int)session.getProject().playlist.lanes.size() + row - (int)visible.size();
}

int Playlist::rowOfLaneIndex (int laneIndex) const
{
    const auto visible = getVisibleLanes();
    const auto it = std::find (visible.begin(), visible.end(), laneIndex);
    return it != visible.end() ? (int)(it - visible.begin()) : -1;
}

bool Playlist::groupLaneWithAbove (int row)
{
    const auto& all = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row), above = laneIndexOfRow (row - 1);
    if (row <= 0 || index >= (int)all.size())
        return false;
    // Under the track above; if that one is itself in a group with others like this one, into its group.
    const auto& upper = all[(size_t)above];
    const auto& lane = all[(size_t)index];
    const auto target = upper.group == lane.group || upper.group.isValid() == false ? upper.id : upper.group;
    return perform ("lane.set", object ({ { "lane", (int)lane.id.value }, { "group", (int)target.value } }));
}

bool Playlist::ungroupLane (int row)
{
    const auto& project = session.getProject();
    const auto index = laneIndexOfRow (row);
    if (index >= (int)project.playlist.lanes.size())
        return false;
    const auto& lane = project.playlist.lanes[(size_t)index];
    const auto* parent = project.findLane (lane.group);
    return parent != nullptr &&
           perform ("lane.set", object ({ { "lane", (int)lane.id.value }, { "group", (int)parent->group.value } }));
}

bool Playlist::setLaneCollapsed (int row, bool collapsed)
{
    const auto& all = session.getProject().playlist.lanes;
    const auto index = laneIndexOfRow (row);
    return index < (int)all.size() &&
           perform ("lane.set", object ({ { "lane", (int)all[(size_t)index].id.value }, { "collapsed", collapsed } }));
}

//==============================================================================
void Playlist::handleAsyncUpdate()
{
    // Deleted clips (or undone ones) leave the selection.
    if (std::any_of (selection.begin(), selection.end(),
                     [this] (model::ClipId id) { return session.getProject().findClip (id) == nullptr; }))
        setSelection (selection);
    updateScrollBars();
    repaint();
}

void Playlist::paint (juce::Graphics& g)
{
    g.fillAll (themeOf (*this).get ("panelHeader"));
    for (auto* c : { (juce::Component*)lanes.get(), (juce::Component*)headers.get(), (juce::Component*)ruler.get() })
        c->repaint();
}

void Playlist::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (4, 3);
    for (auto& b : toolButtons)
        b.setBounds (toolbar.removeFromLeft (58));
    toolbar.removeFromLeft (8);
    snapChoice.setBounds (toolbar.removeFromLeft (150));

    const int bar = 12;
    vertical.setBounds (area.removeFromRight (bar).withTrimmedTop (rulerHeight).withTrimmedBottom (bar));
    horizontal.setBounds (area.removeFromBottom (bar).withTrimmedLeft (headerWidth));
    ruler->setBounds (area.removeFromTop (rulerHeight).withTrimmedLeft (headerWidth));
    headers->setBounds (area.removeFromLeft (headerWidth));
    lanes->setBounds (area);
    updateScrollBars();
}

void Playlist::updateScrollBars()
{
    model::Ticks end = barTicks (session.getProject()) * 16;
    for (auto& clip : session.getProject().playlist.clips)
        end = juce::jmax (end, clip.start + clip.length + barTicks (session.getProject()) * 8);

    const auto visibleTicks = lanes->getWidth() / pixelsPerBeat * model::ticksPerQuarterNote;
    horizontal.setRangeLimits (0.0, juce::jmax ((double)end, scrollTicks + visibleTicks));
    horizontal.setCurrentRange (scrollTicks, visibleTicks, juce::dontSendNotification);
    vertical.setRangeLimits (0.0, juce::jmax ((double)getNumRows() * rowHeight, scrollY + lanes->getHeight()));
    vertical.setCurrentRange (scrollY, lanes->getHeight(), juce::dontSendNotification);
}

void Playlist::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    (bar == &horizontal ? scrollTicks : scrollY) = start;
    repaint();
}

void Playlist::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
        pixelsPerBeat = juce::jlimit (2.0, 400.0, pixelsPerBeat * (wheel.deltaY > 0 ? 1.15 : 1.0 / 1.15));
    else if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        scrollTicks =
            juce::jmax (0.0, scrollTicks - (double)(wheel.deltaX + (e.mods.isShiftDown() ? wheel.deltaY : 0.0f)) *
                                               16.0 * model::ticksPerQuarterNote);
    else
        scrollY = juce::jmax (0.0, scrollY - (double)wheel.deltaY * 3.0 * rowHeight);

    updateScrollBars();
    repaint();
}

void Playlist::timerCallback()
{
    // The song position is shown in song mode, playing or not.
    double ticks = -1.0;
    if (session.getMode() == Session::Mode::song)
    {
        auto& edit = session.getEdit();
        ticks = engine::timeToTicks (edit, edit.getTransport().getPosition());
    }

    if (std::abs (ticks - playheadTicks) > 1.0)
    {
        playheadTicks = ticks;
        lanes->repaint();
        ruler->repaint();
    }
}

} // namespace spinus::ui
