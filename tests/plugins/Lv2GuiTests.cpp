#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>

// LV2 plugin windows (UIs) that misbehave as real ones do must never take Spinus down: a UI that cannot open
// leaves the plugin without an editor (Spinus then shows its generic one), and one that opens works. The UIs
// are in tests/plugins/Lv2UiTestPlugin.c; JUCE's LV2 host is patched in cmake/SpinusJuce.cmake.

#if defined(SPINUS_TEST_LV2_UI_BUNDLE)

namespace
{
std::unique_ptr<juce::AudioPluginInstance> loadLv2 (juce::LV2PluginFormat& format, const juce::String& bundle,
                                                    const juce::String& uri)
{
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, bundle);
    for (auto* d : found)
        if (d->fileOrIdentifier == uri)
        {
            juce::String error;
            auto plugin = format.createInstanceFromDescription (*d, 48000.0, 512, error);
            INFO (error);
            return plugin;
        }
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> loadTest (juce::LV2PluginFormat& format, const char* kind)
{
    return loadLv2 (format, SPINUS_TEST_LV2_UI_BUNDLE, juce::String ("urn:spinus:test:lv2-ui#") + kind);
}

bool haveDisplay() { return juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty(); }

/** Opens the editor on screen for a moment, as Spinus's plugin window does, then closes it. */
void showBriefly (juce::AudioPluginInstance& plugin, juce::AudioProcessorEditor& editor)
{
    {
        juce::DocumentWindow window (plugin.getName(), juce::Colours::black, 0);
        window.setUsingNativeTitleBar (true);
        window.setContentNonOwned (&editor, true);
        window.setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        window.clearContentComponent();
    }
    plugin.editorBeingDeleted (&editor);
}
} // namespace

TEST_CASE ("An LV2 UI that needs a feature Spinus lacks is never opened", "[plugins][lv2-gui]")
{
    juce::LV2PluginFormat format;
    auto plugin = loadTest (format, "unsupported");
    REQUIRE (plugin != nullptr);
    CHECK (! plugin->hasEditor());
    std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
    CHECK (editor == nullptr); // Spinus's plugin window shows the generic editor
}

TEST_CASE ("An LV2 UI that fails to open leaves the plugin without an editor", "[plugins][lv2-gui]")
{
    if (! haveDisplay())
        SKIP ("no X display");

    juce::LV2PluginFormat format;
    auto plugin = loadTest (format, "fails");
    REQUIRE (plugin != nullptr);
    CHECK (plugin->hasEditor()); // known only on opening
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        CHECK (editor == nullptr); // Spinus's plugin window shows the generic editor
        CHECK (plugin->getActiveEditor() == nullptr);
        CHECK (! plugin->hasEditor()); // from then on
    }

    // The plugin still plays.
    plugin->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> audio (juce::jmax (1, plugin->getTotalNumOutputChannels()), 512);
    juce::MidiBuffer midi;
    plugin->processBlock (audio, midi);
    plugin->releaseResources();
}

TEST_CASE ("An LV2 UI without the extension data its .ttl lists opens (DrumGizmo's crash)", "[plugins][lv2-gui]")
{
    if (! haveDisplay())
        SKIP ("no X display");

    // Its .ttl lists ui:resize, but extension_data returns NULL for it: JUCE read through that pointer.
    juce::LV2PluginFormat format;
    auto plugin = loadTest (format, "null-resize");
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->hasEditor());
    for (int attempt = 0; attempt < 2; ++attempt) // closing and opening again works
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        REQUIRE (editor != nullptr);
        CHECK (editor->getWidth() == 320); // the UI's own window
        CHECK (editor->getHeight() == 200);
        showBriefly (*plugin, *editor);
    }
}

TEST_CASE ("An LV2 UI that needs instance-access gets its plugin", "[plugins][lv2-gui]")
{
    if (! haveDisplay())
        SKIP ("no X display");

    // The UI only opens when the instance-access data is its own plugin's LV2_Handle.
    juce::LV2PluginFormat format;
    auto plugin = loadTest (format, "instance-access");
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->hasEditor());
    std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
    REQUIRE (editor != nullptr);
    showBriefly (*plugin, *editor);
}

TEST_CASE ("DrumGizmo's window opens, when DrumGizmo is installed", "[plugins][lv2-gui]")
{
    const juce::File bundle ("/usr/lib/lv2/drumgizmo.lv2");
    if (! bundle.getChildFile ("manifest.ttl").existsAsFile())
        SKIP ("DrumGizmo (LV2) is not installed");
    if (! haveDisplay())
        SKIP ("no X display");
#if defined(__SANITIZE_ADDRESS__)
    // DrumGizmo 0.9.20 itself frees with delete what it allocated with new[] (in setSamplerate). To check
    // Spinus's side by hand: ASAN_OPTIONS=alloc_dealloc_mismatch=0 SPINUS_LV2_DRUMGIZMO_ASAN=1 spinus_tests ...
    if (juce::SystemStats::getEnvironmentVariable ("SPINUS_LV2_DRUMGIZMO_ASAN", {}).isEmpty())
        SKIP ("DrumGizmo's own code fails AddressSanitizer's checks");
#endif

    juce::LV2PluginFormat format;
    auto plugin = loadLv2 (format, "http://drumgizmo.org/lv2", "http://drumgizmo.org/lv2");
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->hasEditor());
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        REQUIRE (editor != nullptr);
        CHECK (editor->getWidth() > 100);
        CHECK (editor->getHeight() > 100);
        showBriefly (*plugin, *editor);
    }
}

TEST_CASE ("Every installed LV2 plugin window opens and closes", "[.manual]")
{
    // For checking this computer's LV2 plugins by hand: spinus_tests "Every installed LV2 plugin window*"
    // (SPINUS_LV2_SHOW_MS=3000 keeps each window up for a screenshot).
    const int showMs = juce::SystemStats::getEnvironmentVariable ("SPINUS_LV2_SHOW_MS", "200").getIntValue();
    juce::LV2PluginFormat format;
    juce::StringArray seen;
    const auto folders = format.getDefaultLocationsToSearch();
    for (int i = 0; i < folders.getNumPaths(); ++i)
        for (const auto& entry :
             juce::RangedDirectoryIterator (folders[i], false, "*.lv2", juce::File::findDirectories))
        {
            juce::OwnedArray<juce::PluginDescription> found;
            format.findAllTypesForFile (found, entry.getFile().getFullPathName());
            for (auto* d : found)
            {
                if (seen.contains (d->fileOrIdentifier))
                    continue;
                seen.add (d->fileOrIdentifier);
                juce::String error;
                auto plugin = format.createInstanceFromDescription (*d, 48000.0, 512, error);
                if (plugin == nullptr || ! plugin->hasEditor())
                    continue;
                std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
                std::cout << d->name << " (" << d->fileOrIdentifier << "): "
                          << (editor != nullptr
                                  ? juce::String (editor->getWidth()) + "x" + juce::String (editor->getHeight())
                                  : juce::String ("no window, generic editor"))
                          << "\n"
                          << std::flush;
                if (editor != nullptr)
                {
                    juce::DocumentWindow window (d->name, juce::Colours::black, 0);
                    window.setUsingNativeTitleBar (true);
                    window.setContentNonOwned (editor.get(), true);
                    window.setVisible (true);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (showMs);
                    window.clearContentComponent();
                    plugin->editorBeingDeleted (editor.get());
                }
            }
        }
}

#endif
