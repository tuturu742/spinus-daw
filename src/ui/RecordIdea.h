#pragma once

#include "ai/AiService.h"
#include "session/IdeaToNotes.h"
#include "session/Session.h"

namespace spinus::ui
{

/** "Record idea" in the Piano Roll: records a take over the current
    pattern, asks the AI service what it heard and shows the notes as a suggestion for the
    selected channel, to Accept or Discard. Also teaches the AI service the user's beatbox sounds.

    What is happening shows in the Piano Roll's suggestion bar; its Discard button stops it.

    Message thread only.
*/
class RecordIdea final : private juce::ChangeListener, private juce::Timer
{
public:
    RecordIdea (Session&, ai::AiService&);
    ~RecordIdea() override;

    enum class Kind
    {
        melody,
        beats
    };

    /** Records a take over the current pattern for the selected channel. Fails with the reason. */
    juce::Result record (Kind);

    /** Teaches the beatbox sounds: a few seconds each of kick, snare and hat. */
    juce::Result teach();

    /** Asks what was heard in a take and shows the suggestion (record() does this after recording). */
    void analyse (Kind, const IdeaRecorder::Take&);

    /** The menu under the Record idea button. */
    void showMenu (juce::Component& target);

    bool isBusy() const noexcept { return step != Step::idle; }

    static constexpr double teachSeconds = 4.0;

private:
    enum class Step
    {
        idle,
        recording,
        analysing,
        teaching, // recording one sound
        learning  // the AI service learning it
    };

    juce::Result canListen() const;
    void callJob (const juce::String& method, const juce::var& params, std::function<void (const juce::var&)> done);
    void tell (const juce::String& message, bool clearSoon = false);
    void stop (const juce::String& message = {});
    void teachNext();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    Session& session;
    ai::AiService& service;
    int listener = 0;
    Step step = Step::idle;
    Kind kind = Kind::melody;
    model::ChannelId channel;
    model::PatternId pattern;
    juce::String job;
    std::function<void (const juce::var&)> whenDone;
    int teaching = 0; // which sound: 0 kick, 1 snare, 2 hat
    juce::String shown;
    juce::uint32 clearAt = 0;

    JUCE_DECLARE_NON_COPYABLE (RecordIdea)
};

} // namespace spinus::ui
