#pragma once

#include "commands/CommandHistory.h"

#include <juce_events/juce_events.h>

namespace spinus
{

/** Notes suggested for a pattern (by the pattern generator, hum to notes, ...), shown over the pattern
    as ghost notes until the user accepts or discards them (AGENTS rule 3: AI proposes, the user
    decides). Accepting is one undo step; discarding changes nothing.

    A source may offer several variations at once (shown one at a time) and a way to make another
    suggestion (Regenerate). Only one suggestion is shown at a time: a new one replaces it.

    Message thread only. Sends a change message whenever what is shown changes.
*/
class Suggestions : public juce::ChangeBroadcaster
{
public:
    struct Suggestion
    {
        juce::String title; // what was asked for, e.g. "Funky bass line, 2 bars"
        model::PatternId pattern;
        /** Each channel's notes in the pattern after accepting (they replace that channel's notes). */
        std::map<model::ChannelId, std::vector<model::Note>> notes;
        /** The pattern's length after accepting, when the suggestion is longer than the pattern. */
        std::optional<model::Ticks> patternLength;
    };

    struct Source
    {
        juce::String name;                // e.g. "Pattern generator"
        std::function<void()> regenerate; // asks for another suggestion; empty when the source cannot
    };

    explicit Suggestions (commands::CommandHistory&);

    /** Shows the first of these variations, replacing what was shown. */
    void show (std::vector<Suggestion> variations, Source = {});

    /** While a source works on a suggestion (e.g. "Generating..."); cleared by show() or clear(). */
    void setWorking (const juce::String& message);
    juce::String getWorkingMessage() const { return working; }

    bool isShowing() const noexcept { return ! variations.empty(); }
    const Suggestion* getCurrent() const;
    const Source& getSource() const noexcept { return source; }

    int getNumVariations() const noexcept { return (int)variations.size(); }
    int getVariationIndex() const noexcept { return current; }
    void showVariation (int index);

    /** Writes the shown variation into the pattern as one undo step, then hides it. */
    juce::Result accept();

    /** Hides the suggestion; the project does not change. */
    void discard();

    /** Asks the source for another suggestion (it calls show() when ready). False if it cannot. */
    bool regenerate();

    /** Listen: playback uses the shown variation's notes instead of the pattern's, without changing the
        project (hear a suggestion before accepting it). Off again after Accept or
        Discard, or when nothing is shown. */
    void setListening (bool);
    bool isListening() const noexcept { return listening && isShowing(); }

    /** The project as it sounds while listening: the shown variation's notes in place of its channels'. */
    model::Project withSuggestion (const model::Project&) const;

    /** Called when what playback should use changes (listening turned on or off, another variation shown
        while listening); the session resyncs the engine. */
    std::function<void()> onPlaybackChanged;

private:
    commands::CommandHistory& history;
    std::vector<Suggestion> variations;
    int current = 0;
    Source source;
    juce::String working;
    bool listening = false;

    void playbackChanged();
};

} // namespace spinus
