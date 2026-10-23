#include "PluginWindow.h"

#include "WindowStacking.h"
#include "engine/EngineHost.h"
#include "session/BuiltInPresets.h"
#include "session/SlicesToPattern.h"

namespace spinus::ui
{

namespace te = tracktion;

namespace
{
/** The plugin's editor, or a generic one with a slider per parameter when it has none. */
class EditorContent final : public te::Plugin::EditorComponent
{
public:
    explicit EditorContent (te::ExternalPlugin& p) : plugin (p)
    {
        if (auto* instance = plugin.getAudioPluginInstance())
        {
            editor.reset (instance->createEditorAndMakeActive());
            if (editor == nullptr)
                editor = std::make_unique<juce::GenericAudioProcessorEditor> (*instance);
            addAndMakeVisible (*editor);
            setSize (juce::jmax (8, editor->getWidth()), juce::jmax (8, editor->getHeight()));
        }
    }

    ~EditorContent() override
    {
        // JUCE leaves this to the host: until told, the plugin thinks its editor is still open and
        // refuses to create another one (the next window would only show sliders).
        if (editor != nullptr)
            if (auto* instance = plugin.getAudioPluginInstance())
                instance->editorBeingDeleted (editor.get());
        editor.reset();
    }

    bool isValid() const noexcept { return editor != nullptr; }

    bool allowWindowResizing() override { return editor != nullptr && editor->isResizable(); }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override
    {
        return editor != nullptr ? editor->getConstrainer() : nullptr;
    }
    bool isEditorResizable() override { return allowWindowResizing(); }

    void resized() override
    {
        if (editor != nullptr && allowWindowResizing())
            editor->setBounds (getLocalBounds());
    }

    void childBoundsChanged (juce::Component* child) override
    {
        // The plugin resized its editor (e.g. a CLAP request_resize): follow it.
        if (child == editor.get())
            setSize (juce::jmax (8, editor->getWidth()), juce::jmax (8, editor->getHeight()));
    }

private:
    te::ExternalPlugin& plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};
} // namespace

//==============================================================================
PluginWindow::PluginWindow (te::Plugin& p, const juce::String& title, juce::Component* owner)
    : juce::DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::closeButton, false), plugin (p)
{
    setUsingNativeTitleBar (true);

    if (auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin))
    {
        if (auto content = std::make_unique<EditorContent> (*external); content->isValid())
            editor = std::move (content);
    }

    if (editor == nullptr)
        return;

    setContentNonOwned (editor.get(), true);
    setResizable (editor->allowWindowResizing(), false);
    if (editor->allowWindowResizing())
        setConstrainer (editor->getBoundsConstrainer());

    setTopLeftPosition (plugin.windowState->choosePositionForPluginWindow());
    addToDesktop();
    if (owner != nullptr)
        keepAboveOwner (*this, *owner);
    trackBounds = true;
}

PluginWindow::~PluginWindow()
{
    trackBounds = false;
    plugin.edit.flushPluginStateIfNeeded (plugin);
    clearContentComponent();
    editor.reset();
}

void PluginWindow::closeButtonPressed() { plugin.windowState->closeWindowExplicitly(); }

void PluginWindow::moved()
{
    if (trackBounds)
        plugin.windowState->lastWindowBounds = getBounds();
}

void PluginWindow::install (Session& session, juce::Component* owner)
{
    EngineHost::setPluginWindowFactory (session.getEdit().engine,
                                        [&session, owner = juce::Component::SafePointer<juce::Component> (owner)] (
                                            te::Plugin& plugin) -> std::unique_ptr<juce::Component>
                                        {
                                            auto device = session.getAdapter().findDevice (plugin);
                                            connectSlicesToPattern (session, plugin);
                                            connectBuiltInPresets (session, plugin);
                                            auto title = device ? session.getDeviceTitle (*device) : juce::String();
                                            // Every plugin, Surge XT too, shows its own editor as its makers designed
                                            // it.
                                            auto window = std::make_unique<PluginWindow> (
                                                plugin, title.isNotEmpty() ? title : plugin.getName(),
                                                owner.getComponent());
                                            if (! window->hasEditor())
                                                return {};
                                            return window;
                                        });
}

void PluginWindow::uninstall (Session& session) { EngineHost::setPluginWindowFactory (session.getEdit().engine, {}); }

} // namespace spinus::ui
