#include <catch2/catch_test_macros.hpp>

#include "FakeProvider.h"
#include "ai/AiService.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "ui/AiSettingsDialog.h"
#include "ui/AssistantPanel.h"
#include "ui/MainView.h"

using namespace spinus;
using namespace spinus::ui;
using spinus::test::waitFor;

// Spinus with the AI service missing, crashing in the middle of a request, or not answering at all (tests/ai/
// fake-service stands in for it; no model and no internet are used): the DAW stays usable, the message thread
// never waits for the service, and the AI panels say what went wrong.

namespace
{
struct Fixture
{
    /** `mode`: what the stand-in does (see its run_spinus_ai.py); empty: the service is not installed. */
    explicit Fixture (const juce::String& mode)
    {
        root.createDirectory();
        auto installation = ai::AiService::findInstallation();
        if (mode.isEmpty())
            installation.python = juce::File();
        else
        {
            // The stand-in needs only Python's standard library: any Python does.
            if (installation.python == juce::File())
                installation.python = systemPython();
            installation.package =
                juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("ai/fake-service").getChildFile ("spinus_ai");
        }
        service.setInstallation (installation);
        service.getFolder().createDirectory();
        service.getFolder().getChildFile ("fake-mode").replaceWithText (mode);
        service.setEnabled (true);
        panel.setSize (300, 600);
    }

    ~Fixture()
    {
        service.setEnabled (false);
        root.deleteRecursively();
    }

    static juce::File systemPython()
    {
        for (auto* path : { "/usr/bin/python3", "/usr/local/bin/python3" })
            if (juce::File (path).existsAsFile())
                return juce::File (path);
        return {};
    }

    /** The DAW works meanwhile: a change, its undo, play and stop, and a save, each right away. */
    void checkUsable()
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        juce::String error;
        auto command =
            commands::CommandRegistry::get().create ("project.setTempo", juce::JSON::parse (R"({"bpm": 133})"), error);
        REQUIRE (command != nullptr);
        REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
        session.play();
        session.stop();
        REQUIRE (session.getHistory().undo());
        CHECK (juce::Time::getMillisecondCounterHiRes() - start < 2000.0);
    }

    /** How long the message thread takes to run a message: it never waits for the service. */
    static double messageDelay()
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        bool ran = false;
        juce::MessageManager::callAsync ([&ran] { ran = true; });
        waitFor ([&ran] { return ran; }, 5000);
        return juce::Time::getMillisecondCounterHiRes() - start;
    }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-ai-misbehaving-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ai::AiService service { root };
    AssistantPanel panel { session, service };
};

bool pythonMissing()
{
    return ai::AiService::findInstallation().python == juce::File() && Fixture::systemPython() == juce::File();
}
} // namespace

TEST_CASE ("Without the AI service installed, the assistant says so and the DAW works", "[ui][ai][misbehaving]")
{
    Fixture f ({});
    f.panel.ask ("Make it slower");
    REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }, 5000));
    REQUIRE (! f.panel.getEntries().empty());
    CHECK (f.panel.getEntries().back().kind == AssistantPanel::Entry::Kind::error);
    CHECK (f.panel.getEntries().back().text.contains ("not installed"));
    CHECK (f.service.getState() == ai::AiService::State::failed);
    f.checkUsable();

    // The main window too: its AI items work or say why, and project commands don't care.
    MainView view (f.session);
    view.setAiService (&f.service);
    juce::String error;
    REQUIRE (f.session.getHistory()
                 .perform (commands::CommandRegistry::get().create ("project.setTempo",
                                                                    juce::JSON::parse (R"({"bpm": 90})"), error))
                 .wasOk());
    CHECK (view.getCommandManager().invokeDirectly (MainView::undo, false));
    CHECK (view.getCommandManager().invokeDirectly (MainView::aiSettings, false));
    AiSettingsDialog::closeWindow();
}

TEST_CASE ("The AI service crashing in the middle of a request: the assistant says so, and asks again later",
           "[ui][ai][misbehaving]")
{
    if (pythonMissing())
        SKIP ("no Python for the AI service");

    // Before it answers.
    {
        Fixture f ("crash");
        f.panel.ask ("Make it slower");
        CHECK (f.panel.isAnswering());
        REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }));
        REQUIRE (! f.panel.getEntries().empty());
        const auto& last = f.panel.getEntries().back();
        CHECK (last.kind == AssistantPanel::Entry::Kind::error);
        INFO (last.text);
        CHECK (last.text.contains ("stopped unexpectedly"));
        CHECK (last.text.contains ("crashed on purpose")); // its last words, from its log
        f.checkUsable();

        // The next question starts it again (and here it crashes again, and says so again).
        f.panel.ask ("Again?");
        REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }));
        CHECK (f.panel.getEntries().back().kind == AssistantPanel::Entry::Kind::error);
    }

    // After it took the question, while it works on the answer.
    {
        Fixture f ("crash-job");
        f.panel.ask ("Make it slower");
        REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }));
        REQUIRE (! f.panel.getEntries().empty());
        const auto& last = f.panel.getEntries().back();
        INFO (last.text);
        CHECK (last.kind == AssistantPanel::Entry::Kind::error);
        CHECK (last.text.contains ("stopped"));
        f.checkUsable();
    }
}

TEST_CASE ("An AI service that doesn't answer: the DAW works, Stop frees the assistant", "[ui][ai][misbehaving]")
{
    if (pythonMissing())
        SKIP ("no Python for the AI service");

    Fixture f ("silent");
    f.panel.ask ("Make it slower");
    CHECK (f.panel.isAnswering());
    CHECK (f.panel.getStatusText().isNotEmpty()); // "Thinking..."
    REQUIRE (waitFor ([&] { return f.service.getState() == ai::AiService::State::running; }));

    // A few seconds without an answer: nothing waits for it.
    for (int i = 0; i < 10; ++i)
    {
        CHECK (Fixture::messageDelay() < 500.0);
        f.checkUsable();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    }
    CHECK (f.panel.isAnswering());

    // The user gives up: Stop frees the panel at once, though the service never answers, and says so.
    f.panel.stop();
    CHECK (! f.panel.isAnswering());
    REQUIRE (! f.panel.getEntries().empty());
    CHECK (f.panel.getEntries().back().kind == AssistantPanel::Entry::Kind::error);
    CHECK (f.panel.getEntries().back().text.contains ("Stopped"));

    // Options > AI says it is waiting, and turning AI off ends the wait.
    AiSettingsDialog settings (f.service);
    CHECK (settings.isWaiting());
    settings.setAiOn (false);
    CHECK (waitFor ([&] { return ! settings.isWaiting(); }, 5000));
    CHECK (f.service.getState() == ai::AiService::State::off);
}

TEST_CASE ("An AI service that doesn't answer within the time limit: Spinus stops waiting and says so",
           "[ui][ai][misbehaving][time-limit]")
{
    if (pythonMissing())
        SKIP ("no Python for the AI service");

    // No reply to the question at all.
    {
        Fixture f ("silent");
        f.service.setTimeLimit (1);
        f.panel.ask ("Make it slower");
        CHECK (f.panel.isAnswering());
        REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }, 15000));
        REQUIRE (! f.panel.getEntries().empty());
        const auto& last = f.panel.getEntries().back();
        INFO (last.text);
        CHECK (last.kind == AssistantPanel::Entry::Kind::error);
        CHECK (last.text.startsWith ("No answer from the AI service for 1 second."));
        CHECK (last.text.contains ("raise the time limit in Options > AI"));
        f.checkUsable();
    }

    // The question is taken, as a job, and then nothing more comes.
    {
        Fixture f ("silent-job");
        f.service.setTimeLimit (1);
        f.panel.ask ("Make it slower");
        REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }, 15000));
        REQUIRE (! f.panel.getEntries().empty());
        const auto& last = f.panel.getEntries().back();
        INFO (last.text);
        CHECK (last.kind == AssistantPanel::Entry::Kind::error);
        CHECK (last.text.startsWith ("No answer from the AI service for 1 second."));

        // Any other job (Generate, Record idea...) gets its last update, an error, the way they all end.
        juce::String job, kind, message;
        const auto listener = f.service.addListener (
            [&] (const juce::String& method, const juce::var& params)
            {
                if (method == "ai.v1.job.update" && params["job"].toString() == job)
                {
                    kind = params["kind"].toString();
                    message = params["message"].toString();
                }
            });
        f.service.call ("ai.v1.pattern.generate", juce::var (new juce::DynamicObject()),
                        [&] (const juce::var& result, const juce::String&) { job = result["job"].toString(); });
        REQUIRE (waitFor ([&] { return kind.isNotEmpty(); }, 15000));
        CHECK (job.isNotEmpty());
        CHECK (kind == "error");
        CHECK (message.startsWith ("No answer from the AI service for 1 second."));
        f.service.removeListener (listener);
        f.checkUsable();
    }
}

TEST_CASE ("A job that keeps reporting progress may take longer than the time limit", "[ui][ai][time-limit]")
{
    if (pythonMissing())
        SKIP ("no Python for the AI service");

    // 3 seconds in all, with a word every 0.3 s: the limit counts from the last word, not from the start.
    Fixture f ("slow-job");
    f.service.setTimeLimit (1);
    f.panel.ask ("Make it slower");
    REQUIRE (waitFor ([&] { return ! f.panel.isAnswering(); }, 15000));
    REQUIRE (! f.panel.getEntries().empty());
    const auto& last = f.panel.getEntries().back();
    INFO (last.text);
    CHECK (last.kind == AssistantPanel::Entry::Kind::assistant);
    CHECK (last.text.contains ("Fine."));
}

TEST_CASE ("The time limit for an answer is kept with the AI settings", "[ui][ai][time-limit]")
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-ai-time-limit-" + juce::Uuid().toString());
    {
        ai::AiService service (root);
        CHECK (service.getTimeLimit() == 180); // 3 minutes until the user changes it
        AiSettingsDialog dialog (service);
        CHECK (dialog.getTimeLimitText() == "3 minutes");
        dialog.setTimeLimit (600);
        CHECK (service.getTimeLimit() == 600);
        CHECK (dialog.getTimeLimitText() == "10 minutes");
        service.setEnabled (true);
    }
    {
        ai::AiService service (root);
        CHECK (service.getTimeLimit() == 600);
        CHECK (service.isEnabled()); // kept beside the other settings, not instead of them
        AiSettingsDialog dialog (service);
        CHECK (dialog.getTimeLimitText() == "10 minutes");
        dialog.setTimeLimit (45);
        CHECK (dialog.getTimeLimitText() == "45 seconds");
        dialog.setTimeLimit (0);
        CHECK (dialog.getTimeLimitText() == "No limit");
        service.setEnabled (false);
    }
    {
        ai::AiService service (root);
        CHECK (service.getTimeLimit() == 0);
    }
    CHECK (ai::AiService::describeTimeLimit (60) == "1 minute");
    CHECK (ai::AiService::describeTimeLimit (90) == "90 seconds");
    root.deleteRecursively();
}
