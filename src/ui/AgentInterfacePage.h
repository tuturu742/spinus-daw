#pragma once

#include "ai/AgentInterface.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Options > AI > Coding agents: Spinus's live interface for outside coding agents (Claude Code, Codex, Cursor).

    Turns it on or off, shows the address and token, how to set up each agent (to copy), and while an agent is
    connected, who, how many changes it made, Disconnect and Undo the agent's changes. Works with AI on or off:
    the music tools need AI, Spinus's own tools don't. */
class AgentInterfacePage final : public juce::Component
{
public:
    explicit AgentInterfacePage (ai::AgentInterface&);

    std::function<void()> onBack;

    /** Reads the interface's state again (after it changes). */
    void refresh();
    /** What each agent's setup box shows, for tests. */
    juce::String getSetupText (const juce::String& agent) const;
    juce::String getStatusText() const { return statusText.getText(); }
    juce::ToggleButton& getOnOff() noexcept { return onOff; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void copy (const juce::String& text);

    ai::AgentInterface& agents;
    juce::ToggleButton onOff { "Let coding agents work on the open project" };
    juce::Label intro, statusText, portLabel { {}, "Port" }, tokenLabel { {}, "Token" };
    juce::TextEditor portField, tokenField;
    juce::TextButton newTokenButton { "New token" }, copyTokenButton { "Copy" };
    struct Setup
    {
        juce::String id;
        juce::Label title;
        juce::TextEditor text;
        juce::TextButton copyButton { "Copy" };
    };
    std::array<Setup, 3> setups;
    juce::TextButton disconnectButton { "Disconnect" }, undoButton { "Undo the agent's changes" },
        backButton { "Back" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AgentInterfacePage)
};

} // namespace spinus::ui
