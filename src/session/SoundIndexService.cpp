#include "SoundIndexService.h"
#include "ProgramFolder.h"
#include "plugins/HostLibraries.h"

namespace spinus
{

SoundIndexService::SoundIndexService (const plugins::DeviceList& list) : devices (list)
{
    options.helper = getHelperProgram ("spinus-plugin-scanner");
    options.hostLibraries = plugins::getHostLibraries();
}

SoundIndexService::~SoundIndexService()
{
    alive = nullptr;
    waiting.clear();
    indexer = nullptr; // stops the update (its helper is killed)
}

void SoundIndexService::setFolder (const juce::File& f)
{
    folder = f;
    sendChangeMessage();
}

//==============================================================================
std::vector<SoundSource> SoundIndexService::getAllSources() const
{
    return getSources ? getSources() : findSoundSources (devices);
}

std::vector<SoundSource> SoundIndexService::getIndexedSources() const
{
    const auto included = getIncludedPlugins();
    auto sources = getAllSources();
    std::erase_if (sources,
                   [&] (const SoundSource& s) { return ! isAlwaysMeasured (s) && ! included.contains (s.key); });
    return sources;
}

juce::StringArray SoundIndexService::getIncludedPlugins() const
{
    juce::StringArray keys;
    // Kept in a variable: the array lives in the parsed file, which must outlive the loop.
    const auto file = juce::JSON::parse (folder.getChildFile ("plugins.json"));
    if (const auto* saved = file["include"].getArray())
        for (const auto& k : *saved)
            keys.add (k.toString());
    return keys;
}

void SoundIndexService::setIncludedPlugins (const juce::StringArray& keys)
{
    juce::Array<juce::var> chosen;
    for (const auto& k : keys)
        chosen.add (k);
    auto* o = new juce::DynamicObject();
    o->setProperty ("include", chosen);
    folder.createDirectory();
    folder.getChildFile ("plugins.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n");
    sendChangeMessage();
}

void SoundIndexService::setIncluded (const juce::String& key, bool include)
{
    auto keys = getIncludedPlugins();
    if (include == keys.contains (key))
        return;
    if (include)
        keys.add (key);
    else
        keys.removeString (key);
    setIncludedPlugins (keys);
}

//==============================================================================
void SoundIndexService::update()
{
    for (const auto& r : waiting)
        if (r.what == "update")
            return; // one is enough
    auto sources = getIndexedSources();
    auto sampleFolders = getSampleFolders ? getSampleFolders() : juce::Array<juce::File>();
    enqueue ({ "update", [o = options, sources = std::move (sources), sampleFolders = std::move (sampleFolders)] (
                             SoundIndex& index, const std::function<bool()>& stop,
                             const std::function<void (const SoundIndexer::Progress&)>& report)
               { return SoundIndexer::update (index, o, sources, sampleFolders, stop, report); } });
}

bool SoundIndexService::refreshInstrument (const juce::String& key)
{
    std::optional<SoundSource> source;
    for (auto& s : getAllSources())
        if (s.key == key)
            source = s;
    if (! source)
        return false;
    if (! isAlwaysMeasured (source.value()))
        setIncluded (key, true);
    enqueue ({ source.value().name,
               [o = options, s = source.value()] (SoundIndex& index, const std::function<bool()>& stop,
                                                  const std::function<void (const SoundIndexer::Progress&)>& report)
               {
                   SoundIndexer::Progress p;
                   p.stage = "instruments";
                   p.total = 1;
                   p.current = s.name;
                   report (p);
                   index.measureAgain (s.key);
                   return SoundIndexer::updateInstrument (index, o, s, stop);
               } });
    return true;
}

void SoundIndexService::refreshSamples (const juce::Array<juce::File>& filesOrFolders)
{
    if (filesOrFolders.isEmpty())
        return;
    enqueue ({ filesOrFolders.size() == 1 ? filesOrFolders.getFirst().getFileName()
                                          : juce::String (filesOrFolders.size()) + " sample folders",
               [o = options, filesOrFolders] (SoundIndex& index, const std::function<bool()>& stop,
                                              const std::function<void (const SoundIndexer::Progress&)>& report)
               { return SoundIndexer::updateSamples (index, o, filesOrFolders, stop, report); } });
}

int SoundIndexService::measureAgain (const juce::String& key)
{
    if (isUpdating())
        return 0;
    SoundIndex index (folder);
    index.load();
    juce::StringArray keys;
    if (key.isNotEmpty())
    {
        if (index.instruments.count (key) > 0)
            keys.add (key);
    }
    else
        for (const auto& [k, instrument] : index.instruments)
            if (instrument.silentOrFailed())
                keys.add (k);
    if (keys.isEmpty())
        return 0;

    // One of the user's plugins is measured only when ticked: ticked now, so the update reaches it.
    auto included = getIncludedPlugins();
    for (const auto& k : keys)
    {
        index.measureAgain (k);
        if (! isAlwaysMeasured (index.instruments[k].source))
            included.addIfNotAlreadyThere (k);
    }
    setIncludedPlugins (included);
    update();
    return keys.size();
}

bool SoundIndexService::forget (const juce::String& key)
{
    if (isUpdating())
        return false;
    SoundIndex index (folder);
    index.load();
    setIncluded (key, false);
    const bool forgotten = index.forget (key);
    sendChangeMessage();
    return forgotten;
}

bool SoundIndexService::deleteIndex()
{
    if (isUpdating() || SoundIndexer::isAnyRunning())
        return false;
    SoundIndex index (folder);
    const bool done = index.removeAll();
    lastResult.reset();
    lastText.clear();
    sendChangeMessage();
    return done;
}

//==============================================================================
void SoundIndexService::enqueue (Request r)
{
    waiting.push_back (std::move (r));
    if (! isUpdating())
        startNext();
    else
        sendChangeMessage();
}

void SoundIndexService::startNext()
{
    while (! waiting.empty() && indexer == nullptr)
    {
        auto next = std::move (waiting.front());
        waiting.pop_front();
        running = next.what;
        progress = {};
        indexer = std::make_unique<SoundIndexer> (folder, options);
        const bool started = indexer->start (
            std::move (next.job),
            [this] (const SoundIndexer::Progress& p)
            {
                progress = p;
                sendChangeMessage();
            },
            [this] (const SoundIndexer::Result& r)
            {
                lastResult = r;
                const auto measured = r.soundsMeasured + r.samplesMeasured,
                           removed = r.soundsRemoved + r.samplesRemoved;
                lastText = r.cancelled
                               ? juce::String ("Stopped; what was measured is kept, Update carries on from "
                                               "there.")
                               : (running == "update" ? juce::String ("Updated: ") : running + ": ") +
                                     juce::String (measured) + (measured == 1 ? " sound" : " sounds") + " measured" +
                                     (removed > 0 ? ", " + juce::String (removed) + " gone" : juce::String()) + ".";
                // Deleted on the next message: this is called from inside it.
                juce::MessageManager::callAsync (
                    [this, done = std::shared_ptr<SoundIndexer> (std::move (indexer)),
                     still = std::weak_ptr<bool> (alive)]
                    {
                        juce::ignoreUnused (done);
                        if (still.expired())
                            return;
                        startNext();
                        sendChangeMessage();
                    });
                sendChangeMessage();
            });
        if (! started)
        {
            indexer = nullptr;
            lastText = "The sound index could not be updated now.";
        }
        sendChangeMessage();
    }
}

void SoundIndexService::cancel()
{
    waiting.clear();
    if (indexer != nullptr)
        indexer->cancel(); // reports the stop through its finished callback
    sendChangeMessage();
}

SoundIndexer::Progress SoundIndexService::getProgress() const { return progress; }

juce::String SoundIndexService::getStatusText() const
{
    auto count = [] (int n, const char* one, const char* many)
    { return juce::String (n) + " " + (n == 1 ? one : many); };
    if (isUpdating())
    {
        const auto& p = progress;
        auto text = running == "update" ? juce::String ("Updating the sound index") : "Measuring " + running + " again";
        if (p.stage.isNotEmpty())
            text << ": " << p.stage << " " << juce::jmin (p.done + 1, p.total) << " of " << p.total
                 << (p.current.isNotEmpty() ? " (" + p.current + ")" : juce::String()) << ", " << p.sounds
                 << " measured so far";
        if (! waiting.empty())
            text << "; then " << count ((int)waiting.size(), "more", "more");
        return text + "...";
    }
    const auto s = SoundIndex::readSummary (folder);
    juce::String text;
    if (s.updated == juce::Time())
        text = "No sound index yet. Update sound index measures the presets of your instruments and the samples in "
               "your Browser's sample folders, so Spinus can choose the closest sounds you have.";
    else
        text = "Sound index: " + count (s.instruments, "instrument", "instruments") + ", " +
               count (s.presets, "preset", "presets") + ", " + count (s.samples, "sample", "samples") + "; updated " +
               s.updated.formatted ("%d %b %Y %H:%M") +
               (s.problems > 0 ? " (" + count (s.problems, "problem", "problems") + ")" : juce::String()) + ".";
    return lastText.isNotEmpty() ? lastText + " " + text : text;
}

} // namespace spinus
