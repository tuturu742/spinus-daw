#pragma once

#include "AiService.h"

#include <functional>
#include <memory>

namespace spinus::ai
{

/*  The steps the analysis panels (the Analyzer, the Track creator) share with the AI service. Message
    thread only; nothing is called back once `alive` is false (the panel is gone). */

/** Starts a job (a method that answers {job}) and calls `finished` once when it ends: with its result, or
    with the error (the call's or the job's; then the result is void). `started` gets the job's id once it
    runs (to cancel it with ai.v1.job.cancel), `progress` its progress updates (done of total, the step's
    words). */
void runJob (AiService&, const juce::String& method, const juce::var& params, const std::shared_ptr<bool>& alive,
             std::function<void (const juce::String& job)> started,
             std::function<void (const juce::var& result, const juce::String& error)> finished,
             std::function<void (double done, double total, const juce::String& step)> progress = {});

/** What the analysis engine has: installed (a PyTorch build) and on which device, and the model files
    (ai.v1.stems.models: {models: [{id, name, license, size, page, downloaded}]}, asked only when installed). */
struct EngineCheck
{
    juce::String error; // the service could not answer
    bool installed = false, gpu = false;
    juce::var models;
};
void checkAnalysisEngine (AiService&, const std::shared_ptr<bool>& alive,
                          std::function<void (const EngineCheck&)> done);

} // namespace spinus::ai
