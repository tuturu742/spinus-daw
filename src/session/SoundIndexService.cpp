#include "SoundIndexService.h"
#include "PartSounds.h"
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
    listening = nullptr; // stops playing a preset (its helper is killed)
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

LeftOutSounds::Entry SoundIndexService::entryOf (const SoundIndex::Row& row)
{
    LeftOutSounds::Entry e;
    e.name = row.name;
    e.source = row.source;
    if (row.sample)
        e.sample = row.path;
    else if (! row.whole)
    {
        e.instrument = row.instrument;
        e.preset = row.id;
    }
    return e;
}

int SoundIndexService::leaveOut (const std::vector<LeftOutSounds::Entry>& sounds)
{
    if (isUpdating() || sounds.empty())
        return 0;
    LeftOutSounds leftOut (folder);
    leftOut.load();
    for (const auto& e : sounds)
        if (e.isSample() || e.instrument.isNotEmpty())
            leftOut.add (e);
    leftOut.save();
    SoundIndex index (folder);
    index.load();
    const auto dropped = index.dropLeftOut (leftOut);
    sendChangeMessage();
    return dropped;
}

int SoundIndexService::putBack (const std::vector<LeftOutSounds::Entry>& sounds)
{
    LeftOutSounds leftOut (folder);
    leftOut.load();
    int count = 0;
    for (const auto& e : sounds)
        count += leftOut.remove (e) ? 1 : 0;
    if (count == 0)
        return 0;
    leftOut.save();
    SoundIndex index (folder);
    index.load();
    for (const auto& e : sounds)
        if (! e.isSample())
            index.visitAgain (e.instrument);
    update();
    return count;
}

namespace
{
/** A thread that runs `work`, then `done` on the message thread unless the service has gone. */
class ListenThread final : public juce::Thread
{
public:
    using Work = std::function<juce::String (const std::function<bool()>& shouldStop)>;
    ListenThread (Work w, std::function<void (const juce::String&)> d, std::weak_ptr<bool> a)
        : juce::Thread ("Spinus sound index listen"), work (std::move (w)), done (std::move (d)), alive (std::move (a))
    {
    }
    ~ListenThread() override { stopThread (30000); }

    void run() override
    {
        auto problem = work ([this] { return threadShouldExit(); });
        juce::MessageManager::callAsync (
            [problem, d = done, a = alive]
            {
                if (! a.expired())
                    d (problem);
            });
    }

private:
    Work work;
    std::function<void (const juce::String&)> done;
    std::weak_ptr<bool> alive;
};

juce::var note (int pitch, double start, double length, double velocity = 0.8)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("note", pitch);
    o->setProperty ("start", start);
    o->setProperty ("length", length);
    o->setProperty ("velocity", velocity);
    return juce::var (o);
}
} // namespace

void SoundIndexService::listen (const SoundIndex::Row& row, const std::function<void (const juce::String&)>& done)
{
    auto finish = [this, done] (const juce::File& file, const juce::String& problem)
    {
        if (problem.isEmpty() && play)
            play (file);
        if (done)
            done (problem);
    };
    if (row.sample)
        return finish (juce::File (row.path),
                       juce::File (row.path).existsAsFile() ? juce::String() : "the file is gone");
    if (row.whole || row.role == "problem")
        return finish ({}, "it was never measured, so there is nothing to play");
    if (listening != nullptr)
        return finish ({}, "another sound is still being played");
    SoundIndex index (folder);
    index.load();
    const auto found = index.instruments.find (row.instrument);
    if (found == index.instruments.end())
        return finish ({}, "its instrument is no longer in the index");
    const auto source = found->second.source;
    bool pitched = true;
    for (const auto& s : found->second.sounds)
        if (s["id"].toString() == row.id)
            pitched = (bool)s["tracksPitch"];
    const auto output = folder.getChildFile ("listen").getChildFile (
        juce::String::toHexString ((row.instrument + "\n" + row.id).hashCode64()) + ".wav");
    if (output.existsAsFile())
        return finish (output, {});

    juce::Array<juce::var> notes;
    if (pitched)
    {
        notes.add (note (48, 0.0, 1.0)); // C3 held
        for (const auto n : { 60, 64, 67 })
            notes.add (note (n, 1.3, 1.0)); // the chord C4 E4 G4
    }
    else
        for (int step = 0; step < 16; ++step) // two bars of eighths at 120 BPM: kick, snare, hats
        {
            const auto at = step * 0.25;
            notes.add (note (42, at, 0.2, step % 2 == 0 ? 0.7 : 0.5));
            if (step % 4 == 0)
                notes.add (note (36, at, 0.2, 0.9));
            if (step % 4 == 2)
                notes.add (note (38, at, 0.2, 0.9));
        }
    auto* item = new juce::DynamicObject();
    item->setProperty ("id", "listen");
    item->setProperty ("preset", row.id);
    item->setProperty ("notes", notes);
    item->setProperty ("tail", 1.0);
    item->setProperty ("output", output.getFullPathName());
    output.getParentDirectory().createDirectory();
    listening = std::make_unique<ListenThread> (
        [options = options, source, items = juce::Array<juce::var> { juce::var (item) },
         output] (const std::function<bool()>& shouldStop)
        {
            juce::String problem;
            const auto rendered = parts::renderInHelper (options, source, items, shouldStop, problem);
            if (! rendered.isEmpty() && rendered[0].hasProperty ("problem"))
                problem = rendered[0]["problem"].toString();
            return output.existsAsFile() ? juce::String() : (problem.isNotEmpty() ? problem : "it made no sound");
        },
        [this, finish, output] (const juce::String& problem)
        {
            listening = nullptr;
            finish (output, problem);
        },
        alive);
    listening->startThread();
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
