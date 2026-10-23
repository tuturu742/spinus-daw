#pragma once

#include "ai/AiService.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The AI pattern generator: writes notes for chosen channels of the
    current pattern from what the user asks ("a funky bass line for these drums"), or changes their notes
    ("more syncopated", "add a fill in bar 4"), fitting the other channels' notes. The AI service
    (spinus_ai/patterns.py) asks the pattern generator's model and checks and finishes its notes; they
    come back as a suggestion, with up to four variations and Regenerate.

    Message thread only.
*/
class PatternGenerator final : private juce::ChangeListener
{
public:
    PatternGenerator (Session&, ai::AiService&);
    ~PatternGenerator() override;

    struct Request
    {
        juce::String text;
        std::vector<model::ChannelId> channels;
        int variations = 1;
        float swing = 0.0f, humanize = 0.0f; // 0..1
        bool adjust = false;                 // change their notes instead of writing new ones
        int bars = 0;                        // how long; 0: as long as the pattern
    };

    /** The length a request writes, in ticks. */
    model::Ticks lengthOf (const Request&) const;

    /** Asks for a pattern for the current pattern; the result arrives as a suggestion. */
    juce::Result generate (const Request&);

    /** The parameters of ai.v1.pattern.generate for a request (tests). */
    juce::var describe (const Request&) const;

    bool isBusy() const noexcept { return job.isNotEmpty() || waiting; }

    /** The window under the Generate button. */
    void showDialog (juce::Component& button);

    /** The variations the AI service wrote (ai.v1.pattern.generate's "variations") as suggestions. */
    static std::vector<Suggestions::Suggestion> toSuggestions (const juce::var& variations, model::PatternId,
                                                               const juce::String& fallbackTitle);

    /** A Drum Machine channel's pads (name and note), or nothing for other instruments. */
    static std::vector<std::pair<juce::String, int>> getPads (Session&, model::ChannelId);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void finished (const juce::var& result, const Request&, model::PatternId);
    void fail (const juce::String&);

    Session& session;
    ai::AiService& service;
    int listener = 0;
    juce::String job;
    bool waiting = false; // for the job's ID
    std::function<void (const juce::var&)> whenDone;

    JUCE_DECLARE_NON_COPYABLE (PatternGenerator)
};

} // namespace spinus::ui
