#include "Suggestions.h"

#include "commands/CommandRegistry.h"
#include "model/ModelJson.h"

namespace spinus
{

Suggestions::Suggestions (commands::CommandHistory& h) : history (h) {}

void Suggestions::show (std::vector<Suggestion> newVariations, Source newSource)
{
    variations = std::move (newVariations);
    source = std::move (newSource);
    current = 0;
    working.clear();
    if (listening)
        playbackChanged();
    sendChangeMessage();
}

void Suggestions::setWorking (const juce::String& message)
{
    working = message;
    sendChangeMessage();
}

const Suggestions::Suggestion* Suggestions::getCurrent() const
{
    return isShowing() ? &variations[(size_t)current] : nullptr;
}

void Suggestions::showVariation (int index)
{
    if (index < 0 || index >= getNumVariations() || index == current)
        return;
    current = index;
    if (listening)
        playbackChanged();
    sendChangeMessage();
}

void Suggestions::setListening (bool on)
{
    on = on && isShowing();
    if (on == listening)
        return;
    listening = on;
    playbackChanged();
    sendChangeMessage();
}

model::Project Suggestions::withSuggestion (const model::Project& project) const
{
    auto copy = project;
    if (const auto* suggestion = getCurrent())
        for (auto& pattern : copy.patterns)
            if (pattern.id == suggestion->pattern)
            {
                for (auto& [channel, notes] : suggestion->notes)
                    pattern.notes[channel] = notes;
                if (suggestion->patternLength)
                    pattern.length = juce::jmax (pattern.length, *suggestion->patternLength);
            }
    return copy;
}

void Suggestions::playbackChanged()
{
    if (onPlaybackChanged)
        onPlaybackChanged();
}

juce::Result Suggestions::accept()
{
    const auto* suggestion = getCurrent();
    if (suggestion == nullptr)
        return juce::Result::fail ("There is no suggestion to accept.");
    if (history.getProject().findPattern (suggestion->pattern) == nullptr)
        return juce::Result::fail ("The suggestion's pattern no longer exists.");

    // The notes are about to become the pattern's own (that change resyncs playback); if it fails,
    // playback goes back to the pattern's notes.
    const bool wasListening = std::exchange (listening, false);
    const auto fail = [&] (const juce::String& message)
    {
        if (wasListening)
            playbackChanged();
        return juce::Result::fail (message);
    };
    auto transaction = std::make_unique<commands::Transaction> (
        "Accept suggestion" + (suggestion->title.isNotEmpty() ? ": " + suggestion->title : juce::String()));

    for (auto& [channel, notes] : suggestion->notes)
    {
        juce::Array<juce::var> list;
        for (auto& n : notes)
            list.add (model::noteToJson (n));

        auto* params = new juce::DynamicObject();
        params->setProperty ("pattern", (int)suggestion->pattern.value);
        params->setProperty ("channel", (int)channel.value);
        params->setProperty ("notes", list);

        juce::String error;
        auto command = commands::CommandRegistry::get().create ("pattern.setNotes", juce::var (params), error);
        if (command == nullptr)
            return fail (error);
        transaction->add (std::move (command));
    }

    if (const auto* pattern = history.getProject().findPattern (suggestion->pattern);
        suggestion->patternLength && pattern != nullptr && *suggestion->patternLength > pattern->length)
    {
        juce::String error;
        auto grow = commands::CommandRegistry::get().create (
            "pattern.setLength",
            juce::JSON::parse ("{\"pattern\": " + juce::String (suggestion->pattern.value) +
                               ", \"length\": " + juce::String (*suggestion->patternLength) + "}"),
            error);
        if (grow == nullptr)
            return fail (error);
        transaction->add (std::move (grow));
    }

    if (auto result = history.perform (std::move (transaction)); result.failed())
        return fail (result.getErrorMessage());

    discard();
    return juce::Result::ok();
}

void Suggestions::discard()
{
    if (! isShowing() && working.isEmpty())
        return;
    const bool wasListening = listening;
    variations.clear();
    current = 0;
    source = {};
    working.clear();
    listening = false;
    if (wasListening)
        playbackChanged();
    sendChangeMessage();
}

bool Suggestions::regenerate()
{
    if (! source.regenerate)
        return false;
    auto ask = source.regenerate; // the source may call show() right away, replacing `source`
    ask();
    return true;
}

} // namespace spinus
