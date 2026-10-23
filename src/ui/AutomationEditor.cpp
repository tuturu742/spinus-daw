#include "AutomationEditor.h"

#include "Theme.h"
#include "commands/CommandRegistry.h"
#include "engine/Beats.h"
#include "model/ModelJson.h"

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

constexpr auto beat = model::ticksPerQuarterNote;

/** The grid choices: name and ticks (0: none). */
const std::pair<const char*, model::Ticks> snapChoices[] = {
    { "Bar", -1 }, // the time signature's bar
    { "Beat", beat },
    { "1/2 beat", beat / 2 },
    { "1/4 beat (step)", beat / 4 },
    { "1/3 beat (triplet)", beat / 3 },
    { "1/6 beat", beat / 6 },
    { "1/8 beat", beat / 8 },
    { "No grid", 0 },
};
} // namespace

//==============================================================================
/** The points and the curve. */
class AutomationEditor::Canvas final : public juce::Component
{
public:
    explicit Canvas (AutomationEditor& e) : editor (e)
    {
        setWantsKeyboardFocus (true);
        setHelpText ("Click to add a point, drag points to move them (Shift: fine), right-click to delete, drag a "
                     "diamond to bend the curve. Double-click a point to type its value");
    }

    juce::Point<float> positionOf (const model::AutomationPoint& p) const
    {
        return { editor.tickToX ((double)p.time), editor.valueToY (p.value) };
    }

    /** The point under the mouse (within 6 pixels), or -1. */
    int pointAt (juce::Point<float> p) const
    {
        const auto& points = editor.getPoints();
        int best = -1;
        float bestDistance = 6.0f;
        for (size_t i = 0; i < points.size(); ++i)
            if (const auto d = positionOf (points[i]).getDistanceFrom (p); d <= bestDistance)
            {
                best = (int)i;
                bestDistance = d;
            }
        return best;
    }

    /** Where the bend handle of the part starting at point `i` is drawn, if that part has one. */
    std::optional<juce::Point<float>> handleOf (size_t i) const
    {
        const auto& points = editor.getPoints();
        if (i + 1 >= points.size())
            return std::nullopt;
        const auto& a = points[i];
        const auto& b = points[i + 1];
        if (juce::approximatelyEqual (a.value, b.value) ||
            editor.tickToX ((double)b.time) - editor.tickToX ((double)a.time) < 18.0f)
            return std::nullopt;
        const auto middle = ((double)a.time + (double)b.time) / 2.0;
        return juce::Point<float> { editor.tickToX (middle),
                                    editor.valueToY (model::automationValueAt (points, middle)) };
    }

    int handleAt (juce::Point<float> p) const
    {
        for (size_t i = 0; i + 1 < editor.getPoints().size(); ++i)
            if (auto h = handleOf (i); h && h->getDistanceFrom (p) <= 6.0f)
                return (int)i;
        return -1;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto w = (float)getWidth(), h = (float)getHeight();
        g.fillAll (theme.get ("panel"));

        const auto length = editor.getClipLength();
        if (length <= 0)
            return;

        // Outside the clip: darker (points there are not played).
        const auto clipEnd = editor.tickToX ((double)length), clipStart = editor.tickToX (0.0);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (clipEnd, 0.0f, juce::jmax (0.0f, w - clipEnd), h));
        g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, juce::jmax (0.0f, clipStart), h));

        // Grid: values every 10% (stronger at 0, 50 and 100%); time on the snap, beats and bars.
        for (int i = 0; i <= 10; ++i)
        {
            const auto y = editor.valueToY ((float)i / 10.0f);
            g.setColour (theme.get ("border").withAlpha (i % 5 == 0 ? 0.9f : 0.35f));
            g.drawHorizontalLine ((int)y, 0.0f, w);
        }
        const auto bar = editor.getTimeSignature().barTicks();
        const auto step =
            editor.snap > 0 && editor.tickToX ((double)editor.snap) - editor.tickToX (0.0) >= 6.0f ? editor.snap : beat;
        const auto offset = editor.findClip() != nullptr ? editor.findClip()->start : 0;
        for (auto t = juce::jmax<model::Ticks> (0, editor.xToTick (0.0f) / step * step); editor.tickToX ((double)t) < w;
             t += step)
        {
            const auto song = t + offset;
            g.setColour (theme.get ("border").withAlpha (song % bar == 0 ? 1.0f : song % beat == 0 ? 0.5f : 0.2f));
            g.drawVerticalLine ((int)editor.tickToX ((double)t), 0.0f, h);
        }

        const auto& points = editor.getPoints();
        if (points.empty())
            return;

        // The curve, as it plays: one sample per pixel, flat before the first point and after the last.
        const auto colour = juce::Colour (0xff9a7ad6);
        juce::Path curve;
        for (int x = 0; x <= getWidth(); ++x)
        {
            const auto y = editor.valueToY (model::automationValueAt (points, (double)editor.xToTick ((float)x)));
            if (x == 0)
                curve.startNewSubPath ((float)x, y);
            else
                curve.lineTo ((float)x, y);
        }
        auto filled = curve;
        filled.lineTo (w, h);
        filled.lineTo (0.0f, h);
        filled.closeSubPath();
        g.setColour (colour.withAlpha (0.18f));
        g.fillPath (filled);
        g.setColour (colour.brighter (0.4f));
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        // The line being drawn with the Line tool.
        if (mode == Mode::line)
        {
            g.setColour (theme.get ("accent"));
            g.drawLine ({ editor.tickToX ((double)lineFrom.first), editor.valueToY (lineFrom.second),
                          editor.tickToX ((double)lineTo.first), editor.valueToY (lineTo.second) },
                        2.0f);
        }

        // Bend handles, then the points.
        for (size_t i = 0; i + 1 < points.size(); ++i)
            if (auto handle = handleOf (i))
            {
                juce::Path diamond;
                diamond.addRectangle (-3.5f, -3.5f, 7.0f, 7.0f);
                diamond.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi / 4.0f)
                                            .translated (handle->x, handle->y));
                g.setColour (theme.get ("panel"));
                g.fillPath (diamond);
                g.setColour (colour.brighter (0.6f).withAlpha ((int)i == hoveredHandle ? 1.0f : 0.7f));
                g.strokePath (diamond, juce::PathStrokeType (1.5f));
            }

        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto centre = positionOf (points[i]);
            const bool selected = editor.selection.contains ((int)i);
            const bool outside = points[i].time > length;
            const auto r =
                juce::Rectangle<float> (selected ? 11.0f : 9.0f, selected ? 11.0f : 9.0f).withCentre (centre);
            g.setColour (selected ? theme.get ("accent") : juce::Colours::white.withAlpha (outside ? 0.35f : 0.95f));
            g.fillEllipse (r);
            g.setColour (selected ? juce::Colours::white : colour.darker (0.4f));
            g.drawEllipse (r, (int)i == hoveredPoint ? 2.0f : 1.0f);
        }

        if (mode == Mode::selectBox)
        {
            g.setColour (theme.get ("accent").withAlpha (0.15f));
            g.fillRect (box);
            g.setColour (theme.get ("accent"));
            g.drawRect (box, 1.0f);
        }

        if (editor.playheadTicks >= 0.0)
        {
            g.setColour (theme.get ("text"));
            g.drawVerticalLine ((int)editor.tickToX (editor.playheadTicks), 0.0f, h);
        }

        // What the mouse is on (or moving): where and how much, in the control's own units.
        if (readout.isNotEmpty())
        {
            g.setFont (juce::FontOptions (12.0f));
            const auto textWidth =
                (float)juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), readout) + 12.0f;
            auto r = juce::Rectangle<float> (readoutAt.x + 12.0f, readoutAt.y - 26.0f, textWidth, 20.0f);
            if (r.getRight() > w)
                r.setX (readoutAt.x - 12.0f - textWidth);
            if (r.getY() < 0.0f)
                r.setY (readoutAt.y + 10.0f);
            g.setColour (theme.get ("panelHeader").withAlpha (0.92f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (theme.get ("text"));
            g.drawText (readout, r, juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto point = pointAt (e.position);
        const auto handle = point < 0 ? handleAt (e.position) : -1;
        if (point != hoveredPoint || handle != hoveredHandle)
        {
            hoveredPoint = point;
            hoveredHandle = handle;
        }
        if (point >= 0)
            showReadout (editor.getPoints()[(size_t)point], e.position);
        else
            showReadout ({ editor.xToTick (e.position.x), editor.yToValue (e.position.y), 0.0f }, e.position);
        setMouseCursor (point >= 0    ? juce::MouseCursor::DraggingHandCursor
                        : handle >= 0 ? juce::MouseCursor::UpDownResizeCursor
                                      : juce::MouseCursor::CrosshairCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredPoint = hoveredHandle = -1;
        readout.clear();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        mode = Mode::none;
        if (editor.getClipLength() <= 0)
            return;

        startPosition = e.position;
        original = editor.points;
        originalSelection = editor.selection;
        const auto point = pointAt (e.position);

        // Right button: delete every point it touches (FL).
        if (e.mods.isPopupMenu())
        {
            if (point < 0 && handleAt (e.position) < 0)
            {
                showMenu();
                return;
            }
            mode = Mode::erase;
            beginEditing();
            erased.clear();
            eraseAt (e.position);
            return;
        }

        if (const auto handle = point < 0 ? handleAt (e.position) : -1; handle >= 0)
        {
            if (e.getNumberOfClicks() > 1)
            {
                editor.setCurve (handle, 0.0f);
                return;
            }
            mode = Mode::bend;
            bendIndex = handle;
            beginEditing();
            return;
        }

        if (point >= 0)
        {
            if (e.getNumberOfClicks() > 1)
            {
                editor.editPoint (point);
                return;
            }
            if (e.mods.isShiftDown())
            {
                auto s = editor.selection;
                if (! s.erase (point))
                    s.insert (point);
                editor.setSelection (s);
                return;
            }
            if (! editor.selection.contains (point))
                editor.setSelection ({ point });
            mode = Mode::move;
            anchor = point;
            originalSelection = editor.selection;
            beginEditing();
            return;
        }

        if (e.mods.isCtrlDown() || editor.tool == Tool::select)
        {
            mode = Mode::selectBox;
            box = {};
            return;
        }

        const auto time = timeAt (e.position.x, e.mods);
        const auto value = editor.yToValue (e.position.y);
        switch (editor.tool)
        {
        case Tool::draw:
        {
            // A new point, dragged from here.
            beginEditing();
            editor.points.push_back ({ time, value, 0.0f });
            std::set<int> selected { (int)editor.points.size() - 1 };
            editor.sortPoints (editor.points, selected);
            editor.selection = selected;
            original = editor.points;
            originalSelection = selected;
            anchor = *selected.begin();
            mode = Mode::move;
            added = true;
            showReadout (editor.points[(size_t)anchor], e.position);
            repaint();
            break;
        }
        case Tool::line:
            mode = Mode::line;
            lineFrom = lineTo = { time, value };
            break;
        case Tool::paint:
            mode = Mode::paint;
            beginEditing();
            painted.clear();
            paintAt (e.position, e.mods);
            break;
        case Tool::select:
            break;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        switch (mode)
        {
        case Mode::move:
        {
            // Shift: fine, a tenth of the mouse's movement up and down.
            const auto& from = original[(size_t)anchor];
            const auto fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
            auto newTime = from.time + editor.xToTick (e.position.x) - editor.xToTick (startPosition.x);
            if (! e.mods.isAltDown())
                newTime = editor.snapTicks (newTime);
            const auto deltaTicks = newTime - from.time;
            const auto deltaValue = (editor.yToValue (e.position.y) - editor.yToValue (startPosition.y)) * fine;

            auto moved = original;
            auto selected = originalSelection;
            applyMove (moved, selected, deltaTicks, deltaValue);
            editor.points = std::move (moved);
            editor.selection = std::move (selected);
            dragged = true;
            if (const auto shown = findMovedAnchor(); shown >= 0)
                showReadout (editor.points[(size_t)shown], editor.canvas->positionOf (editor.points[(size_t)shown]));
            break;
        }
        case Mode::bend:
        {
            const auto& a = original[(size_t)bendIndex];
            const auto& b = original[(size_t)bendIndex + 1];
            // Up bends the curve up: for a rising part that holds the low value longer (negative).
            const auto up = (startPosition.y - e.position.y) / 80.0f * (e.mods.isShiftDown() ? 0.1f : 1.0f);
            const auto curve = juce::jlimit (-1.0f, 1.0f, a.curve + (b.value > a.value ? -up : up));
            editor.points = original;
            editor.points[(size_t)bendIndex].curve = curve;
            dragged = true;
            readout = "Bend " + juce::String (juce::roundToInt (curve * 100.0f)) + "%";
            readoutAt = e.position;
            break;
        }
        case Mode::erase:
            eraseAt (e.position);
            break;
        case Mode::selectBox:
            box = juce::Rectangle<float> (startPosition, e.position);
            break;
        case Mode::line:
            lineTo = { timeAt (e.position.x, e.mods), editor.yToValue (e.position.y) };
            readout = editor.formatTime (lineTo.first) + "  " +
                      editor.session.describeTargetValue (targetOf(), lineTo.second);
            readoutAt = e.position;
            break;
        case Mode::paint:
            paintAt (e.position, e.mods);
            break;
        case Mode::none:
            return;
        }
        editor.updateStatus();
        editor.repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto m = std::exchange (mode, Mode::none);
        switch (m)
        {
        case Mode::move:
        case Mode::bend:
        case Mode::erase:
        case Mode::paint:
            endEditing (m == Mode::move    ? (added ? "Add automation point" : "Move automation points")
                        : m == Mode::bend  ? "Bend automation curve"
                        : m == Mode::erase ? "Delete automation points"
                                           : "Paint automation");
            break;
        case Mode::selectBox:
        {
            const auto b = juce::Rectangle<float> (startPosition, e.position);
            editor.selectArea (editor.xToTick (b.getX()), editor.xToTick (b.getRight()),
                               editor.yToValue (b.getBottom()), editor.yToValue (b.getY()), e.mods.isShiftDown());
            break;
        }
        case Mode::line:
            if (lineFrom.first != lineTo.first)
            {
                auto [a, b] = std::minmax (lineFrom, lineTo);
                editor.drawLine (a.first, a.second, b.first, b.second);
            }
            break;
        case Mode::none:
            break;
        }
        added = dragged = false;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override { return editor.keyPressed (key); }

private:
    enum class Mode
    {
        none,
        move,
        bend,
        erase,
        selectBox,
        line,
        paint
    };

    model::ParameterTarget targetOf() const
    {
        auto* automation = editor.findAutomation();
        return automation != nullptr ? automation->target : model::ParameterTarget {};
    }

    model::Ticks timeAt (float x, const juce::ModifierKeys& mods) const
    {
        auto t = editor.xToTick (x);
        if (! mods.isAltDown())
            t = editor.snapTicks (t);
        return juce::jlimit<model::Ticks> (0, editor.getClipLength(), t);
    }

    void showReadout (const model::AutomationPoint& p, juce::Point<float> at)
    {
        readout = editor.formatTime (p.time) + "  " + editor.session.describeTargetValue (targetOf(), p.value);
        readoutAt = at;
    }

    void beginEditing()
    {
        editor.editing = true;
        dragged = false;
    }

    void endEditing (const juce::String& what)
    {
        auto edited = editor.points;
        auto selected = editor.selection;
        editor.editing = false;
        if (edited != original || added)
            editor.commit (std::move (edited), std::move (selected), what);
        else
            editor.reload();
    }

    /** Moves the selected points, keeping each inside the clip and 0..1, then sorts them by time. */
    void applyMove (std::vector<model::AutomationPoint>& pts, std::set<int>& selected, model::Ticks deltaTicks,
                    float deltaValue)
    {
        const auto length = editor.getClipLength();
        for (auto i : selected)
        {
            auto& p = pts[(size_t)i];
            p.time = juce::jlimit<model::Ticks> (0, length, p.time + deltaTicks);
            p.value = juce::jlimit (0.0f, 1.0f, p.value + deltaValue);
        }
        movedAnchorTime = pts[(size_t)anchor].time;
        movedAnchorValue = pts[(size_t)anchor].value;
        editor.sortPoints (pts, selected);
    }

    int findMovedAnchor() const
    {
        for (auto i : editor.selection)
            if (editor.points[(size_t)i].time == movedAnchorTime &&
                juce::approximatelyEqual (editor.points[(size_t)i].value, movedAnchorValue))
                return i;
        return -1;
    }

    void eraseAt (juce::Point<float> p)
    {
        // Indices are the original ones: rebuild from the original without the erased points.
        if (const auto i = pointAtIn (original, p); i >= 0)
            erased.insert (i);
        if (erased.size() >= original.size())
            erased.erase (std::prev (erased.end())); // one point stays
        editor.points.clear();
        for (size_t i = 0; i < original.size(); ++i)
            if (! erased.contains ((int)i))
                editor.points.push_back (original[i]);
        editor.selection.clear();
        dragged = true;
    }

    int pointAtIn (const std::vector<model::AutomationPoint>& pts, juce::Point<float> p) const
    {
        for (size_t i = 0; i < pts.size(); ++i)
            if (positionOf (pts[i]).getDistanceFrom (p) <= 6.0f)
                return (int)i;
        return -1;
    }

    /** Paint: one point per grid step under the mouse (every 1/16 beat without a grid); the points
        between the first and last step painted are replaced. */
    void paintAt (juce::Point<float> p, const juce::ModifierKeys& mods)
    {
        const auto step = editor.snap > 0 && ! mods.isAltDown() ? editor.snap : beat / 16;
        const auto t = juce::jlimit<model::Ticks> (
            0, editor.getClipLength(), (model::Ticks)std::llround ((double)editor.xToTick (p.x) / (double)step) * step);
        painted[t] = editor.yToValue (p.y);

        const auto from = painted.begin()->first, to = painted.rbegin()->first;
        editor.points.clear();
        for (auto& point : original)
            if (point.time < from || point.time > to)
                editor.points.push_back (point);
        for (auto& [time, value] : painted)
            editor.points.push_back ({ time, value, 0.0f });
        std::set<int> none;
        editor.sortPoints (editor.points, none);
        editor.selection.clear();
        dragged = true;
        readout = editor.formatTime (t) + "  " + editor.session.describeTargetValue (targetOf(), painted[t]);
        readoutAt = p;
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addItem ("Select all (Ctrl+A)", [this] { editor.selectAll(); });
        menu.addItem ("Delete the selection (Delete)", ! editor.selection.empty(), false,
                      [this] { editor.deleteSelection(); });
        menu.addSeparator();
        menu.addItem ("Shapes...", [this] { editor.showShapesMenu(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    AutomationEditor& editor;
    Mode mode = Mode::none;
    juce::Point<float> startPosition;
    std::vector<model::AutomationPoint> original;
    std::set<int> originalSelection, erased;
    std::map<model::Ticks, float> painted;
    int anchor = -1, bendIndex = -1, hoveredPoint = -1, hoveredHandle = -1;
    model::Ticks movedAnchorTime = 0;
    float movedAnchorValue = 0.0f;
    bool added = false, dragged = false;
    juce::Rectangle<float> box;
    std::pair<model::Ticks, float> lineFrom, lineTo;
    juce::String readout;
    juce::Point<float> readoutAt;
};

//==============================================================================
/** Bars and beats above the canvas (the song's, so they match the Playlist). */
class AutomationEditor::Ruler final : public juce::Component
{
public:
    explicit Ruler (AutomationEditor& e) : editor (e) {}

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panelHeader"));
        const auto* clip = editor.findClip();
        if (clip == nullptr)
            return;

        const auto bar = editor.getTimeSignature().barTicks();
        const auto barWidth = editor.tickToX ((double)bar) - editor.tickToX (0.0);
        const auto beatTicks = editor.getTimeSignature().beatTicks();
        const bool showBeats = barWidth >= 120.0f;
        const auto step = showBeats ? beatTicks : bar * juce::jmax<model::Ticks> (1, (model::Ticks)(50.0f / barWidth));

        g.setFont (juce::FontOptions (11.0f));
        // Song time: the first line at or after the scroll position.
        const auto first = juce::jmax<model::Ticks> (0, clip->start + editor.xToTick (0.0f));
        for (auto song = (first + step - 1) / step * step;
             editor.tickToX ((double)(song - clip->start)) < (float)getWidth(); song += step)
        {
            const auto x = editor.tickToX ((double)(song - clip->start));
            const bool isBar = song % bar == 0;
            g.setColour (theme.get (isBar ? "text" : "textDim"));
            g.drawVerticalLine ((int)x, isBar ? 0.0f : (float)getHeight() * 0.5f, (float)getHeight());
            const auto label = juce::String (song / bar + 1) +
                               (isBar ? juce::String() : "." + juce::String (song % bar / beatTicks + 1));
            g.drawText (label, juce::Rectangle<float> (x + 3.0f, 0.0f, 44.0f, (float)getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }

private:
    AutomationEditor& editor;
};

/** The values at the left, in the control's own units. */
class AutomationEditor::Axis final : public juce::Component
{
public:
    explicit Axis (AutomationEditor& e) : editor (e) {}

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panelHeader"));
        const auto* automation = editor.findAutomation();
        if (automation == nullptr)
            return;

        g.setFont (juce::FontOptions (11.0f));
        const auto canvasTop = (float)(editor.canvas->getY() - getY());
        for (int i = 0; i <= 4; ++i)
        {
            const auto value = (float)i / 4.0f;
            const auto y = canvasTop + editor.valueToY (value);
            g.setColour (theme.get ("textDim"));
            g.drawHorizontalLine ((int)y, (float)getWidth() - 6.0f, (float)getWidth());
            g.setColour (theme.get ("text"));
            g.drawText (editor.session.describeTargetValue (automation->target, value),
                        juce::Rectangle<float> (2.0f, y - 7.0f, (float)getWidth() - 10.0f, 14.0f),
                        juce::Justification::centredRight, true);
        }
    }

private:
    AutomationEditor& editor;
};

//==============================================================================
AutomationEditor::AutomationEditor (Session& s) : Panel ("automation", "Automation"), session (s)
{
    setHelpText ("Automation editor: click to add points, drag to move (Shift: fine), right-click to delete, drag a "
                 "diamond to bend. Double-click a point to type its value. Wheel: scroll, Ctrl+wheel: zoom");

    canvas = std::make_unique<Canvas> (*this);
    ruler = std::make_unique<Ruler> (*this);
    axis = std::make_unique<Axis> (*this);
    for (auto* c : { (juce::Component*)canvas.get(), (juce::Component*)ruler.get(), (juce::Component*)axis.get(),
                     (juce::Component*)&horizontal, (juce::Component*)&title, (juce::Component*)&status })
        addAndMakeVisible (c);

    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    status.setFont (juce::FontOptions (12.0f));
    status.setJustificationType (juce::Justification::centredRight);

    const char* toolNames[] = { "Draw", "Line", "Paint", "Select" };
    const char* toolHelp[] = {
        "Draw: click to add a point, drag points to move them",
        "Line: drag from one place to another for a straight ramp; the points in between are replaced",
        "Paint: drag to draw the curve freehand, one point per grid step",
        "Select: drag a box around points to select them; then drag, nudge with the arrows or Delete them",
    };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = toolButtons[i];
        b.setButtonText (toolNames[i]);
        b.setHelpText (toolHelp[i]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x5171);
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
    snapChoice.setSelectedId (4, juce::dontSendNotification);
    snapChoice.setTooltip ("The grid that points snap to (hold Alt to place freely)");
    snapChoice.onChange = [this]
    {
        const auto i = snapChoice.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (i, (int)std::size (snapChoices)))
            setSnap (snapChoices[i].second < 0 ? getTimeSignature().barTicks() : snapChoices[i].second);
    };
    addAndMakeVisible (snapChoice);

    shapesButton.setHelpText ("Shapes: ramps, waves and steps over the selected points' span, or the whole clip");
    shapesButton.onClick = [this] { showShapesMenu(); };
    addAndMakeVisible (shapesButton);

    emptyMessage.setText ("Double-click an automation clip on the Playlist to edit it here.",
                          juce::dontSendNotification);
    emptyMessage.setJustificationType (juce::Justification::centred);
    emptyMessage.setInterceptsMouseClicks (false, false);
    addChildComponent (emptyMessage);

    horizontal.setAutoHide (false);
    horizontal.addListener (this);

    session.addChangeListener (this);
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    reload();
    startTimerHz (30);
}

AutomationEditor::~AutomationEditor()
{
    cancelPendingUpdate();
    session.getHistory().removeListener (historyListener);
    session.removeChangeListener (this);
}

//==============================================================================
const model::Clip* AutomationEditor::findClip() const
{
    const auto* clip = session.getProject().findClip (clipId);
    return clip != nullptr && std::holds_alternative<model::AutomationClip> (clip->content) ? clip : nullptr;
}

const model::AutomationClip* AutomationEditor::findAutomation() const
{
    const auto* clip = findClip();
    return clip != nullptr ? &std::get<model::AutomationClip> (clip->content) : nullptr;
}

model::Ticks AutomationEditor::getClipLength() const
{
    const auto* clip = findClip();
    return clip != nullptr ? clip->length : 0;
}

model::TimeSignature AutomationEditor::getTimeSignature() const { return session.getProject().settings.timeSignature; }

void AutomationEditor::setClip (model::ClipId id)
{
    clipId = id;
    selection.clear();
    editing = false;
    reload();
    fitToClip();
}

void AutomationEditor::reload()
{
    if (editing)
        return; // the mouse gesture's copy is shown; it is written when the mouse is released

    const auto* automation = findAutomation();
    auto fresh = automation != nullptr ? automation->points : std::vector<model::AutomationPoint>();
    if (fresh.size() != points.size())
        selection.clear();
    points = std::move (fresh);

    title.setText (automation != nullptr ? session.describeTarget (automation->target) : juce::String(),
                   juce::dontSendNotification);
    emptyMessage.setVisible (automation == nullptr);
    updateStatus();
    updateScrollBars();
    repaint();
}

void AutomationEditor::fitToClip()
{
    const auto length = getClipLength();
    if (length <= 0 || canvas->getWidth() <= 0)
        return;
    pixelsPerBeat = juce::jlimit (2.0, 800.0, (double)canvas->getWidth() * 0.94 / ((double)length / (double)beat));
    scrollTicks = -(double)length * 0.03;
    updateScrollBars();
    repaint();
}

void AutomationEditor::updateStatus()
{
    const auto* automation = findAutomation();
    if (automation == nullptr)
    {
        status.setText ({}, juce::dontSendNotification);
        return;
    }
    juce::String text;
    if (selection.size() == 1)
    {
        const auto& p = points[(size_t)*selection.begin()];
        text = "Point at " + formatTime (p.time) + ": " + session.describeTargetValue (automation->target, p.value);
    }
    else if (! selection.empty())
        text = juce::String ((int)selection.size()) + " points selected";
    else
        text = juce::String ((int)points.size()) + (points.size() == 1 ? " point" : " points");
    status.setText (text, juce::dontSendNotification);
}

//==============================================================================
float AutomationEditor::tickToX (double ticks) const
{
    return (float)((ticks - scrollTicks) / (double)beat * pixelsPerBeat);
}

model::Ticks AutomationEditor::xToTick (float x) const
{
    return (model::Ticks)std::llround ((double)x / pixelsPerBeat * (double)beat + scrollTicks);
}

float AutomationEditor::valueToY (float value) const
{
    const auto inset = 10.0f, h = (float)canvas->getHeight() - 2.0f * inset;
    return inset + (1.0f - value) * juce::jmax (1.0f, h);
}

float AutomationEditor::yToValue (float y) const
{
    const auto inset = 10.0f, h = (float)canvas->getHeight() - 2.0f * inset;
    return juce::jlimit (0.0f, 1.0f, 1.0f - (y - inset) / juce::jmax (1.0f, h));
}

void AutomationEditor::setSnap (model::Ticks s)
{
    snap = juce::jmax<model::Ticks> (0, s);
    canvas->repaint();
}

model::Ticks AutomationEditor::snapTicks (model::Ticks t) const
{
    if (snap <= 0)
        return t;
    // On the song's grid: the clip may start off the grid.
    const auto offset = findClip() != nullptr ? findClip()->start : 0;
    return (model::Ticks)std::llround ((double)(t + offset) / (double)snap) * snap - offset;
}

juce::String AutomationEditor::formatTime (model::Ticks t) const
{
    const auto song = t + (findClip() != nullptr ? findClip()->start : 0);
    const auto sig = getTimeSignature();
    const auto bar = sig.barTicks(), beatTicks = sig.beatTicks();
    return juce::String (song / bar + 1) + "." + juce::String (song % bar / beatTicks + 1) + "." +
           juce::String (song % beatTicks).paddedLeft ('0', 3);
}

std::optional<model::Ticks> AutomationEditor::parseTime (const juce::String& text) const
{
    auto parts = juce::StringArray::fromTokens (text.trim(), ".:", {});
    parts.removeEmptyStrings();
    if (parts.isEmpty() || parts.size() > 3)
        return std::nullopt;
    for (auto& p : parts)
        if (! p.containsOnly ("0123456789"))
            return std::nullopt;

    const auto sig = getTimeSignature();
    const auto bars = parts[0].getIntValue() - 1;
    const auto beats = parts.size() > 1 ? parts[1].getIntValue() - 1 : 0;
    const auto ticks = parts.size() > 2 ? parts[2].getIntValue() : 0;
    if (bars < 0 || beats < 0)
        return std::nullopt;
    const auto song = (model::Ticks)bars * sig.barTicks() + (model::Ticks)beats * sig.beatTicks() + ticks;
    return song - (findClip() != nullptr ? findClip()->start : 0);
}

//==============================================================================
void AutomationEditor::sortPoints (std::vector<model::AutomationPoint>& pts, std::set<int>& selected) const
{
    std::vector<std::pair<model::AutomationPoint, bool>> tagged;
    tagged.reserve (pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        tagged.emplace_back (pts[i], selected.contains ((int)i));
    std::stable_sort (tagged.begin(), tagged.end(), [] (auto& a, auto& b) { return a.first.time < b.first.time; });
    selected.clear();
    for (size_t i = 0; i < tagged.size(); ++i)
    {
        pts[i] = tagged[i].first;
        if (tagged[i].second)
            selected.insert ((int)i);
    }
}

bool AutomationEditor::commit (std::vector<model::AutomationPoint> newPoints, std::set<int> newSelection,
                               const juce::String&)
{
    if (findClip() == nullptr || newPoints.empty())
        return false;

    sortPoints (newPoints, newSelection);
    juce::Array<juce::var> list;
    for (auto& p : newPoints)
        list.add (model::pointToJson (p));
    auto* params = new juce::DynamicObject();
    params->setProperty ("clip", (int)clipId.value);
    params->setProperty ("points", list);

    juce::String error;
    auto command = commands::CommandRegistry::get().create ("clip.setPoints", juce::var (params), error);
    if (command == nullptr || session.getHistory().perform (std::move (command)).failed())
    {
        reload();
        return false;
    }
    handleUpdateNowIfNeeded(); // reload now, then keep the selection on the edited points
    points = std::move (newPoints);
    selection = std::move (newSelection);
    updateStatus();
    repaint();
    return true;
}

bool AutomationEditor::addPoint (model::Ticks time, float value)
{
    auto pts = points;
    pts.push_back ({ juce::jlimit<model::Ticks> (0, getClipLength(), time), juce::jlimit (0.0f, 1.0f, value), 0.0f });
    return commit (std::move (pts), { (int)points.size() }, "Add automation point");
}

bool AutomationEditor::movePoints (const std::set<int>& indices, model::Ticks deltaTicks, float deltaValue)
{
    auto pts = points;
    const auto length = getClipLength();
    for (auto i : indices)
        if (juce::isPositiveAndBelow (i, (int)pts.size()))
        {
            auto& p = pts[(size_t)i];
            p.time = juce::jlimit<model::Ticks> (0, length, p.time + deltaTicks);
            p.value = juce::jlimit (0.0f, 1.0f, p.value + deltaValue);
        }
    return pts != points && commit (std::move (pts), indices, "Move automation points");
}

bool AutomationEditor::setPoint (int index, model::Ticks time, float value)
{
    if (! juce::isPositiveAndBelow (index, (int)points.size()))
        return false;
    auto pts = points;
    pts[(size_t)index].time = juce::jlimit<model::Ticks> (0, getClipLength(), time);
    pts[(size_t)index].value = juce::jlimit (0.0f, 1.0f, value);
    return commit (std::move (pts), { index }, "Set automation point");
}

bool AutomationEditor::setCurve (int index, float curve)
{
    if (! juce::isPositiveAndBelow (index, (int)points.size()))
        return false;
    auto pts = points;
    pts[(size_t)index].curve = juce::jlimit (-1.0f, 1.0f, curve);
    return pts != points && commit (std::move (pts), selection, "Bend automation curve");
}

bool AutomationEditor::deletePoints (const std::set<int>& indices)
{
    std::vector<model::AutomationPoint> kept;
    for (size_t i = 0; i < points.size(); ++i)
        if (! indices.contains ((int)i))
            kept.push_back (points[i]);
    if (kept.empty() || kept.size() == points.size())
        return false;
    return commit (std::move (kept), {}, "Delete automation points");
}

bool AutomationEditor::drawLine (model::Ticks from, float fromValue, model::Ticks to, float toValue)
{
    const auto length = getClipLength();
    from = juce::jlimit<model::Ticks> (0, length, from);
    to = juce::jlimit<model::Ticks> (0, length, to);
    if (to <= from)
        return false;
    std::vector<model::AutomationPoint> pts;
    for (auto& p : points)
        if (p.time < from || p.time > to)
            pts.push_back (p);
    pts.push_back ({ from, juce::jlimit (0.0f, 1.0f, fromValue), 0.0f });
    pts.push_back ({ to, juce::jlimit (0.0f, 1.0f, toValue), 0.0f });
    return commit (std::move (pts), {}, "Draw automation line");
}

bool AutomationEditor::applyShape (model::AutomationShape shape, model::Ticks period)
{
    const auto length = getClipLength();
    if (length <= 0)
        return false;

    model::Ticks from = 0, to = length;
    float low = 0.0f, high = 1.0f;
    if (selection.size() >= 2)
    {
        from = points[(size_t)*selection.begin()].time;
        to = points[(size_t)*selection.rbegin()].time;
        auto [lo, hi] = std::minmax_element (selection.begin(), selection.end(), [this] (int a, int b)
                                             { return points[(size_t)a].value < points[(size_t)b].value; });
        if (points[(size_t)*hi].value - points[(size_t)*lo].value > 0.001f)
        {
            low = points[(size_t)*lo].value;
            high = points[(size_t)*hi].value;
        }
        if (to <= from)
        {
            from = 0;
            to = length;
        }
    }

    std::vector<model::AutomationPoint> pts;
    for (auto& p : points)
        if (p.time < from || p.time > to)
            pts.push_back (p);
    for (auto& p : model::makeAutomationShape (shape, from, to, period, low, high))
        pts.push_back (p);
    return commit (std::move (pts), {}, "Automation shape");
}

//==============================================================================
void AutomationEditor::setSelection (std::set<int> s)
{
    std::erase_if (s, [this] (int i) { return ! juce::isPositiveAndBelow (i, (int)points.size()); });
    selection = std::move (s);
    updateStatus();
    canvas->repaint();
}

void AutomationEditor::selectAll()
{
    std::set<int> all;
    for (int i = 0; i < (int)points.size(); ++i)
        all.insert (i);
    setSelection (std::move (all));
}

void AutomationEditor::selectArea (model::Ticks from, model::Ticks to, float low, float high, bool add)
{
    auto s = add ? selection : std::set<int>();
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].time >= from && points[i].time <= to && points[i].value >= low && points[i].value <= high)
            s.insert ((int)i);
    setSelection (std::move (s));
}

void AutomationEditor::setTool (Tool t)
{
    tool = t;
    toolButtons[(int)t].setToggleState (true, juce::dontSendNotification);
}

void AutomationEditor::nudge (float deltaValue, model::Ticks deltaTicks)
{
    if (! selection.empty())
        movePoints (selection, deltaTicks, deltaValue);
}

bool AutomationEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return deleteSelection() || ! selection.empty();
    if (key == juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0))
    {
        selectAll();
        return true;
    }
    if (key == juce::KeyPress::escapeKey && ! selection.empty())
    {
        clearSelection();
        return true;
    }
    if (selection.empty())
        return false;

    const auto valueStep = mods.isShiftDown() ? 0.001f : 0.01f;
    const auto timeStep = mods.isAltDown() || snap <= 0 ? (model::Ticks)1 : snap;
    if (key.getKeyCode() == juce::KeyPress::upKey)
        nudge (valueStep, 0);
    else if (key.getKeyCode() == juce::KeyPress::downKey)
        nudge (-valueStep, 0);
    else if (key.getKeyCode() == juce::KeyPress::leftKey)
        nudge (0.0f, -timeStep);
    else if (key.getKeyCode() == juce::KeyPress::rightKey)
        nudge (0.0f, timeStep);
    else
        return false;
    return true;
}

void AutomationEditor::showShapesMenu()
{
    if (findClip() == nullptr)
        return;
    const auto bar = getTimeSignature().barTicks();
    const std::pair<const char*, model::Ticks> rates[] = {
        { "Every 2 bars", bar * 2 }, { "Every bar", bar },           { "Every half bar", bar / 2 },
        { "Every beat", beat },      { "Every 1/2 beat", beat / 2 }, { "Every 1/4 beat", beat / 4 },
    };
    using Shape = model::AutomationShape;
    auto item = [this] (Shape shape, model::Ticks period)
    { return [this, shape, period] { applyShape (shape, period); }; };

    juce::PopupMenu menu;
    menu.addSectionHeader (selection.size() >= 2 ? "Over the selected points" : "Over the whole clip");
    menu.addItem ("Ramp up", item (Shape::rampUp, 0));
    menu.addItem ("Ramp down", item (Shape::rampDown, 0));
    const std::pair<const char*, Shape> waves[] = { { "Sine wave", Shape::sine },
                                                    { "Triangle wave", Shape::triangle },
                                                    { "Square wave", Shape::square },
                                                    { "Saw wave", Shape::saw } };
    for (auto [name, shape] : waves)
    {
        juce::PopupMenu sub;
        for (auto [rateName, period] : rates)
            sub.addItem (rateName, item (shape, period));
        menu.addSubMenu (name, sub);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&shapesButton));
}

void AutomationEditor::editPoint (int index)
{
    const auto* automation = findAutomation();
    if (automation == nullptr || ! juce::isPositiveAndBelow (index, (int)points.size()))
        return;

    const auto& p = points[(size_t)index];
    auto window =
        std::make_shared<juce::AlertWindow> ("Automation point",
                                             "Type the value (in the control's units, or as a percentage like "
                                             "\"50%\") and the position (bar.beat.tick).",
                                             juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("value", session.describeTargetValue (automation->target, p.value), "Value:");
    window->addTextEditor ("time", formatTime (p.time), "Position:");
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (
        true,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<AutomationEditor> (this), window, index,
             target = automation->target] (int result)
            {
                if (safe == nullptr || result != 1 || ! juce::isPositiveAndBelow (index, (int)safe->points.size()))
                    return;
                const auto& point = safe->points[(size_t)index];
                const auto value = safe->session.parseTargetValue (target, window->getTextEditorContents ("value"));
                const auto time = safe->parseTime (window->getTextEditorContents ("time"));
                safe->setPoint (index, time.value_or (point.time), value.value_or (point.value));
            }));
}

//==============================================================================
void AutomationEditor::paint (juce::Graphics& g)
{
    g.fillAll (themeOf (*this).get ("panelHeader"));
    for (auto* c : { (juce::Component*)canvas.get(), (juce::Component*)ruler.get(), (juce::Component*)axis.get() })
        c->repaint();
}

void AutomationEditor::resized()
{
    const bool firstLayout = canvas->getWidth() <= 0;
    auto area = getLocalBounds();
    auto bar = area.removeFromTop (toolbarHeight).reduced (4, 3);
    for (auto& b : toolButtons)
        b.setBounds (bar.removeFromLeft (58));
    bar.removeFromLeft (8);
    snapChoice.setBounds (bar.removeFromLeft (140));
    bar.removeFromLeft (6);
    shapesButton.setBounds (bar.removeFromLeft (70));
    bar.removeFromLeft (10);
    title.setBounds (bar.removeFromLeft (juce::jmin (260, bar.getWidth() / 2)));
    status.setBounds (bar);

    horizontal.setBounds (area.removeFromBottom (12).withTrimmedLeft (axisWidth));
    ruler->setBounds (area.removeFromTop (rulerHeight).withTrimmedLeft (axisWidth));
    axis->setBounds (area.removeFromLeft (axisWidth));
    canvas->setBounds (area);
    emptyMessage.setBounds (area);
    if (firstLayout)
        fitToClip();
    updateScrollBars();
}

void AutomationEditor::updateScrollBars()
{
    const auto length = (double)juce::jmax<model::Ticks> (beat * 4, getClipLength());
    const auto visible = (double)canvas->getWidth() / pixelsPerBeat * (double)beat;
    horizontal.setRangeLimits (juce::jmin (-length * 0.05, scrollTicks),
                               juce::jmax (length * 1.1, scrollTicks + visible));
    horizontal.setCurrentRange (scrollTicks, visible, juce::dontSendNotification);
}

void AutomationEditor::scrollBarMoved (juce::ScrollBar*, double start)
{
    scrollTicks = start;
    repaint();
}

void AutomationEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
    {
        // Zoom around the mouse.
        const auto at = canvas->getLocalPoint (this, e.position).x;
        const auto tick = (double)at / pixelsPerBeat * (double)beat + scrollTicks;
        pixelsPerBeat = juce::jlimit (2.0, 800.0, pixelsPerBeat * (wheel.deltaY > 0 ? 1.15 : 1.0 / 1.15));
        scrollTicks = tick - (double)at / pixelsPerBeat * (double)beat;
    }
    else
        scrollTicks -= (double)(std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY) *
                       (double)canvas->getWidth() / pixelsPerBeat * (double)beat * 0.5;

    updateScrollBars();
    repaint();
}

void AutomationEditor::timerCallback()
{
    // The song position inside the clip, in song mode.
    double ticks = -1.0;
    if (const auto* clip = findClip(); clip != nullptr && session.getMode() == Session::Mode::song)
    {
        auto& edit = session.getEdit();
        const auto song = engine::timeToTicks (edit, edit.getTransport().getPosition());
        if (song >= (double)clip->start && song <= (double)(clip->start + clip->length))
            ticks = song - (double)clip->start;
    }
    if (std::abs (ticks - playheadTicks) > 1.0)
    {
        playheadTicks = ticks;
        canvas->repaint();
    }
}

} // namespace spinus::ui
