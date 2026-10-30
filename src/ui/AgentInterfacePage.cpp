#include "AgentInterfacePage.h"

#include "Theme.h"

namespace spinus::ui
{
namespace
{
const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}
} // namespace

AgentInterfacePage::AgentInterfacePage (ai::AgentInterface& a) : agents (a)
{
    intro.setText ("A coding agent on this computer (Claude Code, Codex, Cursor...) can read the open song, change it "
                   "and listen to it by numbers, while you watch and play it here. Its changes apply at once, each "
                   "one an undo step; Undo the agent's changes takes the song back to where it was when the agent "
                   "connected. Only programs on this computer with the token can connect; web pages can't.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    intro.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (intro);

    onOff.setHelpText ("The live interface for coding agents: off until you turn it on");
    onOff.onClick = [this] { agents.setEnabled (onOff.getToggleState()); };
    addAndMakeVisible (onOff);

    portField.setInputRestrictions (5, "0123456789");
    portField.setHelpText ("The port agents connect to (127.0.0.1 only)");
    portField.onReturnKey = portField.onFocusLost = [this] { agents.setPort (portField.getText().getIntValue()); };
    tokenField.setReadOnly (true);
    tokenField.setHelpText ("The token an agent needs: like a password, keep it to yourself");
    newTokenButton.setHelpText ("Make a new token: agents set up with the old one can't connect any more");
    newTokenButton.onClick = [this] { agents.newToken(); };
    copyTokenButton.onClick = [this] { copy (agents.getToken()); };
    for (auto* c : std::initializer_list<juce::Component*> { &portLabel, &portField, &tokenLabel, &tokenField,
                                                             &newTokenButton, &copyTokenButton })
        addAndMakeVisible (c);

    const std::array<std::pair<const char*, const char*>, 3> agentsShown {
        { { "claude", "Claude Code: run this in a terminal" },
          { "codex", "Codex: add this to ~/.codex/config.toml" },
          { "cursor", "Cursor: put this in .cursor/mcp.json (or merge it into yours)" } }
    };
    for (size_t i = 0; i < setups.size(); ++i)
    {
        auto& s = setups[i];
        s.id = agentsShown[i].first;
        s.title.setText (agentsShown[i].second, juce::dontSendNotification);
        s.text.setReadOnly (true);
        s.text.setMultiLine (true, true);
        s.text.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        s.copyButton.onClick = [this, &s] { copy (s.text.getText()); };
        addAndMakeVisible (s.title);
        addAndMakeVisible (s.text);
        addAndMakeVisible (s.copyButton);
    }

    statusText.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (statusText);
    disconnectButton.onClick = [this] { agents.disconnect(); };
    undoButton.setHelpText ("Back to the song as it was when the agent connected (your own changes since then too)");
    undoButton.onClick = [this] { agents.undoAgentChanges(); };
    backButton.onClick = [this]
    {
        if (onBack)
            onBack();
    };
    for (auto* b : { &disconnectButton, &undoButton, &backButton })
        addAndMakeVisible (b);
    refresh();
}

void AgentInterfacePage::copy (const juce::String& text) { juce::SystemClipboard::copyTextToClipboard (text); }

juce::String AgentInterfacePage::getSetupText (const juce::String& agent) const
{
    for (auto& s : setups)
        if (s.id == agent)
            return s.text.getText();
    return {};
}

void AgentInterfacePage::refresh()
{
    const bool on = agents.isEnabled();
    onOff.setToggleState (on, juce::dontSendNotification);
    if (! portField.hasKeyboardFocus (true))
        portField.setText (juce::String (agents.getPort()), juce::dontSendNotification);
    tokenField.setText (on ? agents.getToken() : juce::String(), juce::dontSendNotification);
    for (auto& s : setups)
    {
        s.text.setText (on ? agents.setupFor (s.id) : juce::String(), juce::dontSendNotification);
        s.copyButton.setEnabled (on);
    }
    for (auto* c :
         std::initializer_list<juce::Component*> { &portField, &tokenField, &newTokenButton, &copyTokenButton })
        c->setEnabled (on);

    juce::String status;
    if (! on)
        status = "Off. Agents can't connect.";
    else if (! agents.isListening())
        status = agents.getProblem();
    else if (agents.isAgentConnected())
        status = agents.getAgentName() + " is connected and has made " + juce::String (agents.getAgentChanges()) +
                 " changes.";
    else
        status = "On, at http://127.0.0.1:" + juce::String (agents.getPort()) + "/mcp. No agent connected.";
    statusText.setText (status, juce::dontSendNotification);
    disconnectButton.setVisible (agents.isAgentConnected());
    undoButton.setVisible (agents.isAgentConnected());
    resized();
}

void AgentInterfacePage::paint (juce::Graphics&)
{
    const auto& theme = themeOf (*this);
    intro.setColour (juce::Label::textColourId, theme.get ("text"));
    statusText.setColour (juce::Label::textColourId, theme.get ("textDim"));
    onOff.setColour (juce::ToggleButton::textColourId, theme.get ("text"));
    for (auto& s : setups)
        s.title.setColour (juce::Label::textColourId, theme.get ("textDim"));
}

void AgentInterfacePage::resized()
{
    auto area = getLocalBounds();
    intro.setBounds (area.removeFromTop (72));
    onOff.setBounds (area.removeFromTop (26));
    area.removeFromTop (6);
    auto row = area.removeFromTop (28);
    portLabel.setBounds (row.removeFromLeft (40));
    portField.setBounds (row.removeFromLeft (70).reduced (0, 2));
    row.removeFromLeft (12);
    tokenLabel.setBounds (row.removeFromLeft (48));
    copyTokenButton.setBounds (row.removeFromRight (60).reduced (0, 2));
    row.removeFromRight (6);
    newTokenButton.setBounds (row.removeFromRight (90).reduced (0, 2));
    row.removeFromRight (6);
    tokenField.setBounds (row.reduced (0, 2));
    area.removeFromTop (6);

    auto bottom = area.removeFromBottom (32);
    backButton.setBounds (bottom.removeFromRight (80));
    bottom.removeFromRight (8);
    undoButton.setBounds (bottom.removeFromRight (190));
    bottom.removeFromRight (8);
    disconnectButton.setBounds (bottom.removeFromRight (100));
    statusText.setBounds (area.removeFromBottom (40));

    const auto each = juce::jmax (60, area.getHeight() / 3);
    for (auto& s : setups)
    {
        auto box = area.removeFromTop (each);
        auto head = box.removeFromTop (22);
        s.copyButton.setBounds (head.removeFromRight (60).reduced (0, 1));
        s.title.setBounds (head);
        s.text.setBounds (box.reduced (0, 2));
    }
}

} // namespace spinus::ui
