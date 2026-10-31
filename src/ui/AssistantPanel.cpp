#include "AssistantPanel.h"

#include "PatternGenerator.h"

#include "Theme.h"
#include "ai/ProjectContext.h"
#include "session/AgentCommandLine.h"
#include "model/ModelJson.h"
#include "model/StockDevices.h"

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

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

constexpr int pad = 8;

juce::TextLayout layoutFor (const juce::String& text, float width, juce::Colour colour, float size = 13.0f)
{
    juce::AttributedString s;
    s.append (text, juce::FontOptions (size), colour);
    s.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout (s, juce::jmax (40.0f, width));
    return layout;
}
} // namespace

//==============================================================================
/** One message, or one proposal with its steps and Apply / Reject. */
class AssistantPanel::EntryView final : public juce::Component
{
public:
    EntryView (AssistantPanel& p, int i) : panel (p), index (i)
    {
        apply.onClick = [this]
        {
            if (auto r = panel.applyProposal (index); r.failed())
                panel.status.setText (r.getErrorMessage(), juce::dontSendNotification);
        };
        reject.onClick = [this] { panel.rejectProposal (index); };
        addChildComponent (apply);
        addChildComponent (reject);

        // The text can be selected and copied.
        body.setMultiLine (true, true);
        body.setReadOnly (true);
        body.setCaretVisible (false);
        body.setScrollbarsShown (false);
        body.setPopupMenuEnabled (true);
        body.setIndents (0, 0);
        body.setBorder ({});
        body.setFont (juce::FontOptions (13.0f));
        body.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        body.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        body.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        body.setMouseCursor (juce::MouseCursor::IBeamCursor);
        addAndMakeVisible (body);
    }

    /** The message's text box (tests). */
    juce::TextEditor& getBody() noexcept { return body; }

    const Entry& entry() const { return panel.entries[(size_t)index]; }

    juce::String bodyText() const
    {
        const auto& e = entry();
        if (e.kind != Entry::Kind::proposal || e.proposal == nullptr)
            return e.text;
        juce::String text = "Proposal: " + e.proposal->summary + "\n";
        for (size_t i = 0; i < e.proposal->steps.size(); ++i)
            text << "\n" << juce::String ((int)i + 1) << ". " << e.proposal->steps[i].description;
        if (! e.proposal->error.isEmpty())
            text << "\n\n" << e.proposal->error;
        if (e.outcome == Entry::Outcome::applied)
            text << "\n\nApplied. Ctrl+Z undoes it.";
        else if (e.outcome == Entry::Outcome::rejected)
            text << "\n\nRejected.";
        return text;
    }

    int textHeightFor (int width) const
    {
        const auto& theme = themeOf (*this);
        auto layout = layoutFor (bodyText(), (float)(width - 4 * pad - 6), theme.get ("text"));
        return (int)std::ceil (layout.getHeight()) + 4;
    }

    int heightFor (int width) const
    {
        int h = textHeightFor (width) + 2 * pad;
        if (entry().model.isNotEmpty())
            h += 16;
        if (showsButtons())
            h += 34;
        return h + pad;
    }

    bool showsButtons() const
    {
        const auto& e = entry();
        return e.kind == Entry::Kind::proposal && e.outcome == Entry::Outcome::open && e.proposal != nullptr &&
               e.proposal->canApply();
    }

    void refresh()
    {
        apply.setVisible (showsButtons());
        reject.setVisible (showsButtons());
        const auto& theme = themeOf (*this);
        body.setColour (juce::TextEditor::textColourId, theme.get ("text"));
        body.setColour (juce::TextEditor::highlightColourId, theme.get ("accent").withAlpha (0.4f));
        if (body.getText() != bodyText())
            body.setText (bodyText(), false);
        resized();
        repaint();
    }

    void resized() override
    {
        body.setBounds (getLocalBounds().reduced (2 * pad, pad).removeFromTop (textHeightFor (getWidth())));
        auto r = getLocalBounds().reduced (2 * pad, pad).removeFromBottom (30);
        apply.setBounds (r.removeFromLeft (90).reduced (0, 2));
        r.removeFromLeft (8);
        reject.setBounds (r.removeFromLeft (90).reduced (0, 2));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = themeOf (*this);
        const auto& e = entry();
        auto box = getLocalBounds().reduced (pad / 2, pad / 2).toFloat();
        const auto fill = e.kind == Entry::Kind::user       ? theme.get ("accent").withAlpha (0.18f)
                          : e.kind == Entry::Kind::proposal ? theme.get ("panel").brighter (0.08f)
                          : e.kind == Entry::Kind::error    ? theme.get ("danger").withAlpha (0.15f)
                                                            : theme.get ("panel");
        g.setColour (fill);
        g.fillRoundedRectangle (box, 6.0f);
        if (e.kind == Entry::Kind::proposal)
        {
            g.setColour (theme.get ("accent"));
            g.drawRoundedRectangle (box, 6.0f, 1.0f);
        }

        auto text = getLocalBounds().reduced (2 * pad, pad).toFloat();
        text.removeFromTop ((float)textHeightFor (getWidth()));
        if (e.model.isNotEmpty())
        {
            g.setColour (theme.get ("textDim"));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (e.model, text.removeFromTop (16.0f), juce::Justification::centredLeft, true);
        }
    }

private:
    AssistantPanel& panel;
    int index;
    juce::TextButton apply { "Apply" }, reject { "Reject" };
    juce::TextEditor body;
};

/** The conversation, top to bottom. */
class AssistantPanel::ChatView final : public juce::Component
{
public:
    explicit ChatView (AssistantPanel& p) : panel (p) {}

    void rebuild()
    {
        while ((int)views.size() < (int)panel.entries.size())
        {
            views.push_back (std::make_unique<EntryView> (panel, (int)views.size()));
            addAndMakeVisible (*views.back());
        }
        views.resize (panel.entries.size());
        layout (getWidth());
    }

    void layout (int width)
    {
        int y = pad;
        for (auto& v : views)
        {
            const auto h = v->heightFor (width);
            v->setBounds (0, y, width, h);
            v->refresh();
            y += h;
        }
        setSize (width, y + pad);
    }

    void paint (juce::Graphics& g) override
    {
        if (! views.empty())
            return;
        g.setColour (themeOf (*this).get ("textDim"));
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Ask about your song (\"why is my kick quiet?\") or about Spinus (\"how do I sidechain?\"). "
                          "Changes come as proposals you apply or reject.",
                          getLocalBounds().reduced (12), juce::Justification::topLeft, 6);
    }

    std::vector<std::unique_ptr<EntryView>> views;

private:
    AssistantPanel& panel;
};

//==============================================================================
AssistantPanel::AssistantPanel (Session& s, ai::AiService& a) : session (s), service (a)
{
    chat = std::make_unique<ChatView> (*this);
    viewport.setViewedComponent (chat.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    for (auto [button, chip] : { std::pair { &projectChip, "project" }, std::pair { &channelChip, "channel" },
                                 std::pair { &patternChip, "pattern" }, std::pair { &insertChip, "insert" } })
    {
        button->setClickingTogglesState (true);
        button->setToggleState (true, juce::dontSendNotification);
        button->setTooltip ("What the assistant sees; click to leave it out");
        button->onClick = [this, button, c = juce::String (chip)] { setChip (c, button->getToggleState()); };
        addAndMakeVisible (*button);
    }

    status.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (status);
    input.setMultiLine (true, true);
    input.setReturnKeyStartsNewLine (false);
    input.setTextToShowWhenEmpty ("Ask the assistant...", juce::Colours::grey);
    input.onReturnKey = [this] { sendButton.triggerClick(); };
    addAndMakeVisible (input);
    sendButton.onClick = [this]
    {
        if (isAnswering())
            stop();
        else
            ask (input.getText());
    };
    addAndMakeVisible (sendButton);

    offMessage.setText ("The assistant is off. AI features stay off until you turn them on and choose a provider "
                        "(or a local model).",
                        juce::dontSendNotification);
    offMessage.setJustificationType (juce::Justification::topLeft);
    addChildComponent (offMessage);
    setUpButton.onClick = [this]
    {
        if (onSetUpAi)
            onSetUpAi();
    };
    addChildComponent (setUpButton);

    listener = service.addListener ([this] (const juce::String& method, const juce::var& params)
                                    { received (method, params); });
    stateListener = service.addStateListener ([this] { refresh(); });
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { updateChips(); });
    refresh();
}

AssistantPanel::~AssistantPanel()
{
    *alive = false;
    session.getHistory().removeListener (historyListener);
    service.removeStateListener (stateListener);
    service.removeListener (listener);
}

//==============================================================================
juce::var AssistantPanel::makeContext() const
{
    ai::ContextOptions options;
    options.chips.clear();
    for (auto& [chip, on] : chipsOn)
        if (on)
            options.chips.add (chip);
    options.project = chipsOn.at ("project");
    if (chipsOn.at ("channel"))
        options.channel = session.getSelectedChannel();
    if (chipsOn.at ("pattern"))
        options.pattern = session.getCurrentPattern();
    if (chipsOn.at ("insert") && getSelectedInsert)
        options.insert = getSelectedInsert();
    return ai::projectContext (session, options);
}

void AssistantPanel::ask (const juce::String& question)
{
    const auto text = question.trim();
    if (text.isEmpty() || isAnswering() || ! service.isEnabled())
        return;
    entries.push_back ({ Entry::Kind::user, text, {}, {}, Entry::Outcome::open });
    input.clear();

    // The conversation so far, as text (proposals as one line each).
    juce::Array<juce::var> conversation;
    for (auto& e : entries)
    {
        if (e.kind == Entry::Kind::user)
            conversation.add (object ({ { "role", "user" }, { "text", e.text } }));
        else if (e.kind == Entry::Kind::assistant)
            conversation.add (object ({ { "role", "assistant" }, { "text", e.text } }));
        else if (e.kind == Entry::Kind::proposal && e.proposal != nullptr)
            conversation.add (
                object ({ { "role", "assistant" },
                          { "text", "(Proposed: " + e.proposal->summary + " - " +
                                        (e.outcome == Entry::Outcome::applied    ? "applied by the user)"
                                         : e.outcome == Entry::Outcome::rejected ? "rejected by the user)"
                                                                                 : "not applied yet)") } }));
    }

    entries.push_back ({ Entry::Kind::assistant, {}, {}, {}, Entry::Outcome::open });
    waitingForJob = true;
    status.setText ("Thinking...", juce::dontSendNotification);
    service.call ("ai.v1.assistant.ask", object ({ { "conversation", conversation }, { "context", makeContext() } }),
                  [this, a = alive] (const juce::var& result, const juce::String& error)
                  {
                      if (! *a)
                          return;
                      waitingForJob = false;
                      if (error.isNotEmpty())
                          return fail (error);
                      job = result["job"].toString();
                      refresh();
                  });
    refresh();
}

void AssistantPanel::stop()
{
    if (job.isNotEmpty())
        service.call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

void AssistantPanel::received (const juce::String& method, const juce::var& params)
{
    if (method != "ai.v1.job.update" || params["job"].toString() != job || entries.empty())
        return;
    const auto kind = params["kind"].toString();
    if (kind == "text")
    {
        entries.back().text += params["text"].toString();
        chat->layout (viewport.getMaximumVisibleWidth());
        viewport.setViewPosition (0, juce::jmax (0, chat->getHeight() - viewport.getHeight()));
    }
    else if (kind == "status")
        status.setText (params["text"].toString(), juce::dontSendNotification);
    else if (kind == "done")
        finish (params["result"]);
    else if (kind == "error")
        fail (params["message"].toString());
}

void AssistantPanel::finish (const juce::var& result)
{
    job.clear();
    auto& answer = entries.back();
    answer.text = result["text"].toString().trim();
    answer.model = result["model"].toString() + " via " + result["connection"].toString();
    if (answer.text.isEmpty())
        answer.text = "(No text.)";
    if (auto* proposals = result["proposals"].getArray())
        for (auto& p : *proposals)
        {
            Entry e;
            e.kind = Entry::Kind::proposal;
            // Automation points the project harness gave as text ("-12 dB") are read for their targets here.
            juce::String textError;
            auto checked = p.clone();
            if (auto* o = checked.getDynamicObject())
                o->setProperty ("steps", cli::readStepTexts (session, p["steps"], textError));
            e.proposal = std::make_shared<ai::Proposal> (ai::Proposal::prepare (checked, session.getHistory()));
            if (textError.isNotEmpty() && e.proposal->error.isEmpty())
                e.proposal->error = textError;
            entries.push_back (e);
        }
    status.setText ({}, juce::dontSendNotification);

    // Notes the pattern generator wrote for the assistant: a suggestion in the Piano Roll.
    if (auto* suggestions = result["suggestions"].getArray(); suggestions != nullptr && ! suggestions->isEmpty())
    {
        const auto& first = suggestions->getReference (0);
        const model::PatternId pattern { (uint32_t)(int)first["pattern"] };
        auto variations = PatternGenerator::toSuggestions (first["variations"], pattern, "From the assistant");
        if (! variations.empty() && session.getProject().findPattern (pattern) != nullptr)
        {
            session.setCurrentPattern (pattern);
            session.setSelectedChannel (variations.front().notes.begin()->first);
            session.getSuggestions().show (std::move (variations), { "Assistant", nullptr });
            status.setText ("The suggested notes are in the Piano Roll (F7): Accept or Discard them there.",
                            juce::dontSendNotification);
        }
    }
    refresh();
}

void AssistantPanel::fail (const juce::String& message)
{
    job.clear();
    waitingForJob = false;
    if (! entries.empty() && entries.back().kind == Entry::Kind::assistant && entries.back().text.isEmpty())
        entries.pop_back();
    entries.push_back ({ Entry::Kind::error, message, {}, {}, Entry::Outcome::open });
    status.setText ({}, juce::dontSendNotification);
    refresh();
}

juce::Result AssistantPanel::applyProposal (int index)
{
    if (! juce::isPositiveAndBelow (index, (int)entries.size()) || entries[(size_t)index].proposal == nullptr ||
        entries[(size_t)index].outcome != Entry::Outcome::open)
        return juce::Result::fail ("Nothing to apply.");
    auto& e = entries[(size_t)index];
    const auto result = e.proposal->apply (session.getHistory());
    if (result.wasOk())
        e.outcome = Entry::Outcome::applied;
    refresh();
    return result;
}

void AssistantPanel::rejectProposal (int index)
{
    if (juce::isPositiveAndBelow (index, (int)entries.size()) && entries[(size_t)index].proposal != nullptr)
        entries[(size_t)index].outcome = Entry::Outcome::rejected;
    refresh();
}

void AssistantPanel::setChip (const juce::String& chip, bool on)
{
    if (chipsOn.contains (chip))
        chipsOn[chip] = on;
    updateChips();
}

bool AssistantPanel::isChipOn (const juce::String& chip) const
{
    auto it = chipsOn.find (chip);
    return it != chipsOn.end() && it->second;
}

void AssistantPanel::updateChips()
{
    const auto& project = session.getProject();
    const auto* channel = project.findChannel (session.getSelectedChannel());
    const auto* pattern = project.findPattern (session.getCurrentPattern());
    const auto* insert = getSelectedInsert ? project.findInsert (getSelectedInsert()) : nullptr;
    projectChip.setButtonText ("Project");
    channelChip.setButtonText (channel != nullptr ? channel->name : "No channel");
    patternChip.setButtonText (pattern != nullptr ? pattern->name : "No pattern");
    insertChip.setButtonText (insert != nullptr
                                  ? (insert->id == model::masterInsertId ? juce::String ("Master") : insert->name)
                                  : "No insert");
    for (auto [button, chip, present] :
         { std::tuple { &projectChip, "project", true }, std::tuple { &channelChip, "channel", channel != nullptr },
           std::tuple { &patternChip, "pattern", pattern != nullptr },
           std::tuple { &insertChip, "insert", insert != nullptr } })
    {
        button->setEnabled (present);
        button->setToggleState (present && isChipOn (chip), juce::dontSendNotification);
        button->setAlpha (present && isChipOn (chip) ? 1.0f : 0.45f);
    }
}

void AssistantPanel::refresh()
{
    const bool on = service.isEnabled();
    offMessage.setVisible (! on);
    setUpButton.setVisible (! on);
    for (auto* c : std::initializer_list<juce::Component*> { &viewport, &status, &input, &sendButton, &projectChip,
                                                             &channelChip, &patternChip, &insertChip })
        c->setVisible (on);
    sendButton.setButtonText (isAnswering() ? "Stop" : "Send");
    updateChips();
    chat->rebuild();
    chat->layout (viewport.getMaximumVisibleWidth());
    viewport.setViewPosition (0, juce::jmax (0, chat->getHeight() - viewport.getHeight()));
    resized();
    repaint();
}

void AssistantPanel::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panelHeader"));
    g.setColour (theme.get ("border"));
    g.fillRect (0, 0, 1, getHeight());
    g.setColour (theme.get ("text"));
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("Assistant", 10, 4, getWidth() - 20, 20, juce::Justification::centredLeft);
    status.setColour (juce::Label::textColourId, theme.get ("textDim"));
    offMessage.setColour (juce::Label::textColourId, theme.get ("text"));
}

juce::TextEditor* AssistantPanel::getEntryTextBox (int index) const
{
    if (chat == nullptr || ! juce::isPositiveAndBelow (index, (int)chat->views.size()))
        return nullptr;
    return &chat->views[(size_t)index]->getBody();
}

void AssistantPanel::resized()
{
    auto area = getLocalBounds().reduced (8, 4);
    area.removeFromTop (24);
    if (! service.isEnabled())
    {
        offMessage.setBounds (area.removeFromTop (90));
        setUpButton.setBounds (area.removeFromTop (30).withWidth (140));
        return;
    }

    auto chips = area.removeFromTop (48);
    auto top = chips.removeFromTop (24);
    projectChip.setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (1));
    channelChip.setBounds (top.reduced (1));
    patternChip.setBounds (chips.removeFromLeft (chips.getWidth() / 2).reduced (1));
    insertChip.setBounds (chips.reduced (1));

    auto bottom = area.removeFromBottom (64);
    sendButton.setBounds (bottom.removeFromRight (60).reduced (2));
    input.setBounds (bottom.reduced (2));
    status.setBounds (area.removeFromBottom (20));
    viewport.setBounds (area);
    chat->layout (viewport.getMaximumVisibleWidth());
}

} // namespace spinus::ui
