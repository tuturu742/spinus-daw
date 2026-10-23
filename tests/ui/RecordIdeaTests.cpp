#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "engine/EngineHost.h"
#include "ui/RecordIdea.h"

using namespace spinus;
using namespace spinus::model;

namespace
{
constexpr double rate = 48000.0;

struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-idea-tests") + "-" + juce::Uuid().toString());
};

bool waitFor (const std::function<bool()>& done, int ms = 30000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
    while (! done() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    return done();
}

juce::File writeTake (const juce::File& file, const std::vector<float>& x)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    const float* channels[] = { x.data() };
    writer->writeFromFloatArrays (channels, 1, (int)x.size());
    return file;
}

/** A voice-like tone (a band-limited sawtooth) added into x from `start` seconds. */
void sing (std::vector<float>& x, double start, double seconds, int note)
{
    const auto hz = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    const auto first = (size_t)(start * rate), count = (size_t)(seconds * rate);
    for (size_t i = 0; i < count && first + i < x.size(); ++i)
    {
        const auto t = (double)i / rate;
        double v = 0.0;
        for (int h = 1; h < 12; ++h)
            v += std::sin (2.0 * M_PI * h * hz * t) / h;
        const auto fade = std::min (1.0, std::min (t, seconds - t) / 0.01);
        x[first + i] += (float)(0.2 * v * fade);
    }
}

struct Fixture
{
    Fixture()
        : host ({ .openAudioDevice = false }),
          session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine())),
          service (t.root, t.root.getChildFile ("models"))
    {
        Project p;
        p.settings.tempoBpm = 120.0;
        Channel lead;
        lead.id = p.allocateId<ChannelId>();
        lead.name = "Lead";
        p.channels = { lead };
        Pattern pattern;
        pattern.id = p.allocateId<PatternId>();
        pattern.length = 4 * ticksPerQuarterNote;
        p.patterns = { pattern };
        session.getHistory().reset (p);
        session.setSelectedChannel (lead.id);
        session.setCurrentPattern (pattern.id);
        service.setEnabled (true);
    }

    TempFolder t;
    EngineHost host;
    Session session;
    ai::AiService service;
};
} // namespace

TEST_CASE ("Record idea: a hummed take comes back as a suggestion for the selected channel", "[ui][ai][idea]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    Fixture f;
    ui::RecordIdea idea (f.session, f.service);

    // The pattern starts 0.5 s into the take; C4, E4, G4 on beats 1, 2 and 3 (sung a little late).
    std::vector<float> x ((size_t)(3.0 * rate), 0.0f);
    sing (x, 0.52, 0.4, 60);
    sing (x, 1.02, 0.4, 64);
    sing (x, 1.53, 0.4, 67);
    const auto take = writeTake (f.t.root.getChildFile ("take.wav"), x);

    idea.analyse (ui::RecordIdea::Kind::melody, { take, 0.5, 2.0, 120.0 });
    auto& suggestions = f.session.getSuggestions();
    REQUIRE (waitFor ([&] { return suggestions.isShowing() || ! idea.isBusy(); }));
    INFO (suggestions.getWorkingMessage());
    REQUIRE (suggestions.isShowing());

    const auto& notes = suggestions.getCurrent()->notes.at (f.session.getSelectedChannel());
    REQUIRE (notes.size() == 3);
    CHECK ((notes[0].pitch == 60 && notes[0].start == 0));
    CHECK ((notes[1].pitch == 64 && notes[1].start == 960));
    CHECK ((notes[2].pitch == 67 && notes[2].start == 1920));
    CHECK (suggestions.getCurrent()->title.startsWith ("Your melody (3 notes)"));
    CHECK (! take.existsAsFile()); // the take is not kept

    // Accepting writes them.
    REQUIRE (suggestions.accept().wasOk());
    CHECK (f.session.getProject().patterns[0].notes.at (f.session.getSelectedChannel()).size() == 3);
}

TEST_CASE ("Record idea says what it needs", "[ui][ai][idea]")
{
    Fixture f;
    f.service.setEnabled (false);
    ui::RecordIdea idea (f.session, f.service);
    CHECK (idea.record (ui::RecordIdea::Kind::melody).getErrorMessage().contains ("Turn AI on"));

    f.service.setEnabled (true);
    CHECK (idea.record (ui::RecordIdea::Kind::melody).getErrorMessage().contains ("no audio input"));
    CHECK (! idea.isBusy());
}
