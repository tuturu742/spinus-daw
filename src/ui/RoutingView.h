#pragma once

#include "Workspace.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The Routing panel: the whole signal flow on one page. Channels on the left, mixer
    inserts in the middle (each column one step further from the Master), the Master on the right; lines
    for where each one plays into, dashed lines for sends (with their level) and orange dotted lines for
    sidechains. Pointing at a box shows only its connections; clicking an insert selects it in the Mixer.
    Read-only: routing is changed in the Mixer. */
class RoutingView final : public Panel, private juce::AsyncUpdater
{
public:
    explicit RoutingView (Session&);
    ~RoutingView() override;

    struct Node
    {
        enum class Kind
        {
            channel,
            insert,
            master
        };
        Kind kind;
        juce::uint32 id; // ChannelId or InsertId value
        juce::String name;
        juce::Colour colour;
        int column = 0;
        juce::Rectangle<float> box;
    };

    struct Edge
    {
        enum class Kind
        {
            output, // a channel into its insert, an insert into its output
            send,
            sidechain
        };
        Kind kind;
        size_t from, to; // indices into getNodes()
        juce::String label;
    };

    const std::vector<Node>& getNodes() const noexcept { return nodes; }
    const std::vector<Edge>& getEdges() const noexcept { return edges; }
    /** The node under a point of the drawing, if any. */
    std::optional<size_t> nodeAt (juce::Point<float>) const;
    /** The line drawn for an edge: a curve, or round the boxes it would pass behind. */
    juce::Path getEdgePath (const Edge&) const;

    /** Called when an insert (or the Master) is clicked. */
    std::function<void (model::InsertId)> onInsertClicked;

    void updateNow() { handleUpdateNowIfNeeded(); }

    juce::Rectangle<int> getDefaultBounds() const override { return { 150, 70, 860, 480 }; }
    void resized() override;

private:
    class Canvas;

    void handleAsyncUpdate() override;
    void rebuild();
    void layoutNodes();
    bool crossesOtherBox (const juce::Path&, const Edge&) const;
    static juce::Path routeAround (juce::Point<float> start, juce::Point<float> end);

    Session& session;
    int historyListener = 0;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::unique_ptr<Canvas> canvas;
    juce::Viewport viewport;
};

} // namespace spinus::ui
