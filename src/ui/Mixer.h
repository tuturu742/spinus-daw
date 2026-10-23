#pragma once

#include "Workspace.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The Mixer (F9): Master plus any number of inserts as strips, with a "+" strip
    at the end that adds one. Each strip has a level meter, fader, pan, mute and solo. The selected
    insert's effects (any number; at least 10 slots shown, always one empty), output and sends are shown
    on the right.

    Mouse: click a strip to select it; drag its name sideways to reorder; double-click the name to
    rename; right-click for more (remove, "Remove unused inserts"). In the slots: click an empty slot
    to add an effect; double-click an effect to open its window; click the light to bypass; drag a slot onto another to
   reorder; right-click for more. Every edit is one Command, and one fader or knob drag is one undo step.

    Strips are created only for the visible part of the mixer, so hundreds of inserts stay fast.
*/
class Mixer final : public Panel,
                    public juce::DragAndDropTarget,
                    private juce::ChangeListener,
                    private juce::Timer,
                    private juce::AsyncUpdater,
                    private juce::ScrollBar::Listener
{
public:
    static constexpr int stripWidth = 78, masterWidth = 92, detailsWidth = 236;

    explicit Mixer (Session&);
    ~Mixer() override;

    //==============================================================================
    // Editing (used by the mouse handlers and by tests). Each call is one undo step, except that calls
    // with `joined` true join the previous step of the same kind (the rest of one drag).
    bool addInsert();
    bool removeInsert (model::InsertId);
    bool removeUnusedInserts();
    bool moveInsert (model::InsertId, int index);
    bool renameInsert (model::InsertId, const juce::String&);
    bool setInsertColour (model::InsertId, juce::uint32 argb);
    bool setInsertVolume (model::InsertId, double db, bool joined = false);
    bool setInsertPan (model::InsertId, double pan, bool joined = false);
    bool setInsertMuted (model::InsertId, bool);

    /** Sends a channel's notes to the insert's effects (invalid channel: none), e.g. for Spinus Tune. */
    bool setNotesFrom (model::InsertId, model::ChannelId);

    /** Freezes or unfreezes the insert (model/Freeze.h); fails when it cannot be frozen. */
    bool setInsertFrozen (model::InsertId, bool frozen);
    bool setInsertSolo (model::InsertId, bool);

    bool addEffect (model::InsertId, const juce::String& stockType, std::optional<int> slot = {});
    bool addEffect (model::InsertId, const model::Device&, std::optional<int> slot = {});
    bool removeEffect (model::DeviceId);
    /** The same effect with the same settings on another insert, after its last effect (one undo step). */
    bool copyEffect (model::DeviceId, model::InsertId to);
    bool moveEffect (model::DeviceId, model::InsertId, int slot);
    bool setEffectBypassed (model::DeviceId, bool);
    bool showEffectWindow (model::DeviceId);

    bool setOutput (model::InsertId, model::InsertId target);
    /** Feeds `source`'s sound into the insert's sidechain input; an invalid source removes it. */
    bool setSidechain (model::InsertId, model::InsertId source);
    bool setSend (model::InsertId, model::InsertId target, double levelDb, bool joined = false);
    bool removeSend (model::InsertId, model::InsertId target);

    /** Inserts with nothing routed into them and no effects (never the master). */
    std::vector<model::InsertId> findUnusedInserts() const;

    void selectInsert (model::InsertId);
    model::InsertId getSelectedInsert() const noexcept { return selected; }

    /** The message from the last refused edit, e.g. a feedback loop (empty after a success). */
    const juce::String& getLastError() const noexcept { return lastError; }

    /** Strip components that exist right now (only the visible ones). */
    int getNumStripComponents() const { return (int)strips.size(); }
    /** A visible strip's tooltip (its name, and where its sound comes from and goes); empty if not shown. */
    juce::String getStripHelpText (model::InsertId) const;
    void scrollToStrip (int index);

    /** Applies pending updates now (tests). */
    void updateNow() { handleUpdateNowIfNeeded(); }

    juce::Rectangle<int> getDefaultBounds() const override { return { 120, 90, 900, 420 }; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Effects dragged from the Browser: over a strip selects that insert; dropping adds the effect to
    // the end of its slots, or into the slot under the mouse.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class Strip;
    class AddStrip;
    class Details;

    bool perform (const juce::String& command, const juce::var& parameters, bool joined = false);
    int getNumStrips() const; // master + inserts + the "+" strip
    int stripX (int index) const;
    void layoutStrips();
    void layoutDetails();

public:
    /** Effect slots shown for the selected insert (tests). */
    int getNumShownSlots() const;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override { layoutStrips(); }

public:
    Session& session;

private:
    int historyListener = 0;
    model::InsertId selected = model::masterInsertId;
    juce::String lastError;
    bool wasRendering = false;
    std::vector<model::DeviceId> lastStopped;

    juce::Component stripArea;
    std::map<model::InsertId, std::unique_ptr<Strip>> strips;
    std::unique_ptr<AddStrip> addStrip;
    juce::ScrollBar scrollBar { false };
    std::unique_ptr<Details> details;
    juce::Viewport detailsView; // the details scroll when an insert has many effects or sends
};

} // namespace spinus::ui
