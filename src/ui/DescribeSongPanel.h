#pragma once

#include "MessageLabel.h"
#include "ai/AiService.h"

#include <functional>
#include <memory>

namespace spinus::ui
{

/** The Track creator's second way in (experimental): a song described in words ("a sunny, upbeat indie pop song
    at 118 BPM in G major, with acoustic guitar") instead of songs you like.

    Read asks the AI service (ai.v1.song.describe, spinus_ai/describe.py) to read the words into a style summary,
    the same kind the songs you like give: rules on this computer by default, from a table of genres and the
    words for tempo, key, mood, instruments and what to leave out. With "Let the AI read it" and an AI connection,
    the AI reads it instead (checked; the rules' reading stands when it fails, and the page says so). What was
    understood is shown in plain words ("Understood: 118 BPM, G major, sunny, upbeat; genre: indie pop"); the
    Sounds for each part and Write a song pages then work from it, with the sounds you have.

    Nothing changes the project. Without the AI service the page says so. Message thread only. */
class DescribeSongPanel final : public juce::Component
{
public:
    explicit DescribeSongPanel (std::function<ai::AiService*()> service);
    ~DescribeSongPanel() override;

    /** Reads the description (asynchronously); false when there is nothing to read or no AI service. */
    bool read();
    /** Called with the summary once a description is read. */
    std::function<void (const juce::var& summary)> onRead;

    void setText (const juce::String& text) { textBox.setText (text, false); }
    juce::String getText() const { return textBox.getText(); }
    void setUseAi (bool on) { aiButton.setToggleState (on, juce::dontSendNotification); }
    /** A summary read before (the Track creator remembers it), shown as if just read. */
    void show (const juce::var& summary);

    bool isBusy() const noexcept { return busy; }
    const juce::var& getSummary() const noexcept { return summary; }
    juce::String getUnderstoodText() const { return understood.getText(); }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getDetailsText() const { return details.getText(); }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void updateButtons();

    std::function<ai::AiService*()> getService;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    bool busy = false;
    juce::String job;
    juce::var summary;

    juce::Label intro;
    juce::TextEditor textBox;
    juce::ToggleButton aiButton { "Let the AI read it (uses your AI connection)" };
    juce::TextButton readButton { "Read" };
    MessageLabel understood, status;
    juce::TextEditor details;
};

} // namespace spinus::ui
