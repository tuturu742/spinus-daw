#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/drums/DrumMachineEditor.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/drums/StarterPack.h"
#include "engine/EngineHost.h"
#include "project/AudioImport.h"
#include "ui/Browser.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::dsp;

namespace
{
struct Temp
{
    Temp() { root.createDirectory(); }
    ~Temp()
    {
        root.setReadOnly (false, true);
        root.deleteRecursively();
    }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-starter-pack-" + juce::Uuid().toString());
};

/** The pack in the source tree (samples/Spinus Starter Pack): what every package installs. */
juce::File sourcePack()
{
    return juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("samples").getChildFile (starter::packName);
}

void writeFlac (const juce::File& file, double seconds = 0.2, double rate = 44100.0)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i,
                         0.5f * (float)std::sin (2.0 * M_PI * 200.0 * i / rate) *
                             (1.0f - (float)i / (float)audio.getNumSamples()));
    file.getParentDirectory().createDirectory();
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = flac.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

/** A small pack: one kit, 16 pads of its own sounds (pads 4 and 5 a choke group, pad 6 tuned and quieter). */
void makeFakePack (const juce::File& pack)
{
    juce::Array<juce::var> pads;
    for (int i = 0; i < 16; ++i)
    {
        const auto file = "Drums/Sound " + juce::String (i + 1) + ".flac";
        writeFlac (pack.getChildFile (file));
        auto* pad = new juce::DynamicObject();
        pad->setProperty ("name", "Pad " + juce::String (i + 1));
        pad->setProperty ("file", file);
        pad->setProperty ("choke", i == 3 || i == 4 ? 1 : 0);
        if (i == 5)
        {
            pad->setProperty ("tune", 7);
            pad->setProperty ("volume", -6.0);
        }
        pads.add (juce::var (pad));
    }
    auto* kit = new juce::DynamicObject();
    kit->setProperty ("name", "Test Kit");
    kit->setProperty ("pads", pads);
    auto* root = new juce::DynamicObject();
    root->setProperty ("kits", juce::Array<juce::var> { juce::var (kit) });
    pack.getChildFile (starter::kitsFileName).replaceWithText (juce::JSON::toString (juce::var (root)));
}

float parameterValue (DrumMachineProcessor& drums, const char* setting, int pad)
{
    return drums.getParameterState().getRawParameterValue (DrumMachineProcessor::parameterId (setting, pad))->load();
}
} // namespace

TEST_CASE ("The starter pack is found where each package installs it", "[starter-pack]")
{
    Temp t;
    // Arch, AppImage and Flatpak: <prefix>/share/spinus/samples, Spinus in <prefix>/lib/spinus.
    const auto prefix = t.root.getChildFile ("usr");
    const auto shared = prefix.getChildFile ("share/spinus/samples").getChildFile (starter::packName);
    makeFakePack (shared);
    prefix.getChildFile ("lib/spinus").createDirectory();
    CHECK (starter::findPack (prefix.getChildFile ("lib/spinus")) == shared);
    CHECK (starter::findPack (prefix.getChildFile ("bin")) == shared); // /usr/bin/spinus, the link
    // The Drum Machine plugin in another program: its library inside lib/vst3/<bundle>/Contents/<arch>.
    CHECK (starter::findPack (prefix.getChildFile ("lib/vst3/Spinus Drum Machine.vst3/Contents/x86_64-linux")) ==
           shared);

    // Windows: beside spinus.exe.
    const auto windows = t.root.getChildFile ("Spinus DAW");
    makeFakePack (windows.getChildFile ("samples").getChildFile (starter::packName));
    CHECK (starter::findPack (windows) == windows.getChildFile ("samples").getChildFile (starter::packName));

    // A folder with sounds but no kits.json is not the pack: elsewhere, a development build uses the source tree's.
    const auto other = t.root.getChildFile ("other");
    writeFlac (other.getChildFile ("samples").getChildFile (starter::packName).getChildFile ("Kick.flac"));
    CHECK (starter::findPack (other) == sourcePack());
}

TEST_CASE ("The shipped starter pack: every sound reads, every factory kit has its 16 pads", "[starter-pack]")
{
    const auto pack = sourcePack();
    REQUIRE (pack.getChildFile (starter::kitsFileName).existsAsFile());
    const auto check = starter::checkPack (pack);
    INFO (check.problems.joinIntoString ("\n"));
    CHECK (check.problems.isEmpty());
    CHECK (check.sounds >= 50);
    CHECK (check.kits >= 4);
    CHECK (pack.getChildFile ("LICENSE.txt").loadFileAsString().contains ("CC0 1.0 Universal"));
    CHECK (pack.getChildFile ("README.md").loadFileAsString().contains ("signaturesounds.org"));

    // Each kit loads into a Drum Machine with every pad sounding, hats in a choke group, notes from C5.
    for (const auto& kit : starter::readKits (pack))
    {
        DYNAMIC_SECTION (kit.name)
        {
            DrumMachineProcessor drums;
            const auto result = drums.loadFactoryKit (kit, pack);
            INFO (result.getErrorMessage());
            REQUIRE (result.wasOk());
            for (int i = 0; i < DrumMachineProcessor::numPads; ++i)
            {
                const auto& pad = drums.getPads()[(size_t)i];
                CHECK (pad.audio != nullptr);
                CHECK (pad.name == kit.pads[(size_t)i].name);
                CHECK (pad.note == DrumMachineProcessor::firstNote + i);
                if (pad.audio != nullptr)
                {
                    CHECK (pad.audio->getNumSamples() > 100);
                    CHECK (pad.audio->getMagnitude (0, pad.audio->getNumSamples()) > 0.5f); // peak-normalised
                }
            }
            CHECK (drums.getPads()[3].chokeGroup == 1); // Closed Hat
            CHECK (drums.getPads()[4].chokeGroup == 1); // Open Hat
            CHECK (kit.pads[0].name.containsIgnoreCase ("kick"));
            CHECK (kit.pads[1].name.containsIgnoreCase ("snare"));
        }
    }
}

TEST_CASE ("A factory kit sets its pads, and a project keeps the sounds without the pack", "[starter-pack]")
{
    Temp t;
    const auto pack = t.root.getChildFile (starter::packName);
    makeFakePack (pack);
    const auto kits = starter::readKits (pack);
    REQUIRE (kits.size() == 1);

    DrumMachineProcessor drums;
    drums.setPadNote (2, 72);
    REQUIRE (drums.loadFactoryKit (kits[0], pack).wasOk());
    CHECK (drums.getPads()[0].name == "Pad 1");
    CHECK (drums.getPads()[2].note == DrumMachineProcessor::firstNote + 2); // the kit's layout, from C5
    CHECK (parameterValue (drums, "tune", 5) == Catch::Approx (7.0f));
    CHECK (parameterValue (drums, "volume", 5) == Catch::Approx (-6.0f).margin (0.01));
    CHECK (parameterValue (drums, "tune", 0) == Catch::Approx (0.0f));

    // The saved state (in the project and in .spinuskit files) holds the sounds: it opens without the pack.
    juce::MemoryBlock state;
    drums.getStateInformation (state);
    pack.deleteRecursively();
    DrumMachineProcessor opened;
    opened.setStateInformation (state.getData(), (int)state.getSize());
    for (int i = 0; i < DrumMachineProcessor::numPads; ++i)
    {
        REQUIRE (opened.getPads()[(size_t)i].audio != nullptr);
        CHECK (opened.getPads()[(size_t)i].audio->getNumSamples() == drums.getPads()[(size_t)i].audio->getNumSamples());
        CHECK (opened.getPads()[(size_t)i].name == drums.getPads()[(size_t)i].name);
    }
    CHECK (opened.getPads()[3].chokeGroup == 1);
    CHECK (parameterValue (opened, "tune", 5) == Catch::Approx (7.0f));
}

TEST_CASE ("The Drum Machine knows its kit's name and whether a pad changed, saved with it", "[starter-pack][drums]")
{
    Temp t;
    const auto pack = t.root.getChildFile (starter::packName);
    makeFakePack (pack);
    const auto kit = starter::readKits (pack)[0];

    DrumMachineProcessor drums;
    CHECK (drums.getKitDisplayName() == "Spinus Synth Kit"); // a new Drum Machine
    CHECK (! drums.isKitModified());

    REQUIRE (drums.loadFactoryKit (kit, pack).wasOk());
    CHECK (drums.getKitDisplayName() == "Test Kit");

    // Automation (a value without a gesture) is not a change of the kit; a control turned in a window is.
    auto* volume = drums.getParameterState().getParameter (DrumMachineProcessor::parameterId ("volume", 0));
    volume->setValueNotifyingHost (0.3f);
    CHECK (! drums.isKitModified());
    volume->beginChangeGesture();
    volume->setValueNotifyingHost (0.4f);
    volume->endChangeGesture();
    CHECK (drums.getKitDisplayName() == "Test Kit *");

    // Pads: the same note or choke group again is no change; a new one, a sample or a cleared pad is.
    for (const auto& change :
         std::vector<std::function<void()>> {
             [&] { drums.setPadNote (3, 72); }, [&] { drums.setPadChoke (6, 2); }, [&] { drums.clearPad (2); },
             [&] { drums.setPadAudio (1, juce::AudioBuffer<float> (1, 100), 48000.0, "Mine"); },
             [&] { REQUIRE (drums.loadPadFile (0, pack.getChildFile ("Drums/Sound 9.flac")).wasOk()); } })
    {
        REQUIRE (drums.loadFactoryKit (kit, pack).wasOk());
        drums.setPadNote (5, DrumMachineProcessor::firstNote + 5);
        drums.setPadChoke (3, 1);
        CHECK (! drums.isKitModified());
        change();
        CHECK (drums.getKitDisplayName() == "Test Kit *");
    }

    // Kept in the saved state (the project, the plugin's state in another program).
    juce::MemoryBlock state;
    drums.getStateInformation (state);
    DrumMachineProcessor opened;
    opened.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (opened.getKitName() == "Test Kit");
    CHECK (opened.getKitDisplayName() == "Test Kit *");
    REQUIRE (drums.loadFactoryKit (kit, pack).wasOk());
    drums.getStateInformation (state);
    opened.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (opened.getKitDisplayName() == "Test Kit");

    // A .spinuskit file names the kit, saved or loaded; the Synth Kit and clearing name it too.
    drums.setPadChoke (0, 3);
    const auto file = t.root.getChildFile ("Night Drums.spinuskit");
    REQUIRE (drums.saveKit (file).wasOk());
    CHECK (drums.getKitDisplayName() == "Night Drums");
    drums.loadSynthKit();
    CHECK (drums.getKitDisplayName() == "Spinus Synth Kit");
    REQUIRE (drums.loadKit (file).wasOk());
    CHECK (drums.getKitDisplayName() == "Night Drums");
    CHECK (drums.getPads()[0].chokeGroup == 3);
    drums.clearAllPads();
    CHECK (drums.getKitDisplayName() == "New kit");
    for (const auto& pad : drums.getPads())
        CHECK (pad.audio == nullptr);

    // A state saved before kits had names says "Kit", as the button did.
    juce::ValueTree old ("SpinusDrumMachine");
    juce::MemoryBlock oldState;
    {
        juce::MemoryOutputStream out (oldState, false);
        old.writeToStream (out);
    }
    opened.setStateInformation (oldState.getData(), (int)oldState.getSize());
    CHECK (opened.getKitDisplayName() == "Kit");

    // The window's Kit button shows it.
    REQUIRE (drums.loadFactoryKit (kit, pack).wasOk());
    drums.setPadChoke (7, 4);
    std::unique_ptr<juce::AudioProcessorEditor> editor (drums.createEditorAndMakeActive());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    bool shown = false;
    for (auto* child : editor->getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            shown = shown || button->getButtonText() == "Test Kit *";
    CHECK (shown);
    drums.editorBeingDeleted (editor.get());
}

TEST_CASE ("A factory kit with sounds missing loads the others and says which", "[starter-pack]")
{
    Temp t;
    const auto pack = t.root.getChildFile (starter::packName);
    makeFakePack (pack);
    pack.getChildFile ("Drums/Sound 2.flac").deleteFile();
    pack.getChildFile ("Drums/Sound 9.flac").replaceWithText ("not audio");

    DrumMachineProcessor drums;
    const auto result = drums.loadFactoryKit (starter::readKits (pack)[0], pack);
    CHECK (result.failed());
    CHECK (result.getErrorMessage().contains ("Pad 2"));
    CHECK (result.getErrorMessage().contains ("Pad 9"));
    CHECK (drums.getPads()[1].audio == nullptr);
    CHECK (drums.getPads()[8].audio == nullptr);
    CHECK (drums.getPads()[0].audio != nullptr);

    const auto check = starter::checkPack (pack);
    CHECK (check.problems.size() == 2); // Sound 9 does not read; Sound 2 is missing
    CHECK (check.sounds == 15);

    // A path out of the pack is not followed.
    starter::KitPad outside { "Out", "../elsewhere.flac" };
    CHECK (starter::padFile (pack, outside) == juce::File());
}

TEST_CASE ("A starter pack sound on the Playlist is copied into the project", "[starter-pack]")
{
    Temp t;
    const auto pack = sourcePack();
    const auto loop = pack.getChildFile ("Loops/Drums/Trap Beat 115 BPM.flac");
    REQUIRE (loop.existsAsFile());
    const auto project = t.root.getChildFile ("My Song");
    project.createDirectory();

    // A saved project: into its audio/ folder, kept by a path inside the project.
    juce::String path;
    REQUIRE (project::importAudio (loop, project, path).wasOk());
    CHECK (! juce::File::isAbsolutePath (path));
    CHECK (project.getChildFile ("audio").getChildFile (path).existsAsFile());
    CHECK (project.getChildFile ("audio").getChildFile (path).getSize() == loop.getSize());
    CHECK (loop.existsAsFile()); // read only, never moved
}

TEST_CASE ("Starter pack screenshots for the manual", "[.screenshot][starter-pack]")
{
    const auto images = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images");
    auto write = [&] (const juce::Image& image, const juce::String& name)
    {
        const auto file = images.getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream out (file);
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    };

    // The Drum Machine with the first factory kit (Shrike).
    const auto pack = sourcePack();
    const auto kits = starter::readKits (pack);
    REQUIRE (! kits.empty());
    DrumMachineProcessor drums;
    REQUIRE (drums.loadFactoryKit (kits[0], pack).wasOk());
    std::unique_ptr<juce::AudioProcessorEditor> editor (drums.createEditorAndMakeActive());
    auto* window = dynamic_cast<DrumMachineEditor*> (editor.get());
    REQUIRE (window != nullptr);
    window->selectPad (12);
    editor->setSize (800, 520);
    write (editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f), "drum-machine-factory-kit.png");
    drums.editorBeingDeleted (editor.get());

    // The Browser's Samples with the pack open.
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    ui::SpinusLookAndFeel lf { ui::Theme::dark() };
    ui::Browser browser (session);
    browser.setLookAndFeel (&lf);
    browser.setSize (300, 560);
    browser.setStarterPack (pack);
    browser.toggleOpen ("projects");
    browser.toggleFolder (pack);
    browser.toggleFolder (pack.getChildFile ("Drums"));
    browser.toggleFolder (pack.getChildFile ("Drums/Kicks"));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    write (browser.createComponentSnapshot (browser.getLocalBounds(), true, 1.0f), "starter-pack.png");
    browser.setLookAndFeel (nullptr);
}
