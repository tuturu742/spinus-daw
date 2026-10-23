#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "../model/ExampleProject.h"
#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"
#include "project/ProjectFiles.h"
#if ! JUCE_WINDOWS
#include <unistd.h>
#endif

using namespace spinus;
using namespace spinus::project;
using spinus::test::makeExample;
namespace te = tracktion;

namespace
{

/** A fresh folder under the system temp directory, deleted afterwards. */
struct TempFolder
{
    TempFolder()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::String ("spinus-test") + "-" + juce::Uuid().toString()))
    {
        root.createDirectory();
    }

    ~TempFolder() { root.deleteRecursively(); }

    juce::File root;
};

juce::MemoryBlock blob (const juce::String& text)
{
    return juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8());
}

ProjectData exampleData()
{
    ProjectData data;
    data.project = makeExample();
    data.deviceStates[model::DeviceId { 3 }] = blob ("compressor state");
    data.deviceStates[model::DeviceId { 5 }] = blob ("reverb plugin state");
    return data;
}

void requireOk (const juce::Result& r)
{
    INFO (r.getErrorMessage());
    REQUIRE (r.wasOk());
}

juce::File writeWav (const juce::File& file, float frequency)
{
    juce::AudioBuffer<float> buffer (1, 4800);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample (0, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * frequency * (float)i / 48000.0f));

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        out, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return file;
}

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

} // namespace

//==============================================================================
TEST_CASE ("A project folder saves and loads with its device states", "[project]")
{
    TempFolder temp;
    const auto folder = temp.root.getChildFile ("Song");
    const auto data = exampleData();

    requireOk (saveProject (folder, data));
    CHECK (folder.getChildFile ("Song.spn").existsAsFile());
    CHECK (folder.getChildFile ("plugin-states/3.bin").existsAsFile());
    CHECK (folder.getChildFile ("audio").isDirectory());
    // No temporary files are left behind.
    CHECK (folder.findChildFiles (juce::File::findFiles, true, "*.tmp").isEmpty());
    CHECK (folder.findChildFiles (juce::File::findFiles, true, ".*temp*").isEmpty());

    ProjectData loaded;
    requireOk (loadProject (folder, loaded));
    CHECK (loaded.project == data.project);
    CHECK (loaded.deviceStates == data.deviceStates);

    // Saving again after removing a device drops its stale state file.
    auto changed = data;
    changed.deviceStates.erase (model::DeviceId { 5 });
    requireOk (saveProject (folder, changed));
    CHECK (! folder.getChildFile ("plugin-states/5.bin").exists());
    requireOk (loadProject (folder, loaded));
    CHECK (loaded.deviceStates == changed.deviceStates);
}

TEST_CASE ("Loading reports what is wrong", "[project]")
{
    TempFolder temp;
    ProjectData loaded;
    CHECK (
        loadProject (temp.root.getChildFile ("Missing"), loaded).getErrorMessage().contains ("not a project folder"));
    CHECK (loadProject (temp.root, loaded).getErrorMessage().contains ("no .spn project file"));

    temp.root.getChildFile ("Song.spn").replaceWithText ("{ broken");
    const auto error = loadProject (temp.root, loaded).getErrorMessage();
    CHECK (error.contains ("Song.spn"));
    CHECK (error.contains ("not valid JSON"));
}

TEST_CASE ("A renamed folder keeps its .spn file, and an older project.json becomes a .spn", "[project]")
{
    TempFolder temp;
    const auto data = exampleData();
    const auto renamed = temp.root.getChildFile ("Renamed");
    requireOk (saveProject (temp.root.getChildFile ("Song"), data));
    REQUIRE (temp.root.getChildFile ("Song").moveFileTo (renamed));

    CHECK (findProjectFile (renamed) == renamed.getChildFile ("Song.spn"));
    CHECK (projectFolderFor (renamed.getChildFile ("Song.spn")) == renamed);
    CHECK (projectFolderFor (renamed) == renamed);
    requireOk (saveProject (renamed, data));
    CHECK (! renamed.getChildFile ("Renamed.spn").exists());

    // A project saved in the older format: project.json in a "Name.spinus" folder.
    const auto old = temp.root.getChildFile ("Old.spinus");
    requireOk (saveProject (old, data));
    REQUIRE (old.getChildFile ("Old.spn").moveFileTo (old.getChildFile ("project.json")));
    CHECK (findProjectFile (old) == old.getChildFile ("project.json"));
    CHECK (projectFolderFor (old.getChildFile ("project.json")) == old);
    ProjectData loaded;
    requireOk (loadProject (old, loaded));
    CHECK (loaded.project == data.project);

    requireOk (saveProject (old, data));
    CHECK (old.getChildFile ("Old.spn").existsAsFile());
    CHECK (! old.getChildFile ("project.json").exists());
}

TEST_CASE ("Audio files are copied into the project", "[project]")
{
    TempFolder temp;
    const auto folder = temp.root.getChildFile ("Song");
    requireOk (saveProject (folder, exampleData()));

    const auto source = writeWav (temp.root.getChildFile ("kick.wav"), 60.0f);
    juce::String path;
    requireOk (importAudioFile (folder, source, path));
    CHECK (path == "kick.wav");
    CHECK (folder.getChildFile ("audio/kick.wav").hasIdenticalContentTo (source));

    // The same file again is not duplicated; a different file with the same name gets a new name.
    requireOk (importAudioFile (folder, source, path));
    CHECK (path == "kick.wav");

    REQUIRE (temp.root.getChildFile ("other").createDirectory());
    const auto otherKick = writeWav (temp.root.getChildFile ("other/kick.wav"), 90.0f);
    requireOk (importAudioFile (folder, otherKick, path));
    CHECK (path == "kick(2).wav");

    CHECK (importAudioFile (folder, temp.root.getChildFile ("nope.wav"), path).failed());
}

TEST_CASE ("Autosaves keep the newest 10 and are offered when newer than the saved project", "[project]")
{
    TempFolder temp;
    const auto folder = temp.root.getChildFile ("Song");
    requireOk (saveProject (folder, exampleData()));
    folder.getChildFile ("Song.spn")
        .setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::hours (1));

    Autosaves autosaves (folder);
    CHECK (! autosaves.findNewerThanSaved().has_value());

    auto data = exampleData();
    for (int i = 0; i < 12; ++i)
    {
        data.project.settings.tempoBpm = 100.0 + i;
        requireOk (autosaves.save (data));
    }

    const auto all = autosaves.list();
    CHECK (all.size() == Autosaves::keep);

    const auto newest = autosaves.findNewerThanSaved();
    REQUIRE (newest.has_value());
    ProjectData recovered;
    requireOk (loadProject (newest.value_or (juce::File()), recovered));
    CHECK (juce::approximatelyEqual (recovered.project.settings.tempoBpm, 111.0));
    CHECK (recovered.deviceStates == data.deviceStates);

    // Saved with the same contents (file times may not tell them apart): nothing to offer.
    requireOk (saveProject (folder, data));
    CHECK (! autosaves.findNewerThanSaved().has_value());

    // After a normal save the autosaves are older: nothing to offer.
    data.project.settings.tempoBpm = 90.0;
    requireOk (saveProject (folder, data));
    folder.getChildFile ("Song.spn")
        .setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::minutes (1));
    CHECK (! autosaves.findNewerThanSaved().has_value());
}

TEST_CASE ("A session lock left behind means the last session crashed", "[project]")
{
    TempFolder temp;
    CHECK (! SessionLock::previousSessionCrashed (temp.root));

    {
        const SessionLock lock (temp.root);
        CHECK (SessionLock::getState (temp.root) == SessionLock::State::openInSpinus);
        CHECK (! SessionLock::previousSessionCrashed (temp.root)); // still open, not crashed
    }

    CHECK (SessionLock::getState (temp.root) == SessionLock::State::none); // closed cleanly

#if ! JUCE_WINDOWS
    // A lock no Spinus holds any more, whatever process number it names (a Flatpak's Spinus is often number
    // 2 in its own sandbox, and a live process here): left by a crash.
    temp.root.getChildFile (".session.lock").replaceWithText ("pid " + juce::String (getpid()) + "\nflock\n");
    CHECK (SessionLock::previousSessionCrashed (temp.root));
#endif

    // A lock whose process is gone was left by a crash.
    temp.root.getChildFile (".session.lock").replaceWithText ("pid 999999999\nopened earlier\n");
    CHECK (SessionLock::previousSessionCrashed (temp.root));
    SessionLock::clearCrashedLock (temp.root);
    CHECK (SessionLock::getState (temp.root) == SessionLock::State::none);
}

//==============================================================================
TEST_CASE ("Device settings survive save, load and undo", "[project][engine]")
{
    EngineHost host ({ .openAudioDevice = false });
    test::TestDevices devices (host.getEngine());
    TempFolder temp;
    const auto folder = temp.root.getChildFile ("Song");

    float tweaked = 0.0f;
    {
        auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
        engine::EngineAdapter adapter (*edit, devices);
        commands::CommandHistory history (makeExample());
        history.addListener ([&] (const commands::ChangeSet&) { adapter.sync (history.getProject()); });
        adapter.sync (history.getProject());

        // The drums insert's compressor (device 3) is a Tracktion built-in; the reverb (device 5) is a
        // third-party plugin that is not installed here.
        auto* compressor = dynamic_cast<te::CompressorPlugin*> (adapter.getDevicePlugin (model::DeviceId { 3 }));
        REQUIRE (compressor != nullptr);
        compressor->setThreshold (0.123f);
        tweaked = compressor->getThreshold();

        // Removing the insert and undoing brings the compressor back with its setting.
        REQUIRE (history.perform (make ("insert.remove", R"({"insert": 2})")).wasOk());
        REQUIRE (history.undo());
        compressor = dynamic_cast<te::CompressorPlugin*> (adapter.getDevicePlugin (model::DeviceId { 3 }));
        REQUIRE (compressor != nullptr);
        CHECK (juce::approximatelyEqual (compressor->getThreshold(), tweaked));

        // The missing reverb keeps the state it was loaded with.
        adapter.setSavedDeviceStates ({ { model::DeviceId { 5 }, blob ("from another machine") } });
        ProjectData data { history.getProject(), adapter.captureDeviceStates() };
        CHECK (data.deviceStates.at (model::DeviceId { 5 }) == blob ("from another machine"));
        requireOk (saveProject (folder, data));
    }

    ProjectData loaded;
    requireOk (loadProject (folder, loaded));

    auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
    engine::EngineAdapter adapter (*edit, devices);
    adapter.setSavedDeviceStates (loaded.deviceStates);
    adapter.sync (loaded.project);

    auto* compressor = dynamic_cast<te::CompressorPlugin*> (adapter.getDevicePlugin (model::DeviceId { 3 }));
    REQUIRE (compressor != nullptr);
    CHECK (juce::approximatelyEqual (compressor->getThreshold(), tweaked));
    CHECK (adapter.captureDeviceStates().at (model::DeviceId { 5 }) == blob ("from another machine"));
}

#if defined(SPINUS_PLUGINS_DIR)
TEST_CASE ("A third-party plugin's settings survive save and load", "[project][engine]")
{
    const auto clap = juce::File (SPINUS_PLUGINS_DIR).getChildFile ("DragonflyRoomReverb.clap");
    if (! clap.existsAsFile())
        SKIP ("bundled plugins not built");

    EngineHost host ({ .openAudioDevice = false });
    auto& pluginManager = host.getEngine().getPluginManager();
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* format : pluginManager.pluginFormatManager.getFormats())
        if (format->getName() == "CLAP")
            format->findAllTypesForFile (found, clap.getFullPathName());
    REQUIRE (found.size() == 1);
    pluginManager.knownPluginList.addType (*found[0]);

    engine::DefaultDeviceFactory devices;
    model::Project project;
    model::Insert insert;
    insert.id = project.allocateId<model::InsertId>();
    model::Device reverb { project.allocateId<model::DeviceId>(),
                           model::Device::Kind::plugin,
                           {},
                           { "CLAP", found[0]->name, found[0]->manufacturerName, found[0]->fileOrIdentifier,
                             found[0]->uniqueId },
                           false };
    insert.effects.push_back ({ reverb, 1.0f });
    project.mixer.inserts.push_back (insert);

    juce::MemoryBlock saved;
    float value = 0.0f;
    {
        auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
        engine::EngineAdapter adapter (*edit, devices);
        adapter.sync (project);
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (adapter.getDevicePlugin (reverb.id));
        REQUIRE (plugin != nullptr);
        auto* parameter = plugin->getAudioPluginInstance()->getParameters()[2];
        parameter->setValueNotifyingHost (0.37f);
        value = parameter->getValue();
        saved = adapter.captureDeviceStates().at (reverb.id);
    }

    auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
    engine::EngineAdapter adapter (*edit, devices);
    adapter.setSavedDeviceStates ({ { reverb.id, saved } });
    adapter.sync (project);
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (adapter.getDevicePlugin (reverb.id));
    REQUIRE (plugin != nullptr);
    CHECK (std::abs (plugin->getAudioPluginInstance()->getParameters()[2]->getValue() - value) < 1.0e-3f);
}
#endif
