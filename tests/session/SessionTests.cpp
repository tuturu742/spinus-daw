#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "../model/ExampleProject.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "engine/Beats.h"
#include "session/Session.h"
#include "RealtimeRunner.h"

#include <tracktion_graph/tracktion_graph.h>

using namespace spinus;
namespace te = tracktion;

namespace
{

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

struct Fixture
{
    Fixture()
        : host ({ .openAudioDevice = false }),
          session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()))
    {
        session.getHistory().reset (test::makeExample());
    }

    juce::String describe() { return session.getAdapter().describe(); }

    EngineHost host;
    Session session;
};

int countLines (const juce::String& text, const juce::String& part)
{
    int n = 0;
    for (auto& line : juce::StringArray::fromLines (text))
        n += line.contains (part) ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE ("Pattern mode plays the current pattern alone; song mode plays the playlist", "[session]")
{
    Fixture f;
    CHECK (f.session.getMode() == Session::Mode::pattern);
    CHECK (f.session.getCurrentPattern() == model::PatternId { 11 }); // the example's only pattern

    // Pattern mode: the playlist clips are silenced and the pattern plays from 0 on its own.
    auto text = f.describe();
    CHECK (countLines (text, "muted=1") == 4);
    CHECK (countLines (text, "pattern preview channel") == 2);
    CHECK (text.contains ("pattern preview channel 7: 0.000-16.000 notes=[36@0.000 36@1.000 36@2.000 36@3.000]"));
    CHECK (f.session.getAdapter().getPatternLoopRange().getEnd() > te::TimePosition());

    f.session.setMode (Session::Mode::song);
    text = f.describe();
    CHECK (countLines (text, "muted=0") == 4);
    CHECK (countLines (text, "pattern preview") == 0);
    CHECK (f.session.getAdapter().getPatternLoopRange().isEmpty());

    // Editing the pattern in pattern mode updates what plays.
    f.session.setMode (Session::Mode::pattern);
    REQUIRE (
        f.session.getHistory()
            .perform (make ("pattern.setNotes",
                            R"({"pattern": 11, "channel": 7, "notes": [{"start": 0, "length": 480, "pitch": 40}]})"))
            .wasOk());
    // (The example's pattern was 16 beats long; its notes fit in one bar of the song's 3/4, so it is that now.)
    CHECK (f.describe().contains ("pattern preview channel 7: 0.000-3.000 notes=[40@0.000]"));
}

TEST_CASE ("Play loops the pattern; stop returns to where playback started", "[session]")
{
    Fixture f;
    auto& transport = f.session.getEdit().getTransport();

    f.session.play();
    CHECK (f.session.isPlaying());
    CHECK (transport.looping.get());
    CHECK (transport.getLoopRange() == f.session.getAdapter().getPatternLoopRange());

    f.session.stop();
    CHECK (! f.session.isPlaying());
    CHECK (transport.getPosition() == te::TimePosition());

    // Song mode starts where the position is, and goes back there on stop.
    f.session.setMode (Session::Mode::song);
    transport.setPosition (te::TimePosition::fromSeconds (2.0));
    f.session.togglePlay();
    CHECK (! transport.looping.get());
    transport.setPosition (te::TimePosition::fromSeconds (3.0));
    f.session.togglePlay();
    CHECK (transport.getPosition() == te::TimePosition::fromSeconds (2.0));
}

TEST_CASE ("The current pattern follows patterns being added and removed", "[session]")
{
    Fixture f;
    f.session.getHistory().reset (model::Project());
    CHECK (! f.session.getCurrentPattern().isValid());

    REQUIRE (f.session.getHistory().perform (make ("pattern.add", R"({"name": "A"})")).wasOk());
    const auto a = f.session.getProject().patterns[0].id;
    CHECK (f.session.getCurrentPattern() == a);

    REQUIRE (f.session.getHistory().perform (make ("pattern.add", R"({"name": "B"})")).wasOk());
    const auto b = f.session.getProject().patterns[1].id;
    f.session.setCurrentPattern (b);
    CHECK (f.session.getCurrentPattern() == b);

    f.session.setCurrentPattern (model::PatternId { 999 }); // unknown: ignored
    CHECK (f.session.getCurrentPattern() == b);

    REQUIRE (f.session.getHistory()
                 .perform (make ("pattern.remove", "{\"pattern\": " + juce::String (b.value) + "}"))
                 .wasOk());
    CHECK (f.session.getCurrentPattern() == a);
}

TEST_CASE ("Position text and metronome", "[session]")
{
    Fixture f;
    f.session.getHistory().reset (model::Project()); // 120 BPM, 4/4
    auto& transport = f.session.getEdit().getTransport();

    CHECK (f.session.getPositionText() == "1:01:01");
    transport.setPosition (te::TimePosition::fromSeconds (2.75)); // beat 5.5: bar 2, beat 2, third step
    CHECK (f.session.getPositionText() == "2:02:03");

    // In 6/8 a bar is six eighths: the same moment is bar 2, its sixth beat.
    REQUIRE (f.session.getHistory()
                 .perform (make ("project.setTimeSignature", R"({"numerator": 6, "denominator": 8})"))
                 .wasOk());
    f.session.setMode (Session::Mode::song);
    transport.setPosition (te::TimePosition::fromSeconds (2.75));
    CHECK (f.session.getPositionText() == "2:06:01");

    CHECK (! f.session.isMetronomeOn());
    f.session.setMetronome (true);
    CHECK (f.session.isMetronomeOn());
    CHECK (f.session.getEdit().clickTrackEnabled.get());
}

TEST_CASE ("Loop regions: part of the pattern, part of the song, cleared with another pattern", "[session]")
{
    Fixture f;
    auto& transport = f.session.getEdit().getTransport();
    auto seconds = [&] (model::Ticks t)
    { return f.session.getEdit().tempoSequence.toTime (te::BeatPosition::fromBeats ((double)t / 960.0)); };

    // Pattern mode: the second beat to the end of the third.
    f.session.setLoopRegion (Session::Mode::pattern, Session::LoopRegion { 960, 2880 });
    f.session.play();
    CHECK (transport.looping.get());
    CHECK (transport.getLoopRange() == te::TimeRange (seconds (960), seconds (2880)));
    CHECK (transport.getPosition() == seconds (960)); // playing starts at the region
    f.session.stop();

    // Cleared: the whole pattern again.
    f.session.setLoopRegion (Session::Mode::pattern, std::nullopt);
    CHECK (transport.getLoopRange() == f.session.getAdapter().getPatternLoopRange());

    // An empty region counts as none.
    f.session.setLoopRegion (Session::Mode::pattern, Session::LoopRegion { 960, 960 });
    CHECK (! f.session.getLoopRegion (Session::Mode::pattern));

    // Song mode: loops the region, starting there when playback is outside it.
    f.session.setMode (Session::Mode::song);
    CHECK (! transport.looping.get());
    f.session.setLoopRegion (Session::Mode::song, Session::LoopRegion { 3840, 7680 });
    CHECK (! transport.looping.get()); // the transport loops only while it plays
    transport.setPosition (te::TimePosition());
    f.session.play();
    CHECK (transport.looping.get());
    CHECK (transport.getLoopRange() == te::TimeRange (seconds (3840), seconds (7680)));
    CHECK (transport.getPosition() >= seconds (3840));
    f.session.stop();
    f.session.setLoopRegion (Session::Mode::song, std::nullopt);
    CHECK (! transport.looping.get());

    // A pattern's region goes when another pattern is chosen.
    f.session.setMode (Session::Mode::pattern);
    f.session.setLoopRegion (Session::Mode::pattern, Session::LoopRegion { 0, 960 });
    REQUIRE (f.session.getHistory().perform (make ("pattern.add", R"({"name": "B"})")).wasOk());
    f.session.setCurrentPattern (f.session.getProject().patterns.back().id);
    CHECK (! f.session.getLoopRegion (Session::Mode::pattern));
}

TEST_CASE ("In 6/8 a quarter note still lasts a quarter note (Tracktion counts eighths there)", "[session]")
{
    Fixture f;
    f.session.getHistory().reset (model::Project()); // 120 BPM
    REQUIRE (f.session.getHistory()
                 .perform (make ("project.setTimeSignature", R"({"numerator": 6, "denominator": 8})"))
                 .wasOk());
    auto& edit = f.session.getEdit();
    CHECK (juce::approximatelyEqual (engine::ticksToTime (edit, 960.0).inSeconds(), 0.5));
    CHECK (juce::approximatelyEqual (engine::timeToTicks (edit, te::TimePosition::fromSeconds (1.5)), 2880.0));
    // The pattern loop is one 6/8 bar long: three quarter notes, 1.5 s.
    REQUIRE (f.session.getHistory().perform (make ("pattern.add", R"({"length": 2880})")).wasOk());
    f.session.setCurrentPattern (f.session.getProject().patterns.back().id);
    f.session.setMode (Session::Mode::pattern);
    CHECK (juce::approximatelyEqual (f.session.getAdapter().getPatternLoopRange().getEnd().inSeconds(), 1.5));
}

TEST_CASE ("The metronome's first beat of each bar sounds different", "[session][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine());
    REQUIRE (session.getHistory().perform (make ("pattern.add", R"({"length": 3840})")).wasOk());
    session.setMetronome (true);

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = true });
    runner.start();
    runner.runUntil (0.2);
    const auto start = runner.getSecondsProcessed();
    session.play();
    runner.runUntil (start + 2.3);
    session.stop();
    runner.stop();

    // The click at each beat (120 BPM: every half second), from where it starts.
    const auto sr = runner.getParams().sampleRate;
    const auto& out = runner.getChannel (0);
    auto clickAt = [&] (double beatSeconds)
    {
        auto from = (int64_t)((start + beatSeconds) * sr);
        while (from < (int64_t)out.size() && std::abs (out[(size_t)from]) < 0.01f)
            ++from;
        REQUIRE (from + 2000 < (int64_t)out.size());
        return std::vector<float> (out.begin() + from, out.begin() + from + 2000);
    };
    auto difference = [] (const std::vector<float>& a, const std::vector<float>& b)
    {
        double diff = 0.0, energy = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            diff += (a[i] - b[i]) * (a[i] - b[i]);
            energy += a[i] * a[i] + b[i] * b[i];
        }
        return energy > 0.0 ? diff / energy : 0.0;
    };
    auto crossings = [] (const std::vector<float>& a)
    {
        int n = 0;
        for (size_t i = 1; i < a.size(); ++i)
            n += (a[i - 1] < 0.0f) != (a[i] < 0.0f) ? 1 : 0;
        return n;
    };
    const auto first = clickAt (0.0), second = clickAt (0.5), third = clickAt (1.0), nextBar = clickAt (2.0);
    CHECK (difference (second, third) < 0.05);                // the same click on beats 2 and 3
    CHECK (difference (first, second) > 0.3);                 // another one on beat 1
    CHECK (crossings (first) > crossings (second) * 13 / 10); // and higher
    CHECK (difference (first, nextBar) < 0.05);
}

TEST_CASE ("The play head never loops over an empty range: a new project, channels added, play, stop, loop regions",
           "[session][audio]")
{
    // Tracktion's play head switches looping on before it takes the loop range, and its audio thread divides by
    // that range's length: looping switched on while the play head still had its empty range crashed Spinus
    // (SIGFPE on the audio thread, adding an instrument to a new project). The transport loops only while it
    // plays, so that switch is made by starting playback, which sets the range first.
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine());
    REQUIRE (session.getProject().patterns.empty()); // a new project: "No pattern"
    REQUIRE (session.getMode() == Session::Mode::pattern);
    auto& transport = session.getEdit().getTransport();

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 20.0, .paceInRealTime = true });
    runner.start();
    auto playHead = [&]() -> tracktion::graph::PlayHead&
    {
        auto* context = transport.getCurrentPlaybackContext();
        REQUIRE (context != nullptr);
        REQUIRE (context->getNodePlayHead() != nullptr);
        return *context->getNodePlayHead();
    };
    // Tracktion updates the play head's loop from the transport on a timer: give it time.
    auto settle = [&] { runner.runUntil (runner.getSecondsProcessed() + 0.8); };
    auto loopsSafely = [&]
    {
        auto& ph = playHead();
        return ! ph.isLooping() || ph.getLoopRange().getLength() > 0;
    };

    REQUIRE (session.getHistory().perform (make ("channel.add", R"({"name": "A"})")).wasOk());
    settle();
    CHECK (! transport.looping.get()); // stopped: nothing loops
    CHECK (! playHead().isLooping());

    // Playing pattern mode without a pattern loops one bar (120 BPM, 4/4: two seconds).
    session.play();
    CHECK (transport.looping.get());
    CHECK (transport.getLoopRange() == te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (2.0)));
    settle();
    CHECK (playHead().isLooping());
    CHECK (playHead().getLoopRange().getLength() > 0);
    session.stop();
    settle();
    CHECK (! transport.looping.get());
    CHECK (! playHead().isLooping());

    // Song mode: a loop region set and cleared while playing keeps the play head looping until playback stops.
    session.setMode (Session::Mode::song);
    session.play();
    CHECK (! transport.looping.get());
    session.setLoopRegion (Session::Mode::song, Session::LoopRegion { 0, 3840 });
    CHECK (transport.looping.get());
    settle();
    CHECK (loopsSafely());
    session.setLoopRegion (Session::Mode::song, std::nullopt);
    CHECK (transport.looping.get()); // over the rest of the song
    CHECK (transport.getLoopRange().getLength() > te::TimeDuration::fromSeconds (3600.0));
    settle();
    CHECK (loopsSafely());
    session.setLoopRegion (Session::Mode::song, Session::LoopRegion { 0, 3840 });
    settle();
    CHECK (loopsSafely());
    session.stop();
    settle();
    CHECK (! transport.looping.get());
    CHECK (loopsSafely());
    runner.stop();
}
