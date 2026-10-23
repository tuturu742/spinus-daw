#include "SelfTest.h"
#include "core/ChildProcess.h"

#include "DemoSong.h"
#include "ProgramFolder.h"
#include "ProjectDocument.h"
#include "Session.h"
#include "ai/AiService.h"
#include "commands/CommandRegistry.h"
#include "project/ProjectFiles.h"
#include "engine/BundledDevices.h"

namespace spinus
{

namespace
{
juce::var run (Session& session, const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    if (command == nullptr)
        return {};
    auto* raw = command.get();
    if (session.getHistory().perform (std::move (command)).failed())
        return {};
    return raw->getResult();
}

/** True if the program starts (its libraries are all found) and ends by itself. */
bool starts (const juce::File& program, const juce::StringArray& arguments)
{
    core::ChildProcess process;
    juce::StringArray command { program.getFullPathName() };
    command.addArray (arguments);
    if (! program.existsAsFile() || ! process.start (command, core::ChildProcess::wantStdErr))
        return false;
    if (! process.waitForProcessToFinish (20000))
    {
        process.kill();
        return false;
    }
    // A missing library: the dynamic loader says so and the program never runs.
    return ! process.readAllProcessOutput().contains ("error while loading shared libraries");
}
} // namespace

bool runSelfTest (tracktion::Engine& engine, const juce::File& folder,
                  const std::function<void (const juce::String&)>& say)
{
    bool allPassed = true;
    auto check = [&] (bool passed, const juce::String& what)
    {
        say ((passed ? "ok: " : "FAILED: ") + what);
        allPassed = allPassed && passed;
        return passed;
    };
    folder.createDirectory();
    const auto settings = folder.getChildFile ("settings");

    // 1. A song: 3x Osc playing a chord for a bar, on the Playlist.
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, engine);
        Session session (engine, std::move (devices));
        ProjectDocument document (session, settings);
        const auto channel =
            run (session, "channel.add",
                 R"({"name": "Synth", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})")["channel"];
        const auto pattern = run (session, "pattern.add", R"({"name": "Chord", "length": 3840})")["pattern"];
        run (
            session, "pattern.setNotes",
            R"({"pattern": )" + pattern.toString() + R"(, "channel": )" + channel.toString() +
                R"(, "notes": [{"start": 0, "length": 3840, "pitch": 60}, {"start": 0, "length": 3840, "pitch": 64}]})");
        const auto lane = run (session, "lane.add", "{}")["lane"];
        const auto clip =
            run (session, "clip.addPattern",
                 R"({"lane": )" + lane.toString() + R"(, "pattern": )" + pattern.toString() + R"(, "start": 0})");
        check (! clip.isVoid() && session.getProject().channels.size() == 1, "make a song with 3x Osc");

        // 2. Save it, then open it again in a new session.
        const auto saved = document.saveAs (folder.getChildFile ("Self Test"));
        check (saved.wasOk(), "save the project" + (saved.failed() ? " (" + saved.getErrorMessage() + ")" : ""));
    }
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, engine);
    Session session (engine, std::move (devices));
    ProjectDocument document (session, settings);
    const auto opened = document.open (folder.getChildFile ("Self Test"));
    check (opened.wasOk() && session.getProject().patterns.size() == 1,
           "open the project again" + (opened.failed() ? " (" + opened.getErrorMessage() + ")" : ""));

    // 3. Export it: the chord is there.
    session.setMode (Session::Mode::song);
    engine::ExportSettings exportSettings;
    exportSettings.format = engine::ExportSettings::Format::wav32Float;
    exportSettings.tailSeconds = 0.5;
    std::optional<engine::ExportJob::Result> result;
    const auto wav = folder.getChildFile ("Self Test.wav");
    auto job = session.createExportJob (wav, exportSettings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    float level = 0.0f;
    if (result && result->ok)
    {
        juce::WavAudioFormat format;
        if (std::unique_ptr<juce::AudioFormatReader> reader (
                format.createReaderFor (new juce::FileInputStream (wav), true));
            reader != nullptr)
        {
            juce::AudioBuffer<float> audio (1, (int)reader->lengthInSamples);
            reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
            level = audio.getRMSLevel (0, 0, juce::jmin (audio.getNumSamples(), (int)reader->sampleRate));
        }
    }
    check (level > 0.01f, "export it with sound (" + juce::String (juce::Decibels::gainToDecibels (level), 1) + " dB)");

    // 4. The helper programs start.
    const auto programs = getProgramFolder();
    check (starts (getHelperProgram (programs, "spinus-plugin-scanner"), {}), "the plugin scanner starts");
    check (starts (getHelperProgram (programs, "spinus-plugin-host"), {}), "the plugin host starts");

    // 5. The AI service's Python has what the service needs (it runs only when AI is turned on). AI is
    //    optional: with no Python for it at all, Spinus works without AI; a Python that is there but
    //    lacks what the service needs is a failure.
    const auto installation = ai::AiService::findInstallation();
    if (installation.python == juce::File())
    {
        say ("skipped: the AI service's Python (not installed; AI is optional, Spinus works without it)");
    }
    else
    {
        core::ChildProcess python;
        const bool pythonRan = python.start (
            juce::StringArray { installation.python.getFullPathName(), "-c",
                                "import sys; sys.path.insert(0, '" +
                                    installation.package.getParentDirectory().getFullPathName() +
                                    "'); import spinus_ai.service, httpx, keyring, numpy; print('ready')" },
            core::ChildProcess::wantStdOut | core::ChildProcess::wantStdErr);
        const auto output = pythonRan ? python.readAllProcessOutput() : juce::String();
        check (output.contains ("ready"),
               "the AI service's Python (" + installation.python.getFullPathName() + ")" +
                   (output.contains ("ready") ? juce::String() : ": " + output.trim().getLastCharacters (300)));
    }

    // 6. The demo songs are there (the Browser's Projects) and read.
    const auto demos = demo::findBundledDemos();
    check (! demos.isEmpty(),
           "the demo songs (" +
               (demos.isEmpty() ? juce::String ("none found") : demo::findDemoFolder().getFullPathName()) + ")");
    for (const auto& demoSong : demos)
    {
        project::ProjectData demoData;
        const auto demoRead = project::loadProject (demoSong, demoData);
        check (demoRead.wasOk() && ! demoData.project.channels.empty(),
               "the demo song " + demoSong.getFileName() + " (" + demoSong.getFullPathName() + ")" +
                   (demoRead.wasOk() ? juce::String() : ": " + demoRead.getErrorMessage()));
    }
    return allPassed;
}

} // namespace spinus
