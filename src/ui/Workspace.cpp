#include "Workspace.h"

#include "Shortcuts.h"
#include "Theme.h"

#include <set>

namespace spinus::ui
{

namespace
{
constexpr int headerHeight = 24;

juce::Colour themed (const juce::Component& c, const char* role, juce::Colour fallback)
{
    if (auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel()))
        return lf->getTheme().get (role);
    return fallback;
}
} // namespace

//==============================================================================
/** A floating panel inside the workspace: title bar (drag to move, buttons to detach and close),
    resizable border, the panel below. */
class Workspace::Frame : public juce::Component, private juce::Timer
{
public:
    Frame (Workspace& w, Panel& p) : workspace (w), panel (p)
    {
        detachButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x86\x97")); // ↗
        detachButton.setHelpText ("Open " + panel.getPanelTitle() + " in its own window");
        detachButton.onClick = [this] { workspace.detachPanel (panel.getPanelId()); };

        closeButton.setButtonText (juce::CharPointer_UTF8 ("\xc3\x97")); // ×
        closeButton.setHelpText ("Close " + panel.getPanelTitle());
        closeButton.onClick = [this] { workspace.hidePanel (panel.getPanelId()); };

        for (auto* b : { &detachButton, &closeButton })
            addAndMakeVisible (b);

        constrainer.setMinimumSize (200, 120);
        // The title bar stays in the workspace: it never goes off the top, and off the other sides enough of
        // it stays to grab it again.
        constrainer.setMinimumOnscreenAmounts (0xffffff, 120, headerHeight, 120);
        // The title bar moves the panel; only the sides and bottom resize it (a top edge would resize
        // the panel when the title bar is grabbed near its edge).
        resizer.setBorderThickness ({ 0, 5, 5, 5 });
        // It paints all of itself: moving it does not repaint what is behind.
        setOpaque (true);
        resizer.setHelpText ("Drag to resize " + panel.getPanelTitle());
        addAndMakeVisible (resizer);

        setHelpText (panel.getPanelTitle() + ": drag the title bar to move");
        attachContent();
    }

    void attachContent()
    {
        addAndMakeVisible (panel, 0);
        resized();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (themed (*this, "panel", juce::Colours::darkgrey));
        auto header = getLocalBounds().removeFromTop (headerHeight);
        const bool front = workspace.getFrontPanelId() == panel.getPanelId();
        g.setColour (themed (*this, "panelHeader", juce::Colours::grey));
        g.fillRect (header);
        g.setColour (front ? themed (*this, "accent", juce::Colours::yellow)
                           : themed (*this, "border", juce::Colours::black));
        g.fillRect (header.removeFromBottom (front ? 2 : 1));
        g.setColour (themed (*this, "text", juce::Colours::white));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (panel.getPanelTitle(), header.withTrimmedLeft (8), juce::Justification::centredLeft);
        g.setColour (themed (*this, "border", juce::Colours::black));
        g.drawRect (getLocalBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        resizer.setBounds (area);
        auto header = area.removeFromTop (headerHeight);
        closeButton.setBounds (header.removeFromRight (headerHeight).reduced (3));
        detachButton.setBounds (header.removeFromRight (headerHeight).reduced (3));

        if (panel.getParentComponent() == this)
            panel.setBounds (area.reduced (1, 0).withTrimmedBottom (1));

        closeButton.toFront (false);
        detachButton.toFront (false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        toFront (true);
        workspace.repaint();
        if (e.getMouseDownY() < headerHeight)
        {
            // Dragging follows the pointer's real position, not the events' (X11 can hold back the last
            // motion event until the next one, so the panel stopped short of the pointer and jumped when it
            // moved again). A timer catches up with the pointer between events.
            grabOffset = e.getScreenPosition() - getScreenPosition();
            dragging = true;
            startTimerHz (60);
        }
    }

    void mouseDrag (const juce::MouseEvent&) override { followPointer(); }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging)
            followPointer();
        stopDragging();
    }

    void timerCallback() override
    {
        if (! juce::Desktop::getInstance().getMainMouseSource().isDragging())
            stopDragging();
        else
            followPointer();
    }

    void followPointer()
    {
        auto* parent = getParentComponent();
        if (! dragging || parent == nullptr)
            return;
        const auto pointer = juce::Desktop::getMousePosition();
        const auto target = parent->getLocalPoint (nullptr, pointer - grabOffset);
        if (target != getPosition())
            constrainer.setBoundsForComponent (this, getBounds().withPosition (target), false, false, false, false);
    }

    void stopDragging()
    {
        dragging = false;
        stopTimer();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < headerHeight)
            workspace.detachPanel (panel.getPanelId());
    }

    // Brings the frame forward when anything inside it is clicked.
    void broughtToFront() override { workspace.repaint(); }

private:
    Workspace& workspace;
    Panel& panel;
    juce::TextButton detachButton, closeButton;
    juce::Point<int> grabOffset;
    bool dragging = false;
    juce::ComponentBoundsConstrainer constrainer;
    juce::ResizableBorderComponent resizer { this, &constrainer };
};

//==============================================================================
/** A panel detached into its own desktop window; closing the window docks it back. */
class Workspace::DetachedWindow : public juce::DocumentWindow
{
public:
    DetachedWindow (Workspace& w, Panel& p, juce::Rectangle<int> size)
        : DocumentWindow (p.getPanelTitle(), themed (w, "background", juce::Colours::black),
                          DocumentWindow::closeButton),
          workspace (w), panel (p)
    {
        // A window of its own is not inside the main view: it needs the Spinus look (colours, knobs) itself.
        setLookAndFeel (&w.getLookAndFeel());
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setContentNonOwned (&panel, false);
        setContentComponentSize (juce::jmax (300, size.getWidth()), juce::jmax (200, size.getHeight()));
        setTopLeftPosition (w.getScreenPosition() + juce::Point<int> (60, 60));
        shortcuts::addTo (*this);
    }

    ~DetachedWindow() override
    {
        shortcuts::removeFrom (*this);
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override { workspace.attachPanel (panel.getPanelId()); }

private:
    Workspace& workspace;
    Panel& panel;
};

//==============================================================================
Workspace::Workspace() { setHelpText ("Workspace: F5 Playlist, F6 Channel Rack, F7 Piano Roll, F9 Mixer"); }

Workspace::~Workspace()
{
    // Windows and frames refer to the panels; delete them first.
    for (auto& [id, entry] : entries)
    {
        entry->window = nullptr;
        entry->frame = nullptr;
    }
}

Workspace::Entry* Workspace::find (const juce::String& id) const
{
    auto it = entries.find (id);
    return it != entries.end() ? it->second.get() : nullptr;
}

void Workspace::addPanel (std::unique_ptr<Panel> panel)
{
    jassert (panel != nullptr && find (panel->getPanelId()) == nullptr);
    const auto id = panel->getPanelId();

    auto entry = std::make_unique<Entry>();
    entry->panel = std::move (panel);
    entry->frame = std::make_unique<Frame> (*this, *entry->panel);
    auto bounds = entry->panel->getDefaultBounds();
    entry->frame->setBounds (bounds.withHeight (bounds.getHeight() + headerHeight));
    addChildComponent (*entry->frame);

    entries[id] = std::move (entry);
    order.add (id);
}

Panel* Workspace::getPanel (const juce::String& id) const
{
    auto* e = find (id);
    return e != nullptr ? e->panel.get() : nullptr;
}

juce::StringArray Workspace::getPanelIds() const { return order; }

void Workspace::showPanel (const juce::String& id)
{
    auto* e = find (id);
    if (e == nullptr || ! isPanelAvailable (id))
        return;

    if (e->window != nullptr)
    {
        e->window->toFront (true);
        return;
    }

    e->frame->setVisible (true);
    e->frame->toFront (true);
    repaint();
}

void Workspace::hidePanel (const juce::String& id)
{
    if (auto* e = find (id))
    {
        if (e->window != nullptr)
            attachPanel (id);
        e->frame->setVisible (false);
        repaint();
    }
}

void Workspace::togglePanel (const juce::String& id)
{
    // FL behaviour: the shortcut closes a panel that is already in front, otherwise brings it forward.
    if (isPanelVisible (id) && ! isPanelDetached (id) && getFrontPanelId() == id)
        hidePanel (id);
    else
        showPanel (id);
}

void Workspace::setPanelAvailable (const juce::String& id, bool available)
{
    if (available)
        unavailable.erase (id);
    else
    {
        hidePanel (id);
        unavailable.insert (id);
    }
}

bool Workspace::isPanelVisible (const juce::String& id) const
{
    auto* e = find (id);
    return e != nullptr && (e->window != nullptr || e->frame->isVisible());
}

void Workspace::detachPanel (const juce::String& id)
{
    auto* e = find (id);
    if (e == nullptr || e->window != nullptr || ! isPanelAvailable (id))
        return;

    // The same size as in the workspace.
    const auto size = e->panel->getBounds();
    e->frame->setVisible (false);
    e->frame->removeChildComponent (e->panel.get());
    e->window = std::make_unique<DetachedWindow> (*this, *e->panel, size);
    e->window->setVisible (true);
    repaint();
}

void Workspace::attachPanel (const juce::String& id)
{
    auto* e = find (id);
    if (e == nullptr || e->window == nullptr)
        return;

    e->window->clearContentComponent();
    e->window = nullptr;
    e->frame->attachContent();
    e->frame->setVisible (true);
    e->frame->toFront (true);
    repaint();
}

juce::ValueTree Workspace::getLayout() const
{
    juce::ValueTree layout ("LAYOUT");
    const auto w = (float)juce::jmax (1, getWidth()), h = (float)juce::jmax (1, getHeight());

    // Back to front: the workspace's children in order, then detached windows.
    std::vector<juce::String> ids;
    for (int i = 0; i < getNumChildComponents(); ++i)
        for (auto& [id, entry] : entries)
            if (entry->frame.get() == getChildComponent (i))
                ids.push_back (id);
    for (auto& id : order)
        if (std::find (ids.begin(), ids.end(), id) == ids.end())
            ids.push_back (id);

    for (auto& id : ids)
    {
        const auto& e = *entries.at (id);
        juce::ValueTree panel ("PANEL");
        panel.setProperty ("id", id, nullptr);
        panel.setProperty ("visible", isPanelVisible (id), nullptr);
        panel.setProperty ("detached", e.window != nullptr, nullptr);
        const auto b = e.frame->getBounds().toFloat();
        panel.setProperty ("x", b.getX() / w, nullptr);
        panel.setProperty ("y", b.getY() / h, nullptr);
        panel.setProperty ("w", b.getWidth() / w, nullptr);
        panel.setProperty ("h", b.getHeight() / h, nullptr);
        if (e.window != nullptr)
        {
            const auto s = e.window->getBounds();
            panel.setProperty ("sx", s.getX(), nullptr);
            panel.setProperty ("sy", s.getY(), nullptr);
            panel.setProperty ("sw", s.getWidth(), nullptr);
            panel.setProperty ("sh", s.getHeight(), nullptr);
        }
        layout.appendChild (panel, nullptr);
    }
    return layout;
}

void Workspace::setLayout (const juce::ValueTree& layout)
{
    const auto w = (float)getWidth(), h = (float)getHeight();
    std::set<juce::String> named;
    for (const auto& panel : layout)
    {
        const auto id = panel["id"].toString();
        auto* e = find (id);
        if (e == nullptr)
            continue;
        named.insert (id);

        if (! (bool)panel["visible"] || ! isPanelAvailable (id))
        {
            hidePanel (id);
            continue;
        }

        if (panel.hasProperty ("x") && w > 0.0f && h > 0.0f)
        {
            auto bounds = juce::Rectangle<float> ((float)panel["x"] * w, (float)panel["y"] * h, (float)panel["w"] * w,
                                                  (float)panel["h"] * h)
                              .toNearestInt();
            bounds = bounds.withSize (juce::jmin (bounds.getWidth(), getWidth()),
                                      juce::jmin (bounds.getHeight(), getHeight()));
            e->frame->setBounds (bounds.constrainedWithin (getLocalBounds()));
        }

        if ((bool)panel["detached"])
        {
            detachPanel (id);
            if (e->window != nullptr)
            {
                if (panel.hasProperty ("sw"))
                    e->window->setBounds ((int)panel["sx"], (int)panel["sy"], (int)panel["sw"], (int)panel["sh"]);
                e->window->toFront (false);
            }
        }
        else
        {
            attachPanel (id);
            showPanel (id); // in the layout's order: the last one ends up in front
        }
    }
    for (auto& [id, entry] : entries)
        if (! named.contains (id))
            hidePanel (id);
    repaint();
}

bool Workspace::isPanelDetached (const juce::String& id) const
{
    auto* e = find (id);
    return e != nullptr && e->window != nullptr;
}

juce::String Workspace::getFrontPanelId() const
{
    for (int i = getNumChildComponents(); --i >= 0;)
        if (auto* frame = dynamic_cast<Frame*> (getChildComponent (i)); frame != nullptr && frame->isVisible())
            for (auto& [id, entry] : entries)
                if (entry->frame.get() == frame)
                    return id;
    return {};
}

void Workspace::paint (juce::Graphics& g) { g.fillAll (themed (*this, "background", juce::Colours::black)); }

} // namespace spinus::ui
