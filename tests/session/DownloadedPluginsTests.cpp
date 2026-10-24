/*  The free plugins Spinus downloads, used as Spinus devices: Six Sines (CLAP) and TIME-12 and GATE-12 (VST3).
    Each plays in an exported song, takes a setting by command, and comes back the same from a saved project.
    Point SPINUS_CHECK_DOWNLOADED at a folder holding their unpacked plugin files (searched recursively), e.g. the
    plugins folder after Options › Get free plugins. Skipped without it.

    Measurements and the plugins' settings as Spinus shows them are printed as "Downloaded | name | value" lines.
    Run: spinus_tests "[downloaded][devices]"  */

#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/StockDevices.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"

#include <iostream>

using namespace spinus;
namespace te = tracktion;

namespace
{
void metric (const juce::String& name, const juce::String& value)
{
    std::cout << "Downloaded | " << name << " | " << value << '\n';
}

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

/** Two bars at 120 BPM: a held chord (an effect hears 3x Osc play it), into insert 1. */
model::Project makeSong (const juce::String& stockType, bool instrument)
{
    using namespace model;
    Project p;
    p.settings.tempoBpm = 120.0;

    Insert insert;
    insert.id = p.allocateId<InsertId>();
    insert.name = "Test";
    if (! instrument)
        insert.effects.push_back ({ Device { p.allocateId<DeviceId>(), Device::Kind::stock, stockType, {}, false } });
    p.mixer.inserts = { insert };

    Channel c;
    c.id = p.allocateId<ChannelId>();
    c.name = "Notes";
    c.instrument = Device { p.allocateId<DeviceId>(),
                            Device::Kind::stock,
                            instrument ? stockType : juce::String ("spinus.3x-osc"),
                            {},
                            false };
    c.targetInsert = insert.id;
    p.channels = { c };

    Pattern pattern;
    pattern.id = p.allocateId<PatternId>();
    pattern.length = 8 * ticksPerQuarterNote;
    for (int pitch : { 48, 55, 60, 64 })
    {
        Note n;
        n.start = 0;
        n.length = 7 * ticksPerQuarterNote;
        n.pitch = pitch;
        pattern.notes[c.id].push_back (n);
    }
    p.patterns = { pattern };

    Lane lane { p.allocateId<LaneId>(), "Track 1", 0xff3d4450, false };
    p.playlist.lanes = { lane };
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), lane.id, 0, pattern.length, false, PatternClip { pattern.id, 0 } });
    return p;
}

/** The device under test: the effect on insert 1, or the channel's instrument. */
model::DeviceId deviceOf (const model::Project& p, bool instrument)
{
    const auto& channel = p.channels.front();
    return instrument && channel.instrument ? channel.instrument->id
                                            : p.mixer.inserts.front().effects.front().device.id;
}

juce::AudioBuffer<float> exportSong (Session& session, const juce::File& destination)
{
    std::optional<engine::ExportJob::Result> result;
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    auto job =
        session.createExportJob (destination, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (result.has_value());
    const auto done = result.value_or (engine::ExportJob::Result {});
    INFO (done.error);
    REQUIRE (done.ok);
    REQUIRE (! done.files.empty());

    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (done.files[0]), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio;
}

float dbOf (float gain) { return juce::Decibels::gainToDecibels (gain, -150.0f); }

/** The RMS of the difference between two exports' left channels, in dB (-150: the same). */
float differenceDb (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double sum = 0.0;
    const int n = std::min (a.getNumSamples(), b.getNumSamples());
    for (int i = 0; i < n; ++i)
        sum += juce::square ((double)a.getSample (0, i) - b.getSample (0, i));
    return dbOf ((float)std::sqrt (sum / std::max (1, n)));
}

/** The quietest and loudest 20 ms of the held chord (0.25 s to 3.25 s), in dB: how much the sound moves. */
std::pair<float, float> loudnessRange (const juce::AudioBuffer<float>& audio, double rate)
{
    const int window = (int)(0.02 * rate);
    float low = 1.0e9f, high = 0.0f;
    for (int start = (int)(0.25 * rate); start + window < (int)(3.25 * rate); start += window)
    {
        const auto rms = audio.getRMSLevel (0, start, window);
        low = std::min (low, rms);
        high = std::max (high, rms);
    }
    return { dbOf (low), dbOf (high) };
}

struct Downloaded
{
    const char* stockType;
    const char* file;
    bool instrument;
    const char* setting; // a parameter to set by command, by its name as the plugin gives it
};

std::vector<juce::File> foldersHolding (const juce::File& folder, const Downloaded& d)
{
    std::vector<juce::File> folders;
    for (const auto& found : folder.findChildFiles (juce::File::findFilesAndDirectories, true, d.file))
        folders.push_back (found.getParentDirectory());
    return folders;
}

std::unique_ptr<engine::DefaultDeviceFactory> makeDevices (te::Engine& engine, const std::vector<juce::File>& folders)
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, folders);
    return devices;
}
} // namespace

TEST_CASE ("Downloaded plugins as devices: they play, take settings by command and reopen the same",
           "[downloaded][devices]")
{
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (! folder.isDirectory())
        SKIP ("SPINUS_CHECK_DOWNLOADED is not set to a folder");

    const std::vector<Downloaded> downloaded {
        { "bundled.six-sines", "Six Sines.clap", true, "Main Output Gain" },
        { "bundled.time12", "TIME-12.vst3", false, "Mix" },
        { "bundled.gate12", "GATE-12.vst3", false, "Mix" },
    };

    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-downloaded-devices-" + juce::Uuid().toString());
    const juce::ScopeGuard cleanUp { [&temp] { temp.deleteRecursively(); } };

    for (auto& d : downloaded)
    {
        DYNAMIC_SECTION (d.stockType)
        {
            const auto folders = foldersHolding (folder, d);
            if (folders.empty())
                SKIP (d.file << " is not in " << folder.getFullPathName());
            const juce::String name = model::findBundledDevice (d.stockType)->displayName;

            EngineHost host { { .openAudioDevice = false } };
            Session session (host.getEngine(), makeDevices (host.getEngine(), folders));
            const auto song = makeSong (d.stockType, d.instrument);
            session.getHistory().reset (song);
            session.getEdit().dispatchPendingUpdatesSynchronously();
            const auto device = deviceOf (song, d.instrument);
            REQUIRE (session.getAdapter().getDevicePlugin (device) != nullptr);
            CHECK (session.getAdapter().getMissingDevices().empty());
            CHECK (engine::isBundledDeviceOpenFrom (folders.front())); // so Get free plugins keeps it

            // Its settings as Spinus lists them (the automation menu, the Value box).
            const auto parameters = session.getDeviceParameters (device);
            metric (name + ": settings", juce::String ((int)parameters.size()));
            juce::String settingId;
            for (size_t i = 0; i < parameters.size(); ++i)
            {
                const auto& [id, parameterName] = parameters[i];
                if (parameterName == d.setting)
                    settingId = id;
                if (i < 40)
                    metric (name + ": setting " + id + " " + parameterName,
                            session.getDeviceParameterValueText (device, id));
            }
            INFO ("no setting named " << d.setting);
            REQUIRE (settingId.isNotEmpty());

            // Sound: it plays (an instrument) or changes what passes through it (an effect).
            const auto audio = exportSong (session, temp.getChildFile (juce::String (d.file) + "-before"));
            const auto peak = audio.getMagnitude (0, audio.getNumSamples());
            const auto [low, high] = loudnessRange (audio, 48000.0);
            metric (name + ": exported peak dB", juce::String (dbOf (peak), 1));
            metric (name + ": quietest and loudest 20 ms of the chord, dB",
                    juce::String (low, 1) + " / " + juce::String (high, 1));
            CHECK (dbOf (peak) > -40.0f);
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                for (int i = 0; i < audio.getNumSamples(); ++i)
                    REQUIRE (std::isfinite (audio.getSample (ch, i)));

            // A setting by command, as the Assistant or automixer would; its text, typed back, gives the same value.
            REQUIRE (session.getHistory()
                         .perform (make ("device.setParameters", R"({"device": )" + juce::String (device.value) +
                                                                     R"(, "values": {")" + settingId + R"(": 0.25}})"))
                         .wasOk());
            session.getEdit().dispatchPendingUpdatesSynchronously();
            const auto text = session.getDeviceParameterValueText (device, settingId);
            metric (name + ": " + d.setting + " at 0.25 shows", text);
            CHECK (text.isNotEmpty());
            model::ParameterTarget target;
            target.kind = model::ParameterTarget::Kind::deviceParameter;
            target.device = device;
            target.parameterId = settingId;
            const auto typed = session.parseTargetValue (target, text);
            metric (name + ": \"" + text + "\" typed reads as",
                    typed ? juce::String (*typed, 3) : juce::String ("nothing"));
            CHECK ((typed.has_value() && std::abs (*typed - 0.25f) < 0.02f));

            const auto after = exportSong (session, temp.getChildFile (juce::String (d.file) + "-after"));
            metric (name + ": exported peak dB with " + d.setting + " at 0.25",
                    juce::String (dbOf (after.getMagnitude (0, after.getNumSamples())), 1));
            metric (name + ": the export with " + d.setting + " at 0.25 minus the first, RMS dB",
                    juce::String (differenceDb (after, audio), 1));
            // Exported again: the same, unless the synth starts its voices at random. Six Sines, without a sound card
            // running (as here), saves a setting changed by command only from its second save on, so its first
            // export after the command may not have it yet: this one shows whether it got there.
            const auto twice = exportSong (session, temp.getChildFile (juce::String (d.file) + "-twice"));
            metric (name + ": the same export again, peak dB / difference RMS dB",
                    juce::String (dbOf (twice.getMagnitude (0, twice.getNumSamples())), 1) + " / " +
                        juce::String (differenceDb (twice, after), 1));

            // Saved and opened again: the same setting, and the plugin's own state.
            const auto stateBefore = session.getAdapter().captureDeviceStates();
            REQUIRE (stateBefore.count (device) == 1);
            {
                ProjectDocument document (session, temp.getChildFile ("settings"));
                const auto saved = document.saveAs (temp.getChildFile (juce::String (d.file) + " song"));
                INFO (saved.getErrorMessage());
                REQUIRE (saved.wasOk());
            }

            EngineHost host2 { { .openAudioDevice = false } };
            Session reopened (host2.getEngine(), makeDevices (host2.getEngine(), folders));
            ProjectDocument document (reopened, temp.getChildFile ("settings2"));
            const auto projectFile =
                temp.getChildFile (juce::String (d.file) + " song").getChildFile (juce::String (d.file) + " song.spn");
            const auto opened = document.open (projectFile);
            INFO (opened.getErrorMessage());
            REQUIRE (opened.wasOk());
            reopened.getEdit().dispatchPendingUpdatesSynchronously();
            CHECK (reopened.getAdapter().getMissingDevices().empty());
            CHECK (reopened.getDeviceParameterValueText (device, settingId) == text);
            const auto stateAfter = reopened.getAdapter().captureDeviceStates();
            REQUIRE (stateAfter.count (device) == 1);
            metric (name + ": state bytes saved / reopened", juce::String ((int)stateBefore.at (device).getSize()) +
                                                                 " / " +
                                                                 juce::String ((int)stateAfter.at (device).getSize()));
            CHECK (stateAfter.at (device) == stateBefore.at (device));

            const auto again = exportSong (reopened, temp.getChildFile (juce::String (d.file) + "-reopened"));
            metric (name + ": reopened export minus the saved one, RMS dB",
                    juce::String (differenceDb (again, after), 1));
        }
    }
}
