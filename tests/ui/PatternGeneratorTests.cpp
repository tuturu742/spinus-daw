#include <catch2/catch_test_macros.hpp>

#include "FakeProvider.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/AiSettingsDialog.h"
#include "ui/PatternGenerator.h"

using namespace spinus;
using namespace spinus::model;
using spinus::test::FakeProvider;
using spinus::test::waitFor;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-gen-tests") + "-" + juce::Uuid().toString());
};

/** Drums (a Drum Machine, kick on 1 and 3) and an empty Bass, in C minor. */
struct Song
{
    Song()
        : session (host.getEngine(),
                   [this]
                   {
                       auto d = std::make_unique<engine::DefaultDeviceFactory>();
                       engine::registerSpinusDevices (*d, host.getEngine());
                       return d;
                   }())
    {
        Project p;
        p.settings.tempoBpm = 96.0;
        p.settings.key = Key { 0, "minor" };
        Channel drumChannel, bassChannel;
        drumChannel.id = drums = p.allocateId<ChannelId>();
        drumChannel.name = "Drums";
        drumChannel.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.drum-machine" };
        bassChannel.id = bass = p.allocateId<ChannelId>();
        bassChannel.name = "Bass";
        bassChannel.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.3x-osc" };
        p.channels = { drumChannel, bassChannel };
        Pattern pattern;
        pattern.id = p.allocateId<PatternId>();
        pattern.length = 4 * ticksPerQuarterNote;
        pattern.notes[drums] = { Note { 0, 240, 60, 1.0f }, Note { 1920, 240, 60, 1.0f } };
        p.patterns = { pattern };
        session.getHistory().reset (p);
        session.setCurrentPattern (pattern.id);
        session.setSelectedChannel (bass);
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session;
    ChannelId drums, bass;
};
} // namespace

TEST_CASE ("The pattern generator is told the song, the channels to write and the ones to fit with",
           "[ui][ai][generator]")
{
    TempFolder t;
    Song song;
    ai::AiService service (t.root, t.root.getChildFile ("models"));
    ui::PatternGenerator generator (song.session, service);

    ui::PatternGenerator::Request request;
    request.text = "a funky bass line";
    request.channels = { song.bass };
    request.variations = 4;
    request.swing = 0.5f;
    const auto params = generator.describe (request);

    CHECK (params["request"] == "a funky bass line");
    CHECK (juce::approximatelyEqual ((double)params["tempo"], 96.0));
    CHECK ((int)params["lengthTicks"] == 3840);
    request.bars = 4; // longer than the one-bar pattern
    CHECK ((int)generator.describe (request)["lengthTicks"] == 4 * 3840);
    request.bars = 0;
    CHECK ((int)params["key"]["tonic"] == 0);
    CHECK (params["key"]["scale"] == "minor");
    CHECK ((int)params["variations"] == 4);
    REQUIRE (params["targets"].size() == 1);
    CHECK ((int)params["targets"][0]["channel"] == (int)song.bass.value);
    CHECK (params["targets"][0]["instrument"] == "3x Osc");
    REQUIRE (params["context"].size() == 1);
    CHECK (params["context"][0]["name"] == "Drums");
    CHECK (params["context"][0]["notes"].size() == 2);
    CHECK (params["context"][0]["pads"][0]["name"] == "Kick"); // read from the Drum Machine itself
    CHECK ((int)params["context"][0]["pads"][0]["note"] == 60);

    // Without AI, or without channels: it says so.
    CHECK (generator.generate (request).getErrorMessage().contains ("turn it on"));
    service.setEnabled (true);
    request.channels.clear();
    CHECK (generator.generate (request).getErrorMessage().contains ("at least one channel"));
}

TEST_CASE ("The pattern generator's notes come back as a suggestion, and Regenerate asks again", "[ui][ai][generator]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    Song song;
    // The fake model writes a bass line (one note out of C minor), then another one when asked again.
    const auto replies = t.root.getChildFile ("replies.json");
    REQUIRE (replies.replaceWithText (juce::String (R"([
        {"tool": ["write_pattern", {"title": "Root notes", "parts": [{"channel": BASS, "notes": [
            {"step": 0, "length": 4, "note": "C2"}, {"step": 8, "length": 4, "note": "E2"}]}]}]},
        {"tool": ["write_pattern", {"title": "Octaves", "parts": [{"channel": BASS, "notes": [
            {"step": 0, "length": 2, "note": "C2"}, {"step": 2, "length": 2, "note": "C3"}]}]}]}
    ])")
                                          .replace ("BASS", juce::String (song.bass.value))));
    FakeProvider provider (replies);
    REQUIRE (provider.ready);
    ai::AiService service (t.root, t.root.getChildFile ("models"));
    ui::AiSettingsDialog settings (service);
    settings.setAiOn (true);
    settings.addConnection ("local", "My local model", {}, provider.url());
    REQUIRE (waitFor (
        [&]
        {
            return ! settings.isWaiting() && ! settings.getConnections().empty() &&
                   ! settings.getConnections()[0].models.isEmpty();
        }));

    // Its own model in Options › AI (otherwise it uses the assistant's).
    CHECK (settings.getPatternsRoute().isEmpty());
    settings.useModel (0, "fake-model", "patterns");
    REQUIRE (waitFor ([&] { return settings.getPatternsRoute() == "My local model: fake-model"; }));

    ui::PatternGenerator generator (song.session, service);
    ui::PatternGenerator::Request request;
    request.text = "a bass line";
    request.channels = { song.bass };
    REQUIRE (generator.generate (request).wasOk());
    auto& suggestions = song.session.getSuggestions();
    CHECK (suggestions.getWorkingMessage().isNotEmpty());
    REQUIRE (waitFor ([&] { return suggestions.isShowing() || ! generator.isBusy(); }));
    INFO (suggestions.getWorkingMessage());
    REQUIRE (suggestions.isShowing());

    CHECK (suggestions.getCurrent()->title == "Root notes");
    const auto& notes = suggestions.getCurrent()->notes.at (song.bass);
    REQUIRE (notes.size() == 2);
    CHECK ((notes[0].pitch == 36 && notes[0].start == 0 && notes[0].length == 960));
    CHECK (notes[1].pitch == 39); // E2 is not in C minor: E-flat
    CHECK (notes[1].start == 1920);

    REQUIRE (suggestions.regenerate());
    REQUIRE (
        waitFor ([&] { return suggestions.getCurrent() != nullptr && suggestions.getCurrent()->title == "Octaves"; }));
    REQUIRE (suggestions.accept().wasOk());
    CHECK (song.session.getProject().patterns[0].notes.at (song.bass).size() == 2);
    CHECK (song.session.getProject().patterns[0].notes.at (song.drums).size() == 2); // the drums stay
}

#include "ui/AssistantPanel.h"

TEST_CASE ("Asked for a bass line, the assistant has the pattern generator write it, as a suggestion",
           "[ui][ai][generator]")
{
    if (ai::AiService::findInstallation().python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    Song song;
    const auto pattern = song.session.getCurrentPattern();
    // The assistant calls write_notes; the generator's model writes; the assistant says so.
    const auto replies = t.root.getChildFile ("replies.json");
    REQUIRE (replies.replaceWithText (juce::String (R"([
        {"tool": ["write_notes", {"request": "a bass line", "pattern": PATTERN, "channels": [BASS]}]},
        {"tool": ["write_pattern", {"title": "Steady", "parts": [{"channel": BASS, "notes": [
            {"step": 0, "length": 8, "note": "C2"}]}]}]},
        {"text": "I wrote a steady bass line: it is in the Piano Roll."}
    ])")
                                          .replace ("PATTERN", juce::String (pattern.value))
                                          .replace ("BASS", juce::String (song.bass.value))));
    FakeProvider provider (replies);
    REQUIRE (provider.ready);

    ai::AiService service (t.root, t.root.getChildFile ("models"));
    ui::AiSettingsDialog settings (service);
    settings.setAiOn (true);
    settings.addConnection ("local", "My local model", {}, provider.url());
    REQUIRE (waitFor (
        [&]
        {
            return ! settings.isWaiting() && ! settings.getConnections().empty() &&
                   ! settings.getConnections()[0].models.isEmpty();
        }));

    song.session.setSelectedChannel (song.drums); // the assistant chooses the channel itself
    ui::AssistantPanel panel (song.session, service);
    panel.setSize (300, 600);
    panel.ask ("Write me a bass line");
    REQUIRE (waitFor ([&] { return ! panel.isAnswering(); }));

    auto& suggestions = song.session.getSuggestions();
    REQUIRE (suggestions.isShowing());
    CHECK (suggestions.getCurrent()->title == "Steady");
    CHECK (suggestions.getCurrent()->notes.at (song.bass).front().pitch == 36);
    CHECK (song.session.getSelectedChannel() == song.bass);                     // the Piano Roll shows it
    CHECK (song.session.getProject().patterns[0].notes.count (song.bass) == 0); // nothing written yet
    CHECK (panel.getEntries().back().text.contains ("steady bass line"));
}
