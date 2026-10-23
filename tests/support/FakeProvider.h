#pragma once

#include "ai/AiService.h"
#include "core/ChildProcess.h"

#include <juce_events/juce_events.h>

namespace spinus::test
{

/** Runs the message loop until done() or the time runs out. */
inline bool waitFor (const std::function<bool()>& done, int ms = 30000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
    while (! done() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    return done();
}

/** The fake local model server (tests/ai/fake_provider.py) on a free port. */
struct FakeProvider
{
    /** With a replies file, the server answers with that conversation instead (see fake_provider.py). With
        models, it lists those instead of "fake-model" and answers as the first. */
    explicit FakeProvider (const juce::File& replies = {}, const juce::StringArray& models = {})
    {
        juce::StreamingSocket probe;
        probe.createListener (0, "127.0.0.1");
        port = probe.getBoundPort();
        probe.close();
        const auto python = ai::AiService::findInstallation().python;
        juce::StringArray command {
            python.getFullPathName(),
            juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("ai/fake_provider.py").getFullPathName(),
            juce::String (port)
        };
        if (replies != juce::File())
            command.add (replies.getFullPathName());
        if (! models.isEmpty())
            command.add ("--models=" + models.joinIntoString (","));
        server.start (command);
        ready = server.readProcessOutput (buffer, 6) > 0;
    }
    ~FakeProvider() { server.kill(); }

    juce::String url() const { return "http://127.0.0.1:" + juce::String (port) + "/v1"; }

    spinus::core::ChildProcess server;
    int port = 0;
    char buffer[8] {};
    bool ready = false;
};

} // namespace spinus::test
