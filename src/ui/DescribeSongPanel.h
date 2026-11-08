#pragma once

#include "MessageLabel.h"
#include "ai/AiService.h"

#include <functional>
#include <memory>

namespace spinus::ui
{

/** The Track creator's second step, the description (experimental): the song in words ("a sunny, upbeat indie pop
    song at 118 BPM in G major, with acoustic guitar"). With reference songs it starts as their summary in words
    ("Like your songs: about 128 BPM, in A minor, four-on-the-floor drums; with drums, bass...") to edit as a
    prompt: the words changed win, the songs' detailed style stays underneath for the rest.

    Read asks the AI service (ai.v1.song.describe, spinus_ai/describe.py) to read the words into a style summary,
    the same kind the songs you like give: rules on this computer by default, from a table of genres and the
    words for tempo, key, mood, instruments and what to leave out. With "Let the AI read it" and an AI connection,
    the AI reads it instead (checked; the rules' reading stands when it fails, and the page says so). What was
    understood is shown in plain words ("Understood: 118 BPM, G major, sunny, upbeat; genre: indie pop"); the
    Sounds for each part and Write a song pages then work from it, with the sounds you have.

    Lyrics (optional, in their own box under the description): the song's words, tagged by section the way song
    generators take them ("[Verse 1]", "[Chorus]", "[Guitar Solo]"). Read reads them too (spinus_ai/lyrics.py): the
    sections and each line's syllables, counted by rules on this computer, or by the AI when it reads. They go into
    the summary as its `lyrics`, for the vocal part and the song's form. "This song has no vocals" leaves them out
    (kept in the box, not read). Lyrics without tags are not guessed at: the page says to add them, or, with the
    AI, shows the AI's tagged version under the box, to use (Use these tags: it replaces the box's text, one undo
    step in the box, and is read) or not (Keep mine). Nothing is used until the user says so.

    How much of the song is sung: once a song with lyrics is read, a range slider under the lyrics shows the genre's
    range (the summary's `described.vocals`, e.g. 10% to 70% for drum and bass); the song writer picks a share inside
    it and gives the rest to instrumental sections. Moving it sets the summary's `vocalShare` ({low, high}, 0 to 1,
    at least 5% apart), kept when the description is read again; Genre's range takes it back. Hidden for an
    instrumental (no lyrics, or "This song has no vocals").

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
    void setLyrics (const juce::String& text) { lyricsBox.setText (text, false); }
    juce::String getLyrics() const { return lyricsBox.getText(); }
    /** "This song has no vocals": the lyrics are kept but not read. */
    void setNoVocals (bool on);
    bool hasNoVocals() const { return noVocalsButton.getToggleState(); }
    /** The user's range for how much is sung ({low, high}, 0 to 1); void when the genre's range stands. */
    juce::var getVocalShare() const { return userShare; }
    /** Sets the user's range (void: the genre's), as the slider does; the read summary carries it. */
    void setVocalShare (const juce::var& share);
    /** Called when the user changes how much is sung. */
    std::function<void()> onShareChanged;
    bool isShareShown() const { return shareSlider.isVisible(); }
    juce::String getShareText() const { return shareLabel.getText(); }
    /** Empties the page (another project): no text, lyrics, reading or references; a reading under way stops. */
    void clear();
    /** A summary read before (the Track creator remembers it), shown as if just read. */
    void show (const juce::var& summary);
    /** The reference songs' summary and the description written from it (ai.v1.song.prompt): Read then changes
        that summary where the description's words differ, and Your songs again puts that text back. Void: no
        references (the description stands alone). */
    void setReferences (const juce::var& summary, const juce::String& generated);
    bool hasReferences() const noexcept { return references.isObject(); }
    juce::String getGenerated() const { return generated; }

    bool isBusy() const noexcept { return busy; }
    const juce::var& getSummary() const noexcept { return summary; }
    juce::String getUnderstoodText() const { return understood.getText(); }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getDetailsText() const { return details.getText(); }
    /** The AI's tagged version of untagged lyrics, waiting for Use these tags or Keep mine; empty when none. */
    juce::String getProposal() const { return proposal; }
    /** Use these tags: the proposal replaces the lyrics (undoable in the box) and the description is read again. */
    void useProposal();
    /** Keep mine: the proposal goes, the lyrics stay as they are. */
    void keepMine();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void updateButtons();
    void setProposal (const juce::String& text);
    void updateShare();
    void shareMoved();

    std::function<ai::AiService*()> getService;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    bool busy = false;
    juce::String job;
    juce::var summary, references, userShare;
    juce::String generated, proposal;

    juce::Label intro;
    juce::TextEditor textBox, lyricsBox;
    juce::Label lyricsLabel;
    juce::ToggleButton noVocalsButton { "This song has no vocals" };
    juce::ToggleButton aiButton { "Let the AI read it (uses your AI connection)" };
    juce::TextButton readButton { "Read" }, againButton { "Your songs again" };
    juce::Label shareLabel;
    juce::Slider shareSlider { juce::Slider::TwoValueHorizontal, juce::Slider::NoTextBox };
    juce::TextButton genreShareButton { "Genre's range" };
    MessageLabel understood, status;
    juce::Label proposalLabel;
    juce::TextEditor proposalBox;
    juce::TextButton useTagsButton { "Use these tags" }, keepMineButton { "Keep mine" };
    juce::TextEditor details;
};

} // namespace spinus::ui
