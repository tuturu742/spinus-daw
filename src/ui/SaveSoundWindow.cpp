#include "SaveSoundWindow.h"

#include "WindowStacking.h"

#include "engine/SamplePreview.h"
#include "session/HiddenInstrument.h"
#include "session/InstrumentPresets.h"

namespace spinus::ui
{

namespace
{
std::vector<SaveSoundWindow*>& openWindows()
{
    static std::vector<SaveSoundWindow*> windows;
    return windows;
}

model::Device deviceOf (const SoundSource& source)
{
    model::Device device;
    if (source.stockType.isNotEmpty())
    {
        device.kind = model::Device::Kind::stock;
        device.stockType = source.stockType;
    }
    else
    {
        device.kind = model::Device::Kind::plugin;
        device.plugin.format = source.format;
        device.plugin.name = source.pluginName.isNotEmpty() ? source.pluginName : source.name;
        device.plugin.fileOrIdentifier = source.file;
        device.plugin.uniqueId = source.uniqueId;
    }
    return device;
}
} // namespace

/** The plugin's editor, with Play, the name, and Save and Cancel under it. */
class SaveSoundWindow::Content final : public juce::Component, private juce::Timer
{
public:
    Content (SaveSoundWindow& w, juce::AudioPluginInstance& p, const juce::String& name) : window (w), instance (p)
    {
        editor.reset (instance.createEditorAndMakeActive());
        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (instance);
        addAndMakeVisible (*editor);
        hint.setText (defaultHint, juce::dontSendNotification);
        hint.setMinimumHorizontalScale (1.0f);
        nameEditor.setText (name, false);
        nameEditor.setTooltip ("The name your saved sound is listed under (Your saved sounds)");
        playButton.setTooltip ("Hear the part's phrase with the sound as it is set up now");
        playButton.onClick = [this] { played (window.play()); };
        notesButton.setTooltip ("Learn this instrument: which keys play and which switch what it plays (Play, Listen "
                                "and the song keep to the keys that play)");
        notesButton.onClick = [this]
        {
            if (window.notes.learn)
                window.notes.learn (
                    [safe = juce::Component::SafePointer<Content> (this)] (std::optional<PlayableKeys> keys)
                    {
                        if (safe != nullptr)
                            safe->window.setNotes (keys);
                    });
        };
        notesButton.setVisible (window.notes.learn != nullptr);
        saveButton.onClick = [this] { window.save (nameEditor.getText()); };
        saveButton.setTooltip ("Keep this sound under its name and close the window");
        keepButton.setTooltip ("Keep this sound under its name and leave the window open: set up the next sound "
                               "(another preset or kit), name it and save it too");
        keepButton.onClick = [this] { kept (nameEditor.getText()); };
        cancelButton.setTooltip ("Close without saving the sound as it is now");
        cancelButton.onClick = [this] { window.finish(); };
        for (auto* c : std::initializer_list<juce::Component*> { &hint, &playButton, &nameLabel, &nameEditor,
                                                                 &keepButton, &saveButton, &cancelButton })
            addAndMakeVisible (c);
        addChildComponent (notesButton);
        notesButton.setVisible (window.notes.learn != nullptr);
        updateNotesText();
        nameLabel.setText ("Name:", juce::dontSendNotification);
        setSize (juce::jmax (minWidth, editor->getWidth()), editor->getHeight() + bar);
    }

    ~Content() override
    {
        // JUCE leaves this to the host: until told, the plugin thinks its editor is still open.
        if (editor != nullptr)
            instance.editorBeingDeleted (editor.get());
        editor.reset();
    }

    /** Save and keep open: kept, said, and the name ready for the next one. */
    void kept (const juce::String& name)
    {
        const auto ok = window.keep (name);
        names.add (name.trim());
        hint.setText (ok ? "Kept " + names.joinIntoString (", ") +
                               ". Set up the next sound, name it and save it too; Close keeps what you saved."
                         : "The instrument gave nothing to keep for " + name.trim() + ".",
                      juce::dontSendNotification);
        cancelButton.setButtonText ("Close");
        cancelButton.setTooltip ("Close the window, keeping the sounds saved so far (not the one set up now)");
        nameEditor.setText (nextName (name.trim()), false);
        nameEditor.selectAll();
        nameEditor.grabKeyboardFocus();
    }

    /** "Clean 2" after "Clean", "Clean 3" after "Clean 2". */
    static juce::String nextName (const juce::String& name)
    {
        const auto number = name.fromLastOccurrenceOf (" ", false, false);
        if (number.containsOnly ("0123456789") && number.isNotEmpty())
            return name.upToLastOccurrenceOf (" ", false, false) + " " + juce::String (number.getIntValue() + 1);
        return name + " 2";
    }

    void updateNotesText()
    {
        notesButton.setButtonText (
            "Keys: " +
            (window.notes.playable.has_value()
                 ? keyName (window.notes.playable->lowest()) + " to " + keyName (window.notes.playable->highest())
                 : juce::String ("any")) +
            "...");
    }

    /** Says it plays, then, once the phrase has ended, whether anything was heard. */
    void played (bool playing)
    {
        if (! playing)
        {
            hint.setText ("There is nothing to play for this part.", juce::dontSendNotification);
            return;
        }
        hint.setText ("Playing...", juce::dontSendNotification);
        window.live->takePeak();
        double end = 0.0;
        for (const auto& n : window.playedPhrase())
            end = juce::jmax (end, n.start + n.length);
        startTimer (juce::roundToInt ((end + 0.5) * 1000.0));
    }

    void timerCallback() override
    {
        stopTimer();
        hint.setText (window.live->takePeak() > 1.0e-4f
                          ? juce::String (defaultHint)
                          : "It made no sound: choose a kit or a patch in its window (a big one may still be "
                            "loading: Play again in a moment).",
                      juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (bar).reduced (8, 6);
        hint.setBounds (bottom.removeFromTop (22));
        bottom.removeFromTop (4);
        cancelButton.setBounds (bottom.removeFromRight (80));
        bottom.removeFromRight (6);
        saveButton.setBounds (bottom.removeFromRight (70));
        bottom.removeFromRight (6);
        keepButton.setBounds (bottom.removeFromRight (150));
        bottom.removeFromRight (10);
        playButton.setBounds (bottom.removeFromLeft (70));
        bottom.removeFromLeft (6);
        if (notesButton.isVisible())
        {
            notesButton.setBounds (bottom.removeFromLeft (150));
            bottom.removeFromLeft (10);
        }
        nameLabel.setBounds (bottom.removeFromLeft (50));
        nameEditor.setBounds (bottom);
        if (editor != nullptr)
            editor->setTopLeftPosition (r.getX() + juce::jmax (0, (r.getWidth() - editor->getWidth()) / 2), r.getY());
    }

    void childBoundsChanged (juce::Component* child) override
    {
        // The plugin resized its editor: follow it.
        if (child == editor.get())
            setSize (juce::jmax (minWidth, editor->getWidth()), editor->getHeight() + bar);
    }

private:
    static constexpr int bar = 64, minWidth = 760;
    static constexpr const char* defaultHint =
        "Set the sound up (a kit, a patch), Play, name it and Save. Several? Save and keep open for each.";
    SaveSoundWindow& window;
    juce::AudioPluginInstance& instance;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::Label hint, nameLabel;
    juce::TextEditor nameEditor;
    juce::TextButton playButton { "Play" }, saveButton { "Save" }, keepButton { "Save and keep open" },
        cancelButton { "Cancel" }, notesButton;
    juce::StringArray names; // kept so far

    friend class SaveSoundWindow;
};

bool SaveSoundWindow::open (Session& session, const parts::Choice& start, const juce::String& name,
                            std::vector<OfflineInstrument::Note> phrase, Notes notes, juce::Component* near,
                            OnSave onSave, juce::String& problem)
{
    if (start.source.key.isEmpty() || (start.source.stockType.isEmpty() && start.source.file.isEmpty()))
    {
        problem = "Choose an instrument for this part first (Other sound...), then Save this sound.";
        return false;
    }
    auto hidden = std::make_unique<HiddenInstrument> (session, deviceOf (start.source));
    if (hidden->getProblem().isNotEmpty() || hidden->getInstance() == nullptr)
    {
        problem =
            hidden->getProblem().isNotEmpty() ? hidden->getProblem() : start.source.name + " could not be loaded.";
        return false;
    }
    // Its sound as chosen: a saved sound, a preset, or as it opens.
    if (start.state != juce::File())
    {
        InstrumentPreset saved;
        saved.kind = InstrumentPreset::Kind::state;
        saved.id = start.preset;
        if (start.state.loadFileAsData (saved.state) && ! saved.state.isEmpty())
            hidden->loadPreset (saved);
    }
    else if (start.preset.isNotEmpty())
    {
        const auto presets = InstrumentPresets::find (*hidden->getInstance(), deviceOf (start.source));
        if (const auto found = std::find_if (presets.begin(), presets.end(),
                                             [&] (const InstrumentPreset& p) { return p.id == start.preset; });
            found != presets.end())
            hidden->loadPreset (*found);
    }
    auto* window = new SaveSoundWindow (session, std::move (hidden), "Save this sound: " + start.source.name, name,
                                        std::move (phrase), std::move (notes), std::move (onSave),
                                        near != nullptr ? near->getTopLevelComponent() : nullptr);
    if (near != nullptr)
        window->setCentrePosition (near->getScreenBounds().getCentre());
    return true;
}

SaveSoundWindow::SaveSoundWindow (Session& s, std::unique_ptr<HiddenInstrument> h, const juce::String& title,
                                  const juce::String& name, std::vector<OfflineInstrument::Note> played, Notes range,
                                  OnSave save, juce::Component* owner)
    : juce::DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::closeButton, false), session (s),
      hidden (std::move (h)), phrase (std::move (played)), notes (std::move (range)), onSave (std::move (save))
{
    // Live from now on: what the user does in its window is heard (one window at a time: the newest).
    live = std::make_unique<engine::LiveInstrument> (*hidden->getInstance());
    live->prepare (session.getSamplePreview().getSampleRate());
    session.getSamplePreview().setLive (live.get());
    setUsingNativeTitleBar (true);
    content = std::make_unique<Content> (*this, *hidden->getInstance(), name);
    setContentNonOwned (content.get(), true);
    setResizable (false, false);
    addToDesktop();
    if (owner != nullptr && owner->isOnDesktop())
        keepAboveOwner (*this, *owner); // above Spinus's window, like a plugin's own window
    setVisible (true);
    openWindows().push_back (this);
}

SaveSoundWindow::~SaveSoundWindow()
{
    std::erase (openWindows(), this);
    // Off the sound card before the plugin goes; the window opened before it, if still open, plays again.
    if (auto& preview = session.getSamplePreview(); preview.getLive() == live.get())
    {
        auto* before = openWindows().empty() ? nullptr : openWindows().back();
        preview.setLive (before != nullptr ? before->live.get() : nullptr);
    }
    live = nullptr;
    clearContentComponent();
    content = nullptr; // the editor goes before the plugin
    hidden = nullptr;
}

int SaveSoundWindow::getNumOpen() { return (int)openWindows().size(); }

void SaveSoundWindow::closeAll()
{
    while (! openWindows().empty())
        delete openWindows().back(); // its destructor takes it off the list
}

bool SaveSoundWindow::saveLastOpened (const juce::String& name)
{
    if (openWindows().empty())
        return false;
    openWindows().back()->save (name);
    return true;
}

bool SaveSoundWindow::keepLastOpened (const juce::String& name)
{
    if (openWindows().empty())
        return false;
    openWindows().back()->content->kept (name);
    return true;
}

void SaveSoundWindow::closeLastOpened()
{
    if (! openWindows().empty())
        openWindows().back()->finish();
}

bool SaveSoundWindow::playLastOpened() { return ! openWindows().empty() && openWindows().back()->play(); }

std::vector<int> SaveSoundWindow::playedNotesLastOpened()
{
    std::vector<int> played;
    if (! openWindows().empty())
        for (const auto& n : openWindows().back()->playedPhrase())
            played.push_back (n.note);
    return played;
}

void SaveSoundWindow::setNotesLastOpened (std::optional<PlayableKeys> keys)
{
    if (! openWindows().empty())
        openWindows().back()->setNotes (keys);
}

float SaveSoundWindow::mixLastOpened (double seconds)
{
    if (openWindows().empty())
        return 0.0f;
    auto& instrument = *openWindows().back()->live;
    juce::AudioBuffer<float> block (2, engine::LiveInstrument::maxBlockSize);
    float loudest = 0.0f;
    for (int done = 0; done < (int)(seconds * instrument.getSampleRate()); done += block.getNumSamples())
    {
        block.clear();
        instrument.mix (block.getArrayOfWritePointers(), 2, block.getNumSamples());
        loudest = juce::jmax (loudest, block.getMagnitude (0, block.getNumSamples()));
    }
    return loudest;
}

bool SaveSoundWindow::keep (const juce::String& name)
{
    juce::MemoryBlock state;
    if (auto* instance = hidden != nullptr ? hidden->getInstance() : nullptr)
        instance->getStateInformation (state);
    kept.push_back ({ name.trim(), state });
    return ! state.isEmpty();
}

void SaveSoundWindow::save (const juce::String& name)
{
    keep (name);
    finish();
}

void SaveSoundWindow::finish()
{
    close (
        [then = std::move (onSave), saved = std::move (kept)]() mutable
        {
            if (then && ! saved.empty())
                then (std::move (saved));
        });
}

std::vector<OfflineInstrument::Note> SaveSoundWindow::playedPhrase() const
{
    auto played = phrase;
    if (notes.playable.has_value())
        for (auto& n : played)
            n.note = notes.playable->fit (n.note);
    return played;
}

void SaveSoundWindow::setNotes (std::optional<PlayableKeys> keys)
{
    notes.playable = keys;
    if (content != nullptr)
    {
        content->updateNotesText();
        content->resized();
    }
}

bool SaveSoundWindow::play()
{
    if (live == nullptr || phrase.empty())
        return false;
    std::vector<engine::LiveInstrument::Note> played;
    played.reserve (phrase.size());
    for (const auto& n : playedPhrase())
        played.push_back ({ n.note, n.start, n.length, n.velocity });
    return live->play (played);
}

void SaveSoundWindow::close (std::function<void()> afterwards)
{
    setVisible (false);
    // Deleted after this call returns (it may be the window's own button calling); then what comes after.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<SaveSoundWindow> (this), afterwards = std::move (afterwards)]
        {
            delete safe.getComponent();
            if (afterwards)
                afterwards();
        });
}

void SaveSoundWindow::closeButtonPressed() { finish(); }

} // namespace spinus::ui
