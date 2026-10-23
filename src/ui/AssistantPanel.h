#pragma once

#include "ai/AiService.h"
#include "ai/Proposal.h"
#include "session/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The assistant in the right dock: ask about the song or about Spinus; the answer
    streams in, says which model gave it, and may come with a proposal: steps the user applies (one undo
    step) or rejects. Nothing changes without Apply (AGENTS rule 3).

    Context chips above the conversation say what the assistant sees: the project, the selected channel,
    pattern and mixer insert. Click one to leave it out.

    When AI is off it says so, with a button to set it up. */
class AssistantPanel final : public juce::Component
{
public:
    AssistantPanel (Session&, ai::AiService&);
    ~AssistantPanel() override;

    /** The Mixer's selected insert (a UI selection, not in the Session). */
    std::function<model::InsertId()> getSelectedInsert;
    /** "Set up AI..." (opens Options > AI). */
    std::function<void()> onSetUpAi;

    struct Entry
    {
        enum class Kind
        {
            user,
            assistant,
            proposal,
            error
        };
        Kind kind = Kind::assistant;
        juce::String text;
        juce::String model; // "claude-haiku-4-5 via Anthropic"
        std::shared_ptr<ai::Proposal> proposal;
        enum class Outcome
        {
            open,
            applied,
            rejected
        } outcome = Outcome::open;
    };

    //==============================================================================
    // For the controls and for tests.
    void ask (const juce::String& question);
    void stop();
    bool isAnswering() const noexcept { return job.isNotEmpty() || waitingForJob; }
    const std::vector<Entry>& getEntries() const noexcept { return entries; }
    /** The selectable text box showing an entry (tests), or nullptr. */
    juce::TextEditor* getEntryTextBox (int index) const;
    juce::String getStatusText() const { return status.getText(); }
    juce::Result applyProposal (int entry);
    void rejectProposal (int entry);
    void setChip (const juce::String& chip, bool on);
    bool isChipOn (const juce::String& chip) const;
    juce::var makeContext() const;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class ChatView;
    class EntryView;

    void received (const juce::String& method, const juce::var& params);
    void finish (const juce::var& result);
    void fail (const juce::String& message);
    void refresh();
    void updateChips();

    Session& session;
    ai::AiService& service;
    int listener = 0, stateListener = 0, historyListener = 0;
    std::vector<Entry> entries;
    juce::String job;
    bool waitingForJob = false;
    std::map<juce::String, bool> chipsOn {
        { "project", true }, { "channel", true }, { "pattern", true }, { "insert", true }
    };

    juce::TextButton projectChip, channelChip, patternChip, insertChip;
    std::unique_ptr<ChatView> chat;
    juce::Viewport viewport;
    juce::Label status, offMessage;
    juce::TextEditor input;
    juce::TextButton sendButton { "Send" }, setUpButton { "Set up AI..." };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace spinus::ui
