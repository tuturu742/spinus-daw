#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/Launcher.h"
#include "ui/Theme.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
juce::File writeTone (const juce::File& file, double seconds)
{
    constexpr double rate = 48000.0;
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.3f * (float)std::sin (2.0 * M_PI * 220.0 * i / rate));
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

struct Fixture
{
    Fixture()
    {
        root.createDirectory();
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        launcher = std::make_unique<Launcher> (*session);
        launcher->setSize (760, 330);
    }
    ~Fixture()
    {
        launcher = nullptr;
        session = nullptr;
        root.deleteRecursively();
    }

    juce::var run (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session->getHistory().perform (std::move (command)).wasOk());
        return raw->getResult();
    }

    /** A pattern with a note for a new 3x Osc channel. */
    model::PatternId makePattern (const juce::String& name)
    {
        const auto channel =
            run ("channel.add", R"({"name": ")" + name +
                                    R"(", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})")["channel"];
        const auto pattern = run ("pattern.add", R"({"name": ")" + name + R"(", "length": 3840})")["pattern"];
        run ("pattern.setNotes", R"({"pattern": )" + pattern.toString() + R"(, "channel": )" + channel.toString() +
                                     R"(, "notes": [{"start": 0, "length": 480, "pitch": 60}]})");
        session->setCurrentPattern (model::PatternId { (uint32_t)(int)pattern });
        return session->getCurrentPattern();
    }

    juce::MouseEvent event (juce::Point<float> p, juce::ModifierKeys mods, juce::Point<float> down, bool dragged)
    {
        auto* grid = &launcher->getGrid();
        return { juce::Desktop::getInstance().getMainMouseSource(),
                 p,
                 mods,
                 juce::MouseInputSource::defaultPressure,
                 juce::MouseInputSource::defaultOrientation,
                 juce::MouseInputSource::defaultRotation,
                 juce::MouseInputSource::defaultTiltX,
                 juce::MouseInputSource::defaultTiltY,
                 grid,
                 grid,
                 juce::Time::getCurrentTime(),
                 down,
                 juce::Time::getCurrentTime(),
                 1,
                 dragged };
    }

    void drag (juce::Point<float> from, juce::Point<float> to)
    {
        auto& grid = launcher->getGrid();
        const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
        grid.mouseDown (event (from, left, from, false));
        if (to != from)
            grid.mouseDrag (event (to, left, from, true));
        grid.mouseUp (event (to, {}, from, to != from));
        launcher->handleUpdateNowIfNeeded();
    }

    void click (juce::Point<float> p) { drag (p, p); }

    juce::Point<float> slot (int column, int row) const
    {
        return launcher->getSlotBounds (column, row).getCentre().toFloat();
    }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-launcher-panel") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<Launcher> launcher;
};
} // namespace

TEST_CASE ("Launcher panel: put patterns and audio in slots, move, loop, rename, remove", "[ui][launcher]")
{
    Fixture f;
    auto& launcher = *f.launcher;
    const auto& project = f.session->getProject();
    CHECK (launcher.getNumColumns() == Launcher::minimumColumns);
    CHECK (launcher.getNumRows() == Launcher::minimumRows);

    // No pattern yet: it says what to do.
    f.click (f.slot (0, 0));
    CHECK (project.launcher.clips.empty());
    CHECK (launcher.getStatusText().contains ("Make a pattern first"));

    // Clicking an empty slot puts the current pattern there, adding the tracks and scenes it needs.
    const auto riff = f.makePattern ("Riff");
    f.click (f.slot (1, 2));
    REQUIRE (launcher.getClip (1, 2) != nullptr);
    CHECK (project.launcher.tracks.size() == 2);
    CHECK (project.launcher.scenes.size() == 3);
    CHECK (std::get<model::PatternClip> (launcher.getClip (1, 2)->content).pattern == riff);
    CHECK (f.session->getHistory().getUndoDescriptions().strings.getLast() == "Put pattern in launcher");
    CHECK (launcher.getNumColumns() == 4); // one empty column to grow into, at least four
    CHECK (launcher.getNumRows() == 4);

    // Dragging moves it; one pass and looping can change.
    f.drag (f.slot (1, 2), f.slot (0, 0));
    CHECK (launcher.getClip (1, 2) == nullptr);
    REQUIRE (launcher.getClip (0, 0) != nullptr);
    const auto clip = launcher.getClip (0, 0)->id;
    REQUIRE (launcher.setLengthInBars (clip, 2));
    CHECK (project.launcher.findClip (clip)->length == 7680);
    REQUIRE (launcher.setLengthInBars (clip, 0));
    CHECK (project.launcher.findClip (clip)->length == 3840);
    REQUIRE (launcher.setLoop (clip, false));
    CHECK (! project.launcher.findClip (clip)->loop);

    // An audio file: its own channel, one pass of the file's length to the nearest beat.
    const auto tone = writeTone (f.root.getChildFile ("Pad Loop.wav"), 2.1); // 4.2 beats at 120 BPM
    CHECK (launcher.isInterestedInFileDrag ({ tone.getFullPathName() }));
    CHECK (! launcher.isInterestedInFileDrag ({ f.root.getChildFile ("notes.txt").getFullPathName() }));
    const auto at = launcher.getGrid().localPointToGlobal (f.slot (1, 0).toInt());
    launcher.filesDropped ({ tone.getFullPathName(), tone.getFullPathName() }, launcher.getLocalPoint (nullptr, at).x,
                           launcher.getLocalPoint (nullptr, at).y);
    REQUIRE (launcher.getClip (1, 0) != nullptr);
    REQUIRE (launcher.getClip (1, 1) != nullptr); // the second file goes below
    CHECK (launcher.getClip (1, 0)->length == 3840);
    CHECK (project.findChannel (std::get<model::AudioClip> (launcher.getClip (1, 0)->content).channel)->name ==
           "Pad Loop");

    // Names, quantising, removing.
    REQUIRE (launcher.renameTrack (0, "Keys"));
    REQUIRE (launcher.renameScene (0, "Intro"));
    CHECK (project.launcher.tracks[0].name == "Keys");
    CHECK (project.launcher.scenes[0].name == "Intro");
    REQUIRE (launcher.setQuantize (model::LaunchQuantize::beat));
    CHECK (project.launcher.quantize == model::LaunchQuantize::beat);
    REQUIRE (launcher.removeScene (1));
    CHECK (launcher.getClip (1, 1) == nullptr);
    REQUIRE (launcher.removeTrack (1));
    CHECK (project.launcher.tracks.size() == 1);

    // The keyboard: arrows choose a slot, Enter places or launches, Delete empties.
    launcher.selectSlot (0, 1);
    CHECK (launcher.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    launcher.handleUpdateNowIfNeeded();
    REQUIRE (launcher.getClip (0, 1) != nullptr);
    CHECK (launcher.keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
    CHECK (launcher.getSelectedSlot() == std::pair { 0, 0 });
    CHECK (launcher.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
    CHECK (launcher.getClip (0, 0) == nullptr);

    // Launching a clip starts playback; Stop stops it.
    REQUIRE (launcher.launch (0, 1));
    CHECK (f.session->isPlaying());
    CHECK (f.session->getAdapter().getLaunchState (launcher.getClip (0, 1)->id) !=
           engine::EngineAdapter::LaunchState::stopped);
    f.session->stop();
    CHECK (! launcher.launch (3, 3)); // an empty slot

    // "Record to Playlist" arms the session's launcher recording.
    for (auto* child : launcher.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child);
            button != nullptr && button->getButtonText() == "Record to Playlist")
        {
            button->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            CHECK (f.session->isLauncherRecording());
            button->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            CHECK (! f.session->isLauncherRecording());
        }

    // Undo walks it all back.
    while (f.session->getHistory().undo())
        ;
    CHECK (project.launcher.isEmpty());
}

TEST_CASE ("Clicking a launcher pattern clip makes its pattern the current one", "[ui][launcher]")
{
    Fixture f;
    const auto riff = f.makePattern ("Riff");
    f.click (f.slot (0, 0));
    REQUIRE (f.launcher->getClip (0, 0) != nullptr);
    const auto pad = f.makePattern ("Pad");
    CHECK (f.session->getCurrentPattern() == pad);

    f.click (f.slot (0, 0)); // also launches it
    CHECK (f.session->getCurrentPattern() == riff);
    f.session->stop();
}

TEST_CASE ("Launcher screenshot for the manual", "[.screenshot][launcher]")
{
    Fixture f;
    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    f.launcher->setLookAndFeel (&lookAndFeel);
    auto& launcher = *f.launcher;
    const juce::StringArray tracks { "Drums", "Bass", "Keys" }, scenes { "Intro", "Verse", "Drop" };
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
        {
            if (column == 2 && row == 0)
                continue;
            const auto pattern = f.makePattern (tracks[column] + " " + juce::String (row + 1));
            REQUIRE (launcher.placePattern (column, row, pattern));
        }
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE (launcher.renameTrack (i, tracks[i]));
        REQUIRE (launcher.renameScene (i, scenes[i]));
    }
    launcher.handleUpdateNowIfNeeded();
    launcher.resized();
    const auto image = launcher.createComponentSnapshot (launcher.getLocalBounds(), true, 1.0f);
    f.launcher->setLookAndFeel (nullptr);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/launcher.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}
