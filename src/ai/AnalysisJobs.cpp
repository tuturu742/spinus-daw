#include "AnalysisJobs.h"

namespace spinus::ai
{

void runJob (AiService& service, const juce::String& method, const juce::var& params,
             const std::shared_ptr<bool>& alive, std::function<void (const juce::String& job)> started,
             std::function<void (const juce::var& result, const juce::String& error)> finished,
             std::function<void (double done, double total, const juce::String& step)> progress)
{
    service.call (method, params,
                  [&service, alive, started = std::move (started), finished = std::move (finished),
                   progress = std::move (progress)] (const juce::var& reply, const juce::String& error)
                  {
                      if (! *alive)
                          return;
                      if (error.isNotEmpty())
                          return finished ({}, error);
                      const auto jobId = reply["job"].toString();
                      // The job's end comes as an update; listen for it once.
                      auto id = std::make_shared<int> (0);
                      *id = service.addListener (
                          [&service, alive, id, jobId, finished, progress] (const juce::String& m, const juce::var& p)
                          {
                              if (! *alive || m != "ai.v1.job.update" || p["job"].toString() != jobId)
                                  return;
                              const auto kind = p["kind"].toString();
                              if (kind == "progress" && progress)
                                  progress ((double)p["done"], (double)p["total"], p["step"].toString());
                              if (kind != "done" && kind != "error")
                                  return;
                              juce::MessageManager::callAsync ([s = &service, id] { s->removeListener (*id); });
                              if (kind == "error")
                                  return finished ({}, p["message"].toString().isNotEmpty()
                                                           ? p["message"].toString()
                                                           : juce::String ("The job failed."));
                              finished (p["result"], {});
                          });
                      if (started)
                          started (jobId);
                  });
}

void checkAnalysisEngine (AiService& service, const std::shared_ptr<bool>& alive,
                          std::function<void (const EngineCheck&)> done)
{
    service.call ("ai.v1.compute.status", {},
                  [&service, alive, done = std::move (done)] (const juce::var& engine, const juce::String& error)
                  {
                      if (! *alive)
                          return;
                      EngineCheck check;
                      check.error = error;
                      check.installed = error.isEmpty() && engine["uses"].isObject();
                      if (! check.installed)
                          return done (check);
                      check.gpu = engine["uses"]["device"].toString() != "cpu";
                      service.call ("ai.v1.stems.models", {},
                                    [alive, done, check] (const juce::var& list, const juce::String& error2) mutable
                                    {
                                        if (! *alive)
                                            return;
                                        check.error = error2;
                                        check.models = list;
                                        done (check);
                                    });
                  });
}

} // namespace spinus::ai
