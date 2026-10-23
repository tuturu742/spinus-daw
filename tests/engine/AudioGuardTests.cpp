#include <catch2/catch_test_macros.hpp>

#include "../model/ExampleProject.h"
#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "TestDevices.h"
#include "engine/AudioGuardPlugin.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"

using namespace spinus;
using namespace spinus::model;
namespace te = tracktion;

namespace
{

/** A deliberately broken effect: writes NaN and infinity into its output. */
class BrokenPlugin final : public te::Plugin
{
public:
    explicit BrokenPlugin (te::PluginCreationInfo info) : te::Plugin (std::move (info)) {}
    ~BrokenPlugin() override { notifyListenersOfDeletion(); }

    static const char* xmlTypeName;
    static const char* getPluginName() { return "Broken"; }
    static juce::ValueTree create()
    {
        juce::ValueTree v (te::IDs::PLUGIN);
        v.setProperty (te::IDs::type, xmlTypeName, nullptr);
        return v;
    }

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    int getNumOutputChannelsGivenInputs (int n) override { return n; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    void initialise (const te::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || fc.bufferNumSamples < 2)
            return;
        fc.destBuffer->setSample (0, fc.bufferStartSample, std::numeric_limits<float>::quiet_NaN());
        fc.destBuffer->setSample (0, fc.bufferStartSample + 1, std::numeric_limits<float>::infinity());
    }
};

const char* BrokenPlugin::xmlTypeName = "spinusTestBroken";

} // namespace

//==============================================================================
TEST_CASE ("The invalid-sample check finds NaN and infinity and nothing else", "[engine][guard]")
{
    juce::AudioBuffer<float> buffer (2, 64);
    buffer.clear();
    for (int i = 0; i < 64; ++i)
        buffer.setSample (1, i, 0.5f * std::sin ((float)i));
    buffer.setSample (0, 3, 1.0e-40f); // a denormal is fine
    buffer.setSample (0, 4, -3.0e38f); // so is a huge finite value
    CHECK_FALSE (engine::AudioGuardPlugin::containsInvalidSamples (buffer, 0, 64));

    for (auto bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity() })
    {
        auto copy = buffer;
        copy.setSample (1, 63, bad);
        CHECK (engine::AudioGuardPlugin::containsInvalidSamples (copy, 0, 64));
        CHECK_FALSE (engine::AudioGuardPlugin::containsInvalidSamples (copy, 0, 63)); // outside the range
    }
}

TEST_CASE ("A device that outputs NaN is silenced and reported; the rest of the mix plays", "[engine][guard]")
{
    EngineHost host ({ .openAudioDevice = false });
    test::TestDevices devices (host.getEngine());
    host.getEngine().getPluginManager().createBuiltInType<BrokenPlugin>();
    devices.registerStockDevice ("test.broken", [] (te::Edit& edit, const Device&)
                                 { return edit.getPluginCache().createNewPlugin (BrokenPlugin::xmlTypeName, {}); });

    Project p;
    Insert bad;
    bad.id = p.allocateId<InsertId>();
    const DeviceId brokenId = p.allocateId<DeviceId>();
    bad.effects.push_back ({ Device { brokenId, Device::Kind::stock, "test.broken", {}, false }, 1.0f });
    p.mixer.inserts = { bad };

    Channel broken;
    broken.id = p.allocateId<ChannelId>();
    broken.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "test.tone", {}, false };
    broken.targetInsert = bad.id;

    Channel fine;
    fine.id = p.allocateId<ChannelId>();
    fine.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "test.tone", {}, false };
    p.channels = { broken, fine };

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = false });
    auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
    engine::EngineAdapter adapter (*edit, devices);
    adapter.sync (p);

    // A guard sits right after every device, and a probe (also an AudioGuardPlugin) right before it.
    auto& list = adapter.getInsertTrack (bad.id)->pluginList;
    CHECK (dynamic_cast<engine::AudioGuardPlugin*> (list[0]) != nullptr);
    CHECK (dynamic_cast<BrokenPlugin*> (list[1]) != nullptr);
    CHECK (dynamic_cast<engine::AudioGuardPlugin*> (list[2]) != nullptr);
    auto& toneList = adapter.getChannelTrack (fine.id)->pluginList;
    CHECK (dynamic_cast<engine::AudioGuardPlugin*> (toneList[0]) != nullptr);
    CHECK (dynamic_cast<engine::AudioGuardPlugin*> (toneList[2]) != nullptr);

    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();
    edit->getTransport().play (false);
    runner.start();
    runner.runUntil (2.0);
    runner.stop();
    edit->getTransport().stop (false, true);

    const auto& left = runner.getChannel (0);
    const auto n = runner.getNumRecordedSamples();
    CHECK (std::all_of (left.begin(), left.begin() + n, [] (float v) { return std::isfinite (v); }));

    // The healthy channel is still heard; only the broken device was silenced.
    const auto sr = runner.getParams().sampleRate;
    CHECK (test::checks::rms (left, (int64_t)(0.5 * sr), (int64_t)(1.5 * sr)) > 0.05f);
    CHECK (adapter.takeSilencedDevices() == std::vector<DeviceId> { brokenId });
    CHECK (adapter.takeSilencedDevices().empty()); // reported once, counters reset
}

TEST_CASE ("Devices whose plugin is missing are listed", "[engine][guard]")
{
    EngineHost host ({ .openAudioDevice = false });
    test::TestDevices devices (host.getEngine());
    auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
    engine::EngineAdapter adapter (*edit, devices);
    adapter.sync (test::makeExample());

    // The example's Dragonfly reverb (device 5) is a third-party plugin not installed in tests.
    CHECK (adapter.getMissingDevices() == std::vector<DeviceId> { DeviceId { 5 } });
}

TEST_CASE ("Audio guard cost", "[.benchmark]")
{
    // Run with: spinus_tests "[.benchmark]". Reported in the plugin-guard PR (real-time checklist).
    juce::AudioBuffer<float> buffer (2, 128);
    for (int i = 0; i < 128; ++i)
    {
        buffer.setSample (0, i, std::sin ((float)i));
        buffer.setSample (1, i, std::cos ((float)i));
    }

    constexpr int runs = 1000000;
    int found = 0;
    const auto start = juce::Time::getHighResolutionTicks();
    for (int r = 0; r < runs; ++r)
        found += engine::AudioGuardPlugin::containsInvalidSamples (buffer, 0, 128) ? 1 : 0;
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);

    std::cout << "audio guard: " << seconds / runs * 1.0e9 << " ns per stereo 128-sample block\n";
    CHECK (found == 0);
}
