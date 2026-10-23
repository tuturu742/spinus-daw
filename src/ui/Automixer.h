#pragma once

#include "core/FileDialog.h"
#include "Workspace.h"
#include "ai/AiService.h"
#include "ai/Proposal.h"
#include "engine/Exporter.h"
#include "session/HiddenInstrument.h"
#include "session/Session.h"
#include "MessageLabel.h"

namespace spinus::ui
{

/** The automixer: balances a mix and says why.

    1. Analyze: Spinus renders each mixer insert and the whole mix (offline, like an export with stems),
       and the AI service measures them: loudness (BS.1770), peaks, tone, width, and which inserts mask
       each other where (ai.v1.mix.analyze).
    2. Plan: rules make a list of changes with reasons ("Cut 250 Hz on Pad: masks Bass"), reviewed by the
       AI when a connection is set up (ai.v1.mix.plan). An optional loudness target adds a limiter on the
       master.
    3. The user unticks what they do not want and applies the rest as one undo step: fader and pan moves,
       and Spinus EQ, Compressor and Limiter effects with their settings (device.setParameters;
       Spinus's own effects, so nothing needs downloading).
    4. A/B: before and after, the after brought to the before's loudness, so louder does not sound better.

    It needs AI turned on (the measuring runs in the AI service), but not an AI connection. */
class Automixer final : public Panel
{
public:
    Automixer (Session&, ai::AiService*);
    ~Automixer() override;

    void setService (ai::AiService*);

    enum class Target
    {
        none,
        streaming, // -14 LUFS
        club       // -9 LUFS
    };

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.
    bool analyze (Target);
    /** A song whose tonal balance the master should move towards (D4); none when empty. */
    void setReference (const juce::File&);
    juce::File getReference() const { return reference; }
    void cancel();
    /** The ticked changes as one undo step ("Automixer: ..."). */
    bool apply();
    /** After apply: hear the mix before the changes (true) or after them (false), at matched loudness. */
    bool listen (bool before);
    /** Ends the comparison, keeping the changes. */
    void keep();

    struct Item
    {
        juce::var json; // {kind, insert, title, reason, ...} (spinus_ai.mixplan)
        bool ticked = true;
    };
    const std::vector<Item>& getItems() const noexcept { return items; }
    void setTicked (int index, bool);

    /** The proposal the ticked items make (prepared against the project; tests). */
    ai::Proposal makeProposal();

    bool isBusy() const noexcept { return busy; }
    bool canCompare() const noexcept { return compareReady; }
    bool isHearingBefore() const noexcept { return hearingBefore; }
    float getMatchTrim() const noexcept { return matchTrim; }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getNote() const { return note.getText(); }
    const juce::var& getMeasurements() const noexcept { return mix; }

    juce::Rectangle<int> getDefaultBounds() const override { return { 120, 80, 620, 480 }; }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class ItemRow;

    void render (bool withStems, const std::function<void (const std::vector<juce::File>&)>& then);
    void measure (const std::vector<juce::File>& files);
    void plan();
    void measureReference();
    void measureAfter();
    void runJob (const juce::String& method, const juce::var& params,
                 const std::function<void (const juce::var&)>& done);
    void fail (const juce::String&);
    void finish (const juce::String&);
    void updateRows();
    void updateButtons();
    juce::File getWorkFolder() const;
    juce::var settingValues (const juce::String& stockType,
                             const std::vector<std::pair<juce::String, juce::String>>& settings, juce::String& error);

    Session& session;
    ai::AiService* service;
    int listener = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    bool busy = false, compareReady = false, hearingBefore = false;
    juce::String job, undoName;
    double progress = -1.0;
    Target target = Target::none;
    std::unique_ptr<engine::ExportJob> exporter;
    std::map<juce::String, model::InsertId> insertsByName; // as the measurements name them
    juce::var mix, master, referenceMeasured;
    juce::File reference;
    float loudnessBefore = 0.0f, matchTrim = 0.0f;
    std::vector<Item> items;
    std::map<juce::String, std::unique_ptr<HiddenInstrument>> converters; // to turn "250 Hz" into 0..1
    mutable juce::File workFolder;

    MessageLabel status;
    juce::Label intro, note, targetLabel, referenceName;
    juce::ComboBox targetChoice;
    juce::TextButton analyzeButton { "Analyze the mix" }, cancelButton { "Cancel" },
        applyButton { "Apply ticked changes" }, beforeButton { "Before" }, afterButton { "After" },
        keepButton { "Keep" }, referenceButton { "Reference song..." }, clearReferenceButton { "No reference" };
    std::unique_ptr<core::FileDialog> chooser;
    juce::ProgressBar bar { progress };
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<ItemRow>> rows;
};

} // namespace spinus::ui
