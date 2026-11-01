#include "SaveSoundWindow.h"

#include "WindowStacking.h"

#include "dsp/sampler/SampleStorage.h"
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
class SaveSoundWindow::Content final : public juce::Component
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
        playButton.onClick = [this] { showPlayed (window.play()); };
        saveButton.onClick = [this] { window.save (nameEditor.getText()); };
        cancelButton.onClick = [this] { window.close(); };
        for (auto* c : std::initializer_list<juce::Component*> { &hint, &playButton, &nameLabel, &nameEditor,
                                                                 &saveButton, &cancelButton })
            addAndMakeVisible (c);
        nameLabel.setText ("Name:", juce::dontSendNotification);
        setSize (juce::jmax (520, editor->getWidth()), editor->getHeight() + bar);
    }

    ~Content() override
    {
        // JUCE leaves this to the host: until told, the plugin thinks its editor is still open.
        if (editor != nullptr)
            instance.editorBeingDeleted (editor.get());
        editor.reset();
    }

    void showPlayed (const juce::File& file)
    {
        hint.setText (file != juce::File()
                          ? juce::String ("Playing. ") + defaultHint
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
        saveButton.setBounds (bottom.removeFromRight (80));
        bottom.removeFromRight (10);
        playButton.setBounds (bottom.removeFromLeft (70));
        bottom.removeFromLeft (10);
        nameLabel.setBounds (bottom.removeFromLeft (50));
        nameEditor.setBounds (bottom);
        if (editor != nullptr)
            editor->setTopLeftPosition (r.getX() + juce::jmax (0, (r.getWidth() - editor->getWidth()) / 2), r.getY());
    }

    void childBoundsChanged (juce::Component* child) override
    {
        // The plugin resized its editor: follow it.
        if (child == editor.get())
            setSize (juce::jmax (520, editor->getWidth()), editor->getHeight() + bar);
    }

private:
    static constexpr int bar = 64;
    static constexpr const char* defaultHint =
        "Set the sound up here (choose a kit or a patch), Play to hear it, name it, then Save: the Track creator "
        "can use it from then on.";
    SaveSoundWindow& window;
    juce::AudioPluginInstance& instance;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::Label hint, nameLabel;
    juce::TextEditor nameEditor;
    juce::TextButton playButton { "Play" }, saveButton { "Save" }, cancelButton { "Cancel" };
};

bool SaveSoundWindow::open (Session& session, const parts::Choice& start, const juce::String& name,
                            std::vector<OfflineInstrument::Note> phrase, juce::Component* near, OnSave onSave,
                            juce::String& problem)
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
                                        std::move (phrase), std::move (onSave),
                                        near != nullptr ? near->getTopLevelComponent() : nullptr);
    if (near != nullptr)
        window->setCentrePosition (near->getScreenBounds().getCentre());
    return true;
}

SaveSoundWindow::SaveSoundWindow (Session& s, std::unique_ptr<HiddenInstrument> h, const juce::String& title,
                                  const juce::String& name, std::vector<OfflineInstrument::Note> notes, OnSave save,
                                  juce::Component* owner)
    : juce::DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::closeButton, false), session (s),
      hidden (std::move (h)), phrase (std::move (notes)), onSave (std::move (save))
{
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
    if (playedOnce)
        session.getSamplePreview().stop(); // its file goes with the window
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

juce::File SaveSoundWindow::playLastOpened()
{
    return openWindows().empty() ? juce::File() : openWindows().back()->play();
}

void SaveSoundWindow::save (const juce::String& name)
{
    juce::MemoryBlock state;
    if (auto* instance = hidden != nullptr ? hidden->getInstance() : nullptr)
        instance->getStateInformation (state);
    close (
        [then = std::move (onSave), name = name.trim(), state]
        {
            if (then)
                then (name, state);
        });
}

juce::File SaveSoundWindow::play()
{
    if (hidden == nullptr || hidden->getInstance() == nullptr || phrase.empty())
        return {};
    // Offline, on the message thread (as PresetPreviews): a few seconds of sound take a moment to make.
    const auto audio = hidden->render (phrase, 0.5);
    if (audio.getMagnitude (0, audio.getNumSamples()) <= 1.0e-4f)
        return {};
    const auto file = played.getFile();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                      .withSampleRate (HiddenInstrument::sampleRate)
                                                                      .withNumChannels (1)
                                                                      .withBitsPerSample (16));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
        return {};
    writer.reset();
    playedOnce = session.getSamplePreview().play (file, dsp::getAudioFormats()) || playedOnce;
    return file;
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

void SaveSoundWindow::closeButtonPressed() { close(); }

} // namespace spinus::ui
