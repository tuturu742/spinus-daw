#include "RoutingView.h"

#include "Theme.h"

#include <cmath>
#include <map>

namespace spinus::ui
{

namespace
{
constexpr float boxWidth = 130.0f, boxHeight = 26.0f, rowGap = 14.0f, columnGap = 70.0f, margin = 16.0f;
const juce::Colour sidechainColour { 0xffe0894a };

const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}
} // namespace

//==============================================================================
class RoutingView::Canvas final : public juce::Component
{
public:
    explicit Canvas (RoutingView& v) : view (v) { setRepaintsOnMouseActivity (false); }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        g.fillAll (theme.get ("panel"));

        const auto& nodes = view.nodes;
        auto involved = [this] (const Edge& e) { return ! hovered || e.from == *hovered || e.to == *hovered; };

        // Lines first, the boxes over them.
        for (const auto& e : view.edges)
        {
            const auto path = view.getEdgePath (e);
            const bool forward = nodes[e.to].column > nodes[e.from].column;
            const auto end = forward ? juce::Point<float> { nodes[e.to].box.getX(), nodes[e.to].box.getCentreY() }
                                     : juce::Point<float> { nodes[e.to].box.getRight(), nodes[e.to].box.getCentreY() };

            const auto alpha = involved (e) ? 1.0f : 0.15f;
            auto colour = e.kind == Edge::Kind::sidechain ? sidechainColour
                          : e.kind == Edge::Kind::send    ? theme.get ("accent")
                                                          : theme.get ("textDim");
            g.setColour (colour.withAlpha (alpha));
            juce::PathStrokeType stroke (e.kind == Edge::Kind::output ? 1.5f : 1.8f);
            if (e.kind == Edge::Kind::output)
            {
                g.strokePath (path, stroke);
            }
            else
            {
                juce::Path dashed;
                const float dashes[] = { e.kind == Edge::Kind::send ? 6.0f : 2.0f, 4.0f };
                stroke.createDashedStroke (dashed, path, dashes, 2);
                g.fillPath (dashed);
            }
            // The arrow head: pointing right into a box's left side, or left into its right side.
            juce::Path head;
            const auto dir = forward ? 1.0f : -1.0f;
            head.addTriangle (end, end.translated (-7.0f * dir, -4.0f), end.translated (-7.0f * dir, 4.0f));
            g.fillPath (head);

            if (e.label.isNotEmpty() && involved (e))
            {
                const auto mid = path.getPointAlongPath (path.getLength() * 0.5f);
                g.setFont (juce::FontOptions (10.5f));
                const auto r = juce::Rectangle<float> (mid.x - 40.0f, mid.y - 15.0f, 80.0f, 13.0f);
                g.setColour (theme.get ("panel").withAlpha (0.8f));
                g.fillRoundedRectangle (
                    r.withSizeKeepingCentre (
                        juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), e.label) + 6.0f, 13.0f),
                    3.0f);
                g.setColour (colour);
                g.drawText (e.label, r, juce::Justification::centred);
            }
        }

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& n = nodes[i];
            const bool lit = ! hovered || *hovered == i || connected (i, *hovered);
            g.setColour (theme.get ("panelHeader").withAlpha (lit ? 1.0f : 0.4f));
            g.fillRoundedRectangle (n.box, 4.0f);
            g.setColour (n.colour.withAlpha (lit ? 1.0f : 0.4f));
            g.fillRoundedRectangle (n.box.withWidth (5.0f), 2.0f);
            if (hovered && *hovered == i)
            {
                g.setColour (theme.get ("accent"));
                g.drawRoundedRectangle (n.box, 4.0f, 1.0f);
            }
            g.setColour (theme.get (lit ? "text" : "textDim"));
            g.setFont (juce::FontOptions (12.0f, n.kind == Node::Kind::master ? juce::Font::bold : juce::Font::plain));
            g.drawText (n.name, n.box.withTrimmedLeft (10.0f).withTrimmedRight (4.0f), juce::Justification::centredLeft,
                        true);
        }

        if (nodes.size() <= 1)
        {
            g.setColour (theme.get ("textDim"));
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Add a channel to see where its sound goes.", getLocalBounds().reduced (20),
                        juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto now = view.nodeAt (e.position);
        if (now != hovered)
        {
            hovered = now;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered.reset();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (auto i = view.nodeAt (e.position))
        {
            const auto& n = view.nodes[*i];
            if (n.kind != Node::Kind::channel && view.onInsertClicked)
                view.onInsertClicked (model::InsertId { n.id });
        }
    }

private:
    bool connected (size_t a, size_t b) const
    {
        for (const auto& e : view.edges)
            if ((e.from == a && e.to == b) || (e.from == b && e.to == a))
                return true;
        return false;
    }

    RoutingView& view;
    std::optional<size_t> hovered;
};

//==============================================================================
RoutingView::RoutingView (Session& s)
    : Panel ("routing", "Routing"), session (s), canvas (std::make_unique<Canvas> (*this))
{
    viewport.setViewedComponent (canvas.get(), false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    setHelpText ("Routing: where every channel and insert plays into. Point at a box to see its connections; "
                 "click an insert to select it in the Mixer. Change routing in the Mixer.");
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    rebuild();
}

RoutingView::~RoutingView() { session.getHistory().removeListener (historyListener); }

void RoutingView::handleAsyncUpdate()
{
    rebuild();
    canvas->repaint();
}

void RoutingView::resized()
{
    viewport.setBounds (getLocalBounds());
    layoutNodes();
}

std::optional<size_t> RoutingView::nodeAt (juce::Point<float> p) const
{
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].box.contains (p))
            return i;
    return std::nullopt;
}

bool RoutingView::crossesOtherBox (const juce::Path& path, const Edge& e) const
{
    const auto length = path.getLength();
    for (int step = 0; (float)step * 3.0f < length; ++step)
    {
        const auto p = path.getPointAlongPath ((float)step * 3.0f);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (i != e.from && i != e.to && nodes[i].box.contains (p))
                return true;
    }
    return false;
}

/** Out of the start's column, along the free gap between rows nearest the line, into the end's column. */
juce::Path RoutingView::routeAround (juce::Point<float> start, juce::Point<float> end)
{
    const auto rowStep = boxHeight + rowGap;
    const auto middle = (start.y + end.y) * 0.5f;
    const auto gapY = margin - rowGap * 0.5f + std::round ((middle - margin + rowGap * 0.5f) / rowStep) * rowStep;
    const auto x1 = start.x + columnGap * 0.5f, x2 = end.x - columnGap * 0.5f;
    juce::Path path;
    path.startNewSubPath (start);
    path.cubicTo ({ start.x + columnGap * 0.3f, start.y }, { x1 - columnGap * 0.2f, gapY }, { x1, gapY });
    path.lineTo (x2, gapY);
    path.cubicTo ({ x2 + columnGap * 0.2f, gapY }, { end.x - columnGap * 0.3f, end.y }, end);
    return path;
}

juce::Path RoutingView::getEdgePath (const Edge& e) const
{
    const auto& a = nodes[e.from].box;
    const auto& b = nodes[e.to].box;
    juce::Path path;
    const juce::Point<float> start { a.getRight(), a.getCentreY() };
    if (nodes[e.to].column > nodes[e.from].column)
    {
        const juce::Point<float> end { b.getX(), b.getCentreY() };
        const auto bend = (end.x - start.x) * 0.5f;
        path.startNewSubPath (start);
        path.cubicTo (start.translated (bend, 0.0f), end.translated (-bend, 0.0f), end);
        // A line that would pass behind another box goes round it, along the gap between two rows.
        if (crossesOtherBox (path, e))
            path = routeAround (start, end);
    }
    else
    {
        // To a box in the same column or further left: out to the right and back in from the right.
        const juce::Point<float> end { b.getRight(), b.getCentreY() };
        path.startNewSubPath (start);
        path.cubicTo (start.translated (50.0f, 0.0f), end.translated (50.0f, 0.0f), end);
    }
    return path;
}

void RoutingView::rebuild()
{
    const auto& project = session.getProject();
    const auto& mixer = project.mixer;
    nodes.clear();
    edges.clear();

    // How many steps each insert is from the Master (its column counts back from the Master's).
    std::map<juce::uint32, int> steps;
    std::function<int (const model::Insert&, int)> stepsOf = [&] (const model::Insert& insert, int guard) -> int
    {
        if (insert.id == model::masterInsertId || guard > 64)
            return 0;
        if (auto it = steps.find (insert.id.value); it != steps.end())
            return it->second;
        const auto* out = project.findInsert (insert.output);
        const auto n = 1 + (out != nullptr ? stepsOf (*out, guard + 1) : 0);
        steps[insert.id.value] = n;
        return n;
    };
    int deepest = 0;
    for (const auto& insert : mixer.inserts)
        deepest = juce::jmax (deepest, stepsOf (insert, 0));

    std::map<juce::uint32, size_t> insertNode;
    nodes.push_back ({ Node::Kind::master,
                       model::masterInsertId.value,
                       "Master",
                       juce::Colour (mixer.master.colour),
                       deepest + 1,
                       {} });
    insertNode[model::masterInsertId.value] = 0;
    for (size_t i = 0; i < mixer.inserts.size(); ++i)
    {
        const auto& insert = mixer.inserts[i];
        insertNode[insert.id.value] = nodes.size();
        nodes.push_back ({ Node::Kind::insert,
                           insert.id.value,
                           juce::String ((int)i + 1) + " " + insert.name,
                           juce::Colour (insert.colour),
                           deepest + 1 - steps[insert.id.value],
                           {} });
    }
    for (const auto& channel : project.channels)
    {
        const auto target = insertNode.find (channel.targetInsert.value);
        if (target == insertNode.end())
            continue;
        edges.push_back ({ Edge::Kind::output, nodes.size(), target->second, {} });
        nodes.push_back ({ Node::Kind::channel, channel.id.value, channel.name, juce::Colour (channel.colour), 0, {} });
    }

    auto addInsertEdges = [&] (const model::Insert& insert)
    {
        const auto from = insertNode.at (insert.id.value);
        if (insert.id != model::masterInsertId)
            if (auto out = insertNode.find (insert.output.value); out != insertNode.end())
                edges.push_back ({ Edge::Kind::output, from, out->second, {} });
        for (const auto& send : insert.sends)
            if (auto to = insertNode.find (send.target.value); to != insertNode.end())
                edges.push_back ({ Edge::Kind::send, from, to->second, juce::String (send.levelDb, 1) + " dB" });
        for (auto source : insert.sidechainSources)
            if (auto src = insertNode.find (source.value); src != insertNode.end())
                edges.push_back ({ Edge::Kind::sidechain, src->second, from, "sidechain" });
    };
    addInsertEdges (mixer.master);
    for (const auto& insert : mixer.inserts)
        addInsertEdges (insert);

    layoutNodes();
}

void RoutingView::layoutNodes()
{
    std::map<int, int> rowsInColumn;
    float width = 0.0f, height = 0.0f;
    // Channels first (left), then inserts in mixer order, then the Master on its own.
    std::vector<size_t> order;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind == Node::Kind::channel)
            order.push_back (i);
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].kind != Node::Kind::channel)
            order.push_back (i);

    for (auto i : order)
    {
        auto& n = nodes[i];
        const auto row = rowsInColumn[n.column]++;
        n.box = { margin + (float)n.column * (boxWidth + columnGap), margin + (float)row * (boxHeight + rowGap),
                  boxWidth, boxHeight };
        width = juce::jmax (width, n.box.getRight() + margin + 40.0f); // room for lines that loop back
        height = juce::jmax (height, n.box.getBottom() + margin);
    }
    canvas->setSize (juce::jmax (viewport.getMaximumVisibleWidth(), juce::roundToInt (width)),
                     juce::jmax (viewport.getMaximumVisibleHeight(), juce::roundToInt (height)));
}

} // namespace spinus::ui
