#include "RecordIdea.h"

namespace spinus::ui
{

namespace
{
const char* const soundNames[] = { "kick", "snare", "hat" };
const char* const soundWords[] = { "KICK (\"boom\")", "SNARE (\"psh\" or \"k\")", "HI-HAT (\"ts\")" };

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}
} // namespace

RecordIdea::RecordIdea (Session& s, ai::AiService& ai) : session (s), service (ai)
{
    listener = service.addListener (
        [this] (const juce::String& method, const juce::var& params)
        {
            if (method != "ai.v1.job.update" || job.isEmpty() || params["job"].toString() != job)
                return;
            const auto kindOfUpdate = params["kind"].toString();
            if (kindOfUpdate == "done")
            {
                job.clear();
                if (auto done = std::exchange (whenDone, nullptr))
                    done (params["result"]);
            }
            else if (kindOfUpdate == "error")
            {
                job.clear();
                stop (params["message"].toString());
            }
        });

    auto& recorder = session.getIdeaRecorder();
    recorder.onFinished = [this] (const IdeaRecorder::Take& take)
    {
        if (step == Step::recording)
            analyse (kind, take);
        else if (step == Step::teaching)
        {
            step = Step::learning;
            tell ("Learning your " + juce::String (soundNames[teaching]) + "...");
            callJob ("ai.v1.listen.teach",
                     object ({ { "file", take.file.getFullPathName() }, { "sound", soundNames[teaching] } }),
                     [this, file = take.file] (const juce::var&)
                     {
                         file.deleteFile();
                         ++teaching;
                         teachNext();
                     });
        }
    };
    recorder.onStopped = [this] (const juce::String& why)
    {
        if (step == Step::recording || step == Step::teaching)
            stop (why);
    };

    session.getSuggestions().addChangeListener (this);
    startTimerHz (10);
}

RecordIdea::~RecordIdea()
{
    session.getSuggestions().removeChangeListener (this);
    service.removeListener (listener);
    auto& recorder = session.getIdeaRecorder();
    recorder.onFinished = nullptr;
    recorder.onStopped = nullptr;
    recorder.cancel();
}

//==============================================================================
juce::Result RecordIdea::canListen() const
{
    if (! service.isEnabled())
        return juce::Result::fail ("Record idea listens with Spinus's AI service on this computer (nothing is sent "
                                   "anywhere). Turn AI on in Options › AI.");
    if (! session.getSelectedChannel().isValid())
        return juce::Result::fail ("Add a channel for the notes first (Channel Rack, F6).");
    if (isBusy())
        return juce::Result::fail ("Record idea is already busy.");
    return juce::Result::ok();
}

juce::Result RecordIdea::record (Kind what)
{
    if (auto ok = canListen(); ok.failed())
        return ok;

    kind = what;
    channel = session.getSelectedChannel();
    pattern = session.getCurrentPattern();
    step = Step::recording;
    if (auto started = session.getIdeaRecorder().startWithPattern(); started.failed())
    {
        step = Step::idle;
        return started;
    }
    pattern = session.getCurrentPattern();
    return juce::Result::ok();
}

juce::Result RecordIdea::teach()
{
    if (! service.isEnabled())
        return canListen();
    if (isBusy())
        return juce::Result::fail ("Record idea is already busy.");
    teaching = 0;
    teachNext();
    return step == Step::idle ? juce::Result::fail (shown) : juce::Result::ok();
}

void RecordIdea::teachNext()
{
    if (teaching >= 3)
    {
        step = Step::idle;
        tell ("Spinus learned your kick, snare and hi-hat. Beatbox a beat to try them.", true);
        return;
    }

    step = Step::teaching;
    if (auto started = session.getIdeaRecorder().startFree (teachSeconds); started.failed())
        return stop (started.getErrorMessage());
}

void RecordIdea::analyse (Kind what, const IdeaRecorder::Take& take)
{
    kind = what;
    if (! channel.isValid())
        channel = session.getSelectedChannel();
    if (! pattern.isValid())
        pattern = session.getCurrentPattern();
    step = Step::analysing;
    tell (what == Kind::melody ? "Listening to your melody..." : "Listening to your beat...");

    callJob (what == Kind::melody ? "ai.v1.listen.melody" : "ai.v1.listen.beats",
             object ({ { "file", take.file.getFullPathName() } }),
             [this, take, what] (const juce::var& heard)
             {
                 const IdeaToNotes convert;
                 const auto& project = session.getProject();
                 auto notes = what == Kind::melody ? convert.melody (heard, take, project, pattern)
                                                   : convert.beats (heard, take, project, pattern);
                 step = Step::idle;
                 take.file.deleteFile();
                 if (notes.empty())
                     return stop (
                         what == Kind::melody
                             ? "No notes were heard in your take. Sing a little louder or closer to the "
                               "microphone, after the count-in."
                             : "No hits were heard in your take. Beatbox a little louder, after the count-in.");

                 auto title = what == Kind::melody ? "Your melody (" + juce::String ((int)notes.size()) + " notes)"
                                                   : "Your beat (" + juce::String ((int)notes.size()) + " hits)";
                 if (what == Kind::beats && ! (bool)heard["taught"])
                     title << " - teach Spinus your sounds for better results";
                 shown.clear();
                 session.getSuggestions().show ({ { title, pattern, { { channel, std::move (notes) } } } },
                                                { "Record idea", nullptr });
             });
}

void RecordIdea::callJob (const juce::String& method, const juce::var& params,
                          std::function<void (const juce::var&)> done)
{
    whenDone = std::move (done);
    service.call (method, params,
                  [this] (const juce::var& result, const juce::String& error)
                  {
                      if (error.isNotEmpty())
                          return stop (error);
                      job = result["job"].toString();
                  });
}

//==============================================================================
void RecordIdea::tell (const juce::String& message, bool clearSoon)
{
    shown = message;
    clearAt = clearSoon ? juce::Time::getMillisecondCounter() + 6000 : 0;
    session.getSuggestions().setWorking (message);
}

void RecordIdea::stop (const juce::String& message)
{
    const auto wasBusy = step;
    step = Step::idle;
    whenDone = nullptr;
    if (job.isNotEmpty())
    {
        service.call ("ai.v1.job.cancel", object ({ { "job", job } }), [] (const juce::var&, const juce::String&) {});
        job.clear();
    }
    if (wasBusy == Step::recording || wasBusy == Step::teaching)
        session.getIdeaRecorder().cancel();
    if (message.isNotEmpty())
        tell (message, true);
    else if (shown.isNotEmpty())
    {
        shown.clear();
        session.getSuggestions().setWorking ({});
    }
}

void RecordIdea::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Discard in the bar while busy: stop.
    if (shown.isNotEmpty() && session.getSuggestions().getWorkingMessage().isEmpty())
    {
        shown.clear();
        if (isBusy())
            stop();
    }
}

void RecordIdea::timerCallback()
{
    if (clearAt != 0 && juce::Time::getMillisecondCounter() > clearAt)
    {
        clearAt = 0;
        if (! isBusy() && shown.isNotEmpty())
        {
            shown.clear();
            session.getSuggestions().setWorking ({});
        }
    }

    const auto& recorder = session.getIdeaRecorder();
    juce::String message;
    if (step == Step::recording)
    {
        const auto what = kind == Kind::melody ? juce::String ("sing or hum") : juce::String ("beatbox");
        message = recorder.getState() == IdeaRecorder::State::countIn
                      ? "Record idea: listen... " + what + " from bar 1 in " + juce::String (recorder.getBeatsToGo()) +
                            (recorder.getBeatsToGo() == 1 ? " beat" : " beats")
                      : "Recording: " + what + " now (" +
                            juce::String (juce::roundToInt (recorder.getProgress() * 100.0f)) + "%)";
    }
    else if (step == Step::teaching)
    {
        const auto left = juce::jmax (0, (int)std::ceil (teachSeconds * (1.0 - recorder.getProgress())));
        message = "Teach Spinus your sounds (" + juce::String (teaching + 1) + " of 3): make your " +
                  soundWords[teaching] + " sound 4 or 5 times, now (" + juce::String (left) + " s)";
    }

    if (message.isNotEmpty() && message != shown)
        tell (message);
}

//==============================================================================
void RecordIdea::showMenu (juce::Component& target)
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Record idea: over the pattern, after a count-in");
    menu.addItem ("Sing or hum a melody",
                  [this]
                  {
                      if (auto r = record (Kind::melody); r.failed())
                          tell (r.getErrorMessage(), true);
                  });
    menu.addItem ("Beatbox a beat",
                  [this]
                  {
                      if (auto r = record (Kind::beats); r.failed())
                          tell (r.getErrorMessage(), true);
                  });
    menu.addSeparator();
    menu.addItem ("Teach Spinus your beatbox sounds...",
                  [this]
                  {
                      if (auto r = teach(); r.failed())
                          tell (r.getErrorMessage(), true);
                  });
    if (isBusy())
        menu.addItem ("Stop", [this] { stop(); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

} // namespace spinus::ui
