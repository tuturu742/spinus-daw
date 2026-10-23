#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <set>

namespace spinus::ui
{

/** A Spinus panel (Channel Rack, Piano Roll, Playlist, Mixer, …) that the Workspace can show. */
class Panel : public juce::Component
{
public:
    Panel (juce::String panelId, juce::String title) : identifier (std::move (panelId)), panelTitle (std::move (title))
    {
        setTitle (panelTitle); // for screen readers
    }

    const juce::String& getPanelId() const noexcept { return identifier; }
    const juce::String& getPanelTitle() const noexcept { return panelTitle; }

    /** Size the panel opens with the first time. */
    virtual juce::Rectangle<int> getDefaultBounds() const { return { 40, 40, 640, 360 }; }

private:
    juce::String identifier, panelTitle;
};

/** The centre of the main window. Panels float inside it like FL Studio windows: they
    can be moved, resized, brought to front, closed and reopened, or detached into their own window
    and docked back again.
*/
class Workspace : public juce::Component
{
public:
    Workspace();
    ~Workspace() override;

    /** Takes ownership. Panels start hidden. */
    void addPanel (std::unique_ptr<Panel>);

    Panel* getPanel (const juce::String& id) const;
    juce::StringArray getPanelIds() const;

    void showPanel (const juce::String& id); // also brings it to front, or focuses its window
    void hidePanel (const juce::String& id);
    void togglePanel (const juce::String& id);
    bool isPanelVisible (const juce::String& id) const;

    /** An unavailable panel (an experimental one while experimental features are off) is hidden and can't be
        shown, detached or brought back by a layout until it is available again. Panels start available. */
    void setPanelAvailable (const juce::String& id, bool available);
    bool isPanelAvailable (const juce::String& id) const { return ! unavailable.contains (id); }

    void detachPanel (const juce::String& id); // into its own window
    void attachPanel (const juce::String& id); // back into the workspace
    bool isPanelDetached (const juce::String& id) const;

    /** Where every panel is (window layouts): a LAYOUT tree with one PANEL per panel, back to
        front, each with `visible`, `detached` and its bounds as fractions of the workspace (x, y, w, h), or
        on the screen for a detached one (sx, sy, sw, sh). */
    juce::ValueTree getLayout() const;
    /** Puts the panels where a layout says. Panels the layout doesn't name are hidden; bounds are kept inside
        the workspace. */
    void setLayout (const juce::ValueTree&);

    /** The frontmost visible panel inside the workspace, or empty. */
    juce::String getFrontPanelId() const;

    void paint (juce::Graphics&) override;

private:
    class Frame;
    class DetachedWindow;

    struct Entry
    {
        std::unique_ptr<Panel> panel;
        std::unique_ptr<Frame> frame;
        std::unique_ptr<DetachedWindow> window;
    };

    Entry* find (const juce::String& id) const;

    std::map<juce::String, std::unique_ptr<Entry>> entries;
    juce::StringArray order; // insertion order, for getPanelIds()
    std::set<juce::String> unavailable;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Workspace)
};

} // namespace spinus::ui
