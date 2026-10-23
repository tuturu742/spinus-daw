#include <catch2/catch_test_macros.hpp>

#include "plugins/clap/ClapPluginFormat.h"

using namespace spinus::plugins;

namespace
{
juce::File bundled (const char* file)
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR).getChildFile (file);
#else
    juce::ignoreUnused (file);
    return {};
#endif
}

std::unique_ptr<juce::AudioPluginInstance> load (ClapPluginFormat& format, const juce::File& file,
                                                 const juce::String& name)
{
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file.getFullPathName());
    for (auto* d : found)
        if (d->name == name)
        {
            juce::String error;
            return format.createInstanceFromDescription (*d, 48000.0, 512, error);
        }
    return nullptr;
}
} // namespace

TEST_CASE ("CLAP plugin windows embed in a Spinus window", "[plugins][clap-gui]")
{
    if (! bundled ("Surge XT.clap").existsAsFile())
        SKIP ("bundled plugins not built");

    ClapPluginFormat format;
    for (auto [file, name] :
         { std::pair { "Surge XT.clap", "Surge XT" }, std::pair { "DragonflyRoomReverb.clap", "Dragonfly Room Reverb" },
           std::pair { "lsp-plugins.clap", "Compressor Stereo" } })
    {
        INFO (name);
        auto plugin = load (format, bundled (file), name);
        REQUIRE (plugin != nullptr);
        REQUIRE (plugin->hasEditor());

        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
        REQUIRE (editor != nullptr);
        CHECK (editor->getWidth() > 100);
        CHECK (editor->getHeight() > 100);
        CHECK (! ClapPluginFormat::isEditorAttached (editor.get())); // not on screen yet

        {
            juce::DocumentWindow window (name, juce::Colours::black, 0);
            window.setUsingNativeTitleBar (true);
            window.setContentNonOwned (editor.get(), true);
            window.setVisible (true);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

            CHECK (ClapPluginFormat::isEditorAttached (editor.get()));
            window.clearContentComponent();
        }

        editor = nullptr;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
}

TEST_CASE ("Show a CLAP plugin window for a few seconds", "[.manual]")
{
    // For looking at a plugin window by eye or with a screenshot tool:
    // SPINUS_CLAP_FILE=lsp-plugins.clap SPINUS_CLAP_NAME="Compressor Stereo" spinus_tests "[.manual]"
    const auto file = juce::SystemStats::getEnvironmentVariable ("SPINUS_CLAP_FILE", "Surge XT.clap");
    const auto name = juce::SystemStats::getEnvironmentVariable ("SPINUS_CLAP_NAME", "Surge XT");
    ClapPluginFormat format;
    auto plugin = load (format, bundled (file.toRawUTF8()), name);
    REQUIRE (plugin != nullptr);
    std::unique_ptr<juce::AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
    REQUIRE (editor != nullptr);

    juce::DocumentWindow window ("Spinus plugin window test", juce::Colours::black, 0);
    window.setUsingNativeTitleBar (true);
    window.setContentNonOwned (editor.get(), true);
    window.setVisible (true);
    std::cout << "WINDOW " << (juce::pointer_sized_uint)window.getWindowHandle() << '\n' << std::flush;
    juce::MessageManager::getInstance()->runDispatchLoopUntil (6000);
    window.clearContentComponent();
}
