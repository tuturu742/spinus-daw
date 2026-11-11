#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "../model/ExampleProject.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "session/ProjectDocument.h"

using namespace spinus;

namespace
{

constexpr double rate = 48000.0;

void requireOk (const juce::Result& r)
{
    INFO (r.getErrorMessage());
    REQUIRE (r.wasOk());
}

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::var& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, params, error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

juce::File writeTone (const juce::File& file, double seconds)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.5f * (float)std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate));
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

float rmsOf (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
    if (reader == nullptr)
        return 0.0f;
    juce::AudioBuffer<float> audio (1, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
    return audio.getRMSLevel (0, 0, juce::jmin (audio.getNumSamples(), (int)rate));
}

juce::MemoryBlock blob (const juce::String& text) { return { text.toRawUTF8(), text.getNumBytesAsUTF8() }; }

/** A session with a project document; everything on disk in a fresh temporary folder. */
struct Fixture
{
    Fixture()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-edge-test-" + juce::Uuid().toString())),
          host ({ .openAudioDevice = false }),
          session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()))
    {
        root.createDirectory();
        document = std::make_unique<ProjectDocument> (session, root.getChildFile ("settings"));
    }

    ~Fixture()
    {
        document = nullptr;
        root.deleteRecursively();
    }

    /** Adds an audio clip playing `file` (an absolute path: Save copies it into the project). */
    void addAudio (const juce::File& file, model::Ticks start = 0)
    {
        if (session.getProject().playlist.lanes.empty())
            requireOk (session.getHistory().perform (make ("lane.add", juce::JSON::parse (R"({"name": "Audio"})"))));
        auto* params = new juce::DynamicObject();
        params->setProperty ("lane", (int)session.getProject().playlist.lanes.front().id.value);
        params->setProperty ("file", file.getFullPathName());
        params->setProperty ("start", (int)start);
        params->setProperty ("length", 1920);
        requireOk (session.getHistory().perform (make ("clip.addAudio", juce::var (params))));
    }

    std::vector<juce::String> audioFiles() const
    {
        std::vector<juce::String> files;
        for (auto& clip : session.getProject().playlist.clips)
            if (auto* audio = std::get_if<model::AudioClip> (&clip.content))
                files.push_back (audio->file);
        return files;
    }

    engine::ExportJob::Result exportTo (const juce::File& destination, engine::ExportSettings settings = {})
    {
        session.setMode (Session::Mode::song);
        std::optional<engine::ExportJob::Result> result;
        auto job = session.createExportJob (destination, std::move (settings),
                                            [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        for (int i = 0; i < 600 && ! result; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        REQUIRE (result.has_value());
        return result.value_or (engine::ExportJob::Result {});
    }

    juce::File root;
    EngineHost host;
    Session session;
    std::unique_ptr<ProjectDocument> document;
};

} // namespace

//==============================================================================
TEST_CASE ("A project in a folder named with spaces, accents and Cyrillic saves, opens and exports",
           "[project][document][export]")
{
    Fixture f;
    // The sample's own name is not plain ASCII either, and it starts outside the project.
    const auto sample = writeTone (f.root.getChildFile (juce::CharPointer_UTF8 ("Сэмплы/бочка é 1.wav")), 1.0);
    f.addAudio (sample);

    const auto folder = f.root.getChildFile (juce::CharPointer_UTF8 ("Мои песни/Café Ünïcode 試し"));
    requireOk (f.document->saveAs (folder));
    CHECK (folder.getChildFile (juce::CharPointer_UTF8 ("Café Ünïcode 試し.spn")).existsAsFile());
    CHECK (folder.getChildFile ("audio").getChildFile (sample.getFileName()).existsAsFile());
    CHECK (f.audioFiles() == std::vector<juce::String> { sample.getFileName() });
    CHECK (f.document->getName() == juce::String (juce::CharPointer_UTF8 ("Café Ünïcode 試し")));
    const auto saved = f.session.getProject();

    // Opened again, by its folder and by its .spn file, with its audio found.
    f.document->newProject();
    requireOk (f.document->open (folder.getChildFile (juce::CharPointer_UTF8 ("Café Ünïcode 試し.spn"))));
    CHECK (f.session.getProject() == saved);
    CHECK (f.session.getAdapter().getMissingAudioClips().empty());
    CHECK (f.document->getRecentProjects().contains (folder));

    // Exported into the same folder, as WAV and FLAC, with the sample's sound in it.
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav16;
    auto result = f.exportTo (folder.getChildFile (juce::CharPointer_UTF8 ("Экспорт é")), settings);
    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.files.size() == 1);
    CHECK (result.files[0] == folder.getChildFile (juce::CharPointer_UTF8 ("Экспорт é.wav")));
    CHECK (rmsOf (result.files[0]) > 0.1f);

    settings.format = engine::ExportSettings::Format::flac;
    result = f.exportTo (folder.getChildFile (juce::CharPointer_UTF8 ("Экспорт é")), settings);
    INFO (result.error);
    REQUIRE (result.ok);
    CHECK (folder.getChildFile (juce::CharPointer_UTF8 ("Экспорт é.flac")).getSize() > 1000);

    // Saving again in place, and autosaving there, work too.
    requireOk (f.session.getHistory().perform (make ("project.setTempo", juce::JSON::parse (R"({"bpm": 99})"))));
    requireOk (f.document->autosave());
    CHECK (project::Autosaves (folder).list().size() == 1);
    requireOk (f.document->save());
}

TEST_CASE ("A missing sample: the project opens, keeps the clip and knows which file is missing", "[project][document]")
{
    Fixture f;
    f.addAudio (writeTone (f.root.getChildFile ("elsewhere/Kept.wav"), 0.5));
    f.addAudio (writeTone (f.root.getChildFile ("elsewhere/Gone.wav"), 0.5), 1920);
    const auto folder = f.root.getChildFile ("Song");
    requireOk (f.document->saveAs (folder));
    const auto saved = f.session.getProject();

    f.document->newProject();
    REQUIRE (folder.getChildFile ("audio/Gone.wav").deleteFile());
    requireOk (f.document->open (folder));
    CHECK (f.session.getProject() == saved);

    const auto missing = f.session.getAdapter().getMissingAudioClips();
    REQUIRE (missing.size() == 1);
    const auto* clip = f.session.getProject().findClip (missing.front());
    REQUIRE (clip != nullptr);
    CHECK (std::get<model::AudioClip> (clip->content).file == "Gone.wav");

    // Saving does not drop the clip: the file can be put back later.
    requireOk (f.document->save());
    model::Project reread;
    requireOk (model::fromJsonText (folder.getChildFile ("Song.spn").loadFileAsString(), reread));
    CHECK (reread == saved);

    // Put back where it was, it is found when the project opens again.
    writeTone (folder.getChildFile ("audio/Gone.wav"), 0.5);
    f.document->newProject();
    requireOk (f.document->open (folder));
    CHECK (f.session.getAdapter().getMissingAudioClips().empty());
}

TEST_CASE ("A missing plugin: the project opens, the slot is kept with its settings, saving keeps them",
           "[project][document]")
{
    Fixture f;
    // The example's reverb is a plugin not installed here (device 5).
    project::ProjectData data;
    data.project = test::makeExample();
    data.deviceStates[model::DeviceId { 5 }] = blob ("Dragonfly's own settings, saved on another computer");
    const auto folder = f.root.getChildFile ("Song");
    requireOk (project::saveProject (folder, data));
    const auto stateFile = folder.getChildFile ("plugin-states/5.bin");
    const auto original = stateFile.loadFileAsString();

    requireOk (f.document->open (folder));
    const auto missing = f.session.getAdapter().getMissingDevices();
    CHECK (std::find (missing.begin(), missing.end(), model::DeviceId { 5 }) != missing.end());
    REQUIRE (f.session.getProject().findDevice (model::DeviceId { 5 }) != nullptr);

    // Saved twice, with an edit in between and that edit undone: the settings stay as they came.
    requireOk (f.session.getHistory().perform (
        make ("insert.setEffect", juce::JSON::parse (R"({"device": 5, "bypassed": true})"))));
    requireOk (f.document->save());
    CHECK (stateFile.loadFileAsString() == original);
    REQUIRE (f.session.getHistory().undo());
    requireOk (f.document->save());
    CHECK (stateFile.loadFileAsString() == original);

    // Its effect slot moved to another insert and back, removed and brought back by undo: still kept.
    requireOk (f.session.getHistory().perform (
        make ("insert.moveEffect", juce::JSON::parse (R"({"device": 5, "insert": 6, "index": 0})"))));
    requireOk (f.session.getHistory().perform (make ("insert.removeEffect", juce::JSON::parse (R"({"device": 5})"))));
    REQUIRE (f.session.getHistory().undo());
    REQUIRE (f.session.getHistory().undo());
    requireOk (f.document->save());
    CHECK (stateFile.loadFileAsString() == original);

    // Autosaves keep it too, and so does Save As.
    requireOk (f.session.getHistory().perform (make ("project.setTempo", juce::JSON::parse (R"({"bpm": 90})"))));
    requireOk (f.document->autosave());
    const auto autosave = project::Autosaves (folder).list().getFirst();
    CHECK (autosave.getChildFile ("plugin-states/5.bin").loadFileAsString() == original);
    requireOk (f.document->saveAs (f.root.getChildFile ("Copy")));
    CHECK (f.root.getChildFile ("Copy/plugin-states/5.bin").loadFileAsString() == original);
}

TEST_CASE ("Save As to a new folder, then the original deleted: the copy opens with all its audio",
           "[project][document]")
{
    Fixture f;
    // Samples from elsewhere, added before the first save, between saves and just before the Save As, and one
    // put in the project's audio/ folder by hand; the folder they came from is gone when the copy opens.
    f.addAudio (writeTone (f.root.getChildFile ("elsewhere/First.wav"), 0.5));
    const auto original = f.root.getChildFile ("Original");
    requireOk (f.document->saveAs (original));
    f.addAudio (writeTone (f.root.getChildFile ("elsewhere/Second.wav"), 0.5), 1920);
    requireOk (f.document->save());
    f.addAudio (writeTone (f.root.getChildFile ("elsewhere/Third.wav"), 0.5), 3840);
    // Some project files sit in subfolders of audio/ (an import kept its folder).
    writeTone (original.getChildFile ("audio/drums/Loop.wav"), 0.5);
    requireOk (f.session.getHistory().perform (make (
        "clip.addAudio",
        juce::JSON::parse (R"({"lane": )" + juce::String (f.session.getProject().playlist.lanes.front().id.value) +
                           R"(, "file": "drums/Loop.wav", "start": 5760, "length": 960})"))));

    const auto copy = f.root.getChildFile ("Copy of the song");
    requireOk (f.document->saveAs (copy));
    CHECK (f.audioFiles() == std::vector<juce::String> { "First.wav", "Second.wav", "Third.wav", "drums/Loop.wav" });
    const auto saved = f.session.getProject();
    f.document->newProject();

    REQUIRE (original.deleteRecursively());
    REQUIRE (f.root.getChildFile ("elsewhere").deleteRecursively());

    requireOk (f.document->open (copy));
    CHECK (f.session.getProject() == saved);
    CHECK (f.session.getAdapter().getMissingAudioClips().empty());
    for (auto& file : f.audioFiles())
        CHECK (copy.getChildFile ("audio").getChildFile (file).existsAsFile());
    CHECK (! f.document->getRecentProjects().contains (original)); // the deleted one is not listed
}

TEST_CASE ("A damaged project file gives a clear message, changes nothing, and its autosave still opens",
           "[project][document]")
{
    Fixture f;
    const auto folder = f.root.getChildFile ("Song");
    f.session.getHistory().reset (test::makeExample());
    requireOk (f.document->saveAs (folder));
    requireOk (f.session.getHistory().perform (make ("project.setTempo", juce::JSON::parse (R"({"bpm": 77})"))));
    requireOk (f.document->autosave());
    const auto autosaved = f.session.getProject();
    f.document->newProject();

    const auto file = folder.getChildFile ("Song.spn");
    const auto good = file.loadFileAsString();
    const std::vector<std::pair<const char*, juce::String>> damages {
        { "cut off", good.substring (0, good.length() / 2) },
        { "cut off at the first byte", good.substring (0, 1) },
        { "not text", juce::String (juce::CharPointer_UTF8 ("\xc3\x28\xa0\xa1 garbage")) },
        { "a list", "[1, 2, 3]" },
        { "an empty object", "{}" },
        { "a wrong type", R"({"schemaVersion": 1, "channels": "none"})" },
        { "from a newer Spinus", R"({"schemaVersion": 99})" },
    };

    // Something open that must stay as it is when opening fails.
    f.session.getHistory().reset (test::makeExample());
    const auto before = f.session.getProject();

    for (auto& [what, text] : damages)
    {
        INFO (what);
        REQUIRE (file.replaceWithText (text, false, false, nullptr));
        const auto result = f.document->open (folder);
        CHECK (result.failed());
        INFO (result.getErrorMessage());
        CHECK (result.getErrorMessage().contains (file.getFullPathName()));
        CHECK (f.session.getProject() == before);
        CHECK (f.document->isUntitled());
    }

    // Zero bytes (what a crash while the disk was writing can leave), and an empty file.
    const char zeros[4096] {};
    REQUIRE (file.replaceWithData (zeros, sizeof (zeros)));
    for (int empty = 0; empty < 2; ++empty)
    {
        INFO (file.getSize() << " bytes");
        const auto result = f.document->open (folder);
        CHECK (result.failed());
        INFO (result.getErrorMessage());
        CHECK (result.getErrorMessage().contains (file.getFullPathName()));
        CHECK (f.session.getProject() == before);
        REQUIRE (file.deleteFile());
        REQUIRE (file.create()); // empty
    }

    // The autosave from before the damage opens.
    const auto autosave = project::Autosaves (folder).list();
    REQUIRE (autosave.size() == 1);
    project::ProjectData recovered;
    requireOk (project::loadProject (autosave.getFirst(), recovered));
    CHECK (recovered.project == autosaved);
}

TEST_CASE ("Projects saved by the alpha releases open, and saving them again loses nothing", "[project][document]")
{
    // The demo songs shipped with alpha 1 to 4 are byte for byte the ones in demo/ (and tests/model/files has
    // the schema's example): projects as those versions wrote them.
    Fixture f;
    const auto demo = juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo");
    auto songs = demo.findChildFiles (juce::File::findDirectories, false);
    REQUIRE (songs.size() >= 4);

    // Every key and value of the old file is still in the file saved now (new keys may be added).
    std::function<juce::String (const juce::var&, const juce::var&, const juce::String&)> lost;
    lost = [&] (const juce::var& old, const juce::var& now, const juce::String& path) -> juce::String
    {
        if (auto* object = old.getDynamicObject())
        {
            for (auto& [key, value] : object->getProperties())
                if (auto problem =
                        lost (value, now.getProperty (key, juce::var::undefined()), path + "." + key.toString());
                    problem.isNotEmpty())
                    return problem;
            return {};
        }
        if (auto* array = old.getArray())
        {
            if (now.getArray() == nullptr || now.size() != array->size())
                return path + ": " + juce::String (array->size()) + " items before, " + juce::String (now.size()) +
                       " now";
            for (int i = 0; i < array->size(); ++i)
                if (auto problem = lost ((*array)[i], now[i], path + "[" + juce::String (i) + "]");
                    problem.isNotEmpty())
                    return problem;
            return {};
        }
        if (now.isUndefined())
            return path + ": gone";
        if (old.isDouble() || old.isInt() || old.isInt64())
            return std::abs ((double)old - (double)now) < 1.0e-6 * juce::jmax (1.0, std::abs ((double)old))
                       ? juce::String()
                       : path + ": " + old.toString() + " before, " + now.toString() + " now";
        return old == now ? juce::String() : path + ": " + old.toString() + " before, " + now.toString() + " now";
    };

    // Spinus's own devices load (the third-party plugins are not installed here: their settings must be kept).
    EngineHost& host = f.host;
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    ProjectDocument document (session, f.root.getChildFile ("settings2"));

    auto settingsOfDevices = [&]
    {
        std::map<model::DeviceId, std::map<juce::String, float>> result;
        for (auto& channel : session.getProject().channels)
            if (channel.instrument)
                result[channel.instrument->id] = session.getAdapter().readParameters (channel.instrument->id);
        for (auto& insert : session.getProject().mixer.inserts)
            for (auto& slot : insert.effects)
                result[slot.device.id] = session.getAdapter().readParameters (slot.device.id);
        return result;
    };

    for (auto& song : songs)
    {
        INFO (song.getFileName());
        const auto copy = f.root.getChildFile (song.getFileName());
        REQUIRE (song.copyDirectoryTo (copy));
        const auto oldText = project::findProjectFile (copy).loadFileAsString();
        std::map<juce::String, juce::MemoryBlock> oldStates;
        for (auto& state : copy.getChildFile ("plugin-states").findChildFiles (juce::File::findFiles, false, "*.bin"))
            state.loadFileAsData (oldStates[state.getFileName()]);

        requireOk (document.open (copy));
        const auto opened = session.getProject();
        const auto settings = settingsOfDevices();
        const auto missing = session.getAdapter().getMissingDevices();
        requireOk (document.save());
        document.newProject();

        const auto problem = lost (juce::JSON::parse (oldText),
                                   juce::JSON::parse (project::findProjectFile (copy).loadFileAsString()), "");
        CHECK (problem == "");

        // Device settings: the missing plugins' as they were, byte for byte; Spinus's own devices read the same.
        for (auto& [name, state] : oldStates)
        {
            INFO (name);
            const auto now = copy.getChildFile ("plugin-states").getChildFile (name);
            CHECK (now.existsAsFile());
            const model::DeviceId id { (uint32_t)now.getFileNameWithoutExtension().getIntValue() };
            if (std::find (missing.begin(), missing.end(), id) != missing.end())
            {
                juce::MemoryBlock bytes;
                now.loadFileAsData (bytes);
                CHECK (bytes == state);
            }
        }

        requireOk (document.open (copy));
        CHECK (session.getProject() == opened);
        // The same settings, to float precision: they come back from the devices' saved states (a value read
        // back through a state may differ in its last bit).
        const auto again = settingsOfDevices();
        REQUIRE (again.size() == settings.size());
        for (auto& [id, values] : settings)
        {
            INFO ("device " << (int)id.value);
            REQUIRE (again.contains (id));
            const auto& now = again.at (id);
            CHECK (now.size() == values.size());
            for (auto& [parameter, value] : values)
            {
                INFO (parameter);
                CHECK (now.contains (parameter));
                if (auto it = now.find (parameter); it != now.end())
                    CHECK (std::abs (it->second - value) < 1.0e-5f);
            }
        }
        document.newProject();
    }
}
