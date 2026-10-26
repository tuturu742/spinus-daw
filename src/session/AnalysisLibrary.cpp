#include "AnalysisLibrary.h"

#include "SongPreparation.h"
#include "dsp/sampler/SampleStorage.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace spinus
{

namespace
{
constexpr const char* entryFile = "song.json";
constexpr const char* stemsListFile = "stems.json"; // written last: the stems are all there

juce::int64 sizeOf (const juce::File& folder)
{
    juce::int64 total = 0;
    if (folder.isDirectory())
        for (const auto& file : folder.findChildFiles (juce::File::findFiles, true))
            total += file.getSize();
    return total;
}

juce::Result encodeFlac (const juce::File& source, const juce::File& target, const std::function<bool()>& shouldStop)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (source));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return juce::Result::fail ("Spinus cannot read " + source.getFullPathName());
    const auto partial = target.withFileExtension ("partial");
    partial.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (partial);
        auto writer = juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                           .withSampleRate (reader->sampleRate)
                                                                           .withNumChannels ((int)reader->numChannels)
                                                                           .withBitsPerSample (24));
        if (writer == nullptr)
            return juce::Result::fail ("Spinus cannot write " + partial.getFullPathName());
        juce::AudioBuffer<float> buffer ((int)reader->numChannels, 65536);
        for (juce::int64 at = 0; at < reader->lengthInSamples; at += buffer.getNumSamples())
        {
            if (shouldStop())
            {
                writer = nullptr;
                partial.deleteFile();
                return juce::Result::fail ("Stopped.");
            }
            const auto count = (int)std::min<juce::int64> (buffer.getNumSamples(), reader->lengthInSamples - at);
            reader->read (&buffer, 0, count, at, true, true);
            writer->writeFromAudioSampleBuffer (buffer, 0, count);
        }
    }
    if (! partial.moveFileTo (target))
        return juce::Result::fail ("Spinus cannot write " + target.getFullPathName());
    return juce::Result::ok();
}
} // namespace

AnalysisLibrary AnalysisLibrary::inAiFolder (const juce::File& aiFolder)
{
    return AnalysisLibrary (aiFolder == juce::File() ? juce::File() : aiFolder.getChildFile ("library"));
}

juce::var AnalysisLibrary::read (const juce::String& digest) const
{
    if (! isAvailable() || digest.isEmpty())
        return {};
    const auto file = songFolder (digest).getChildFile (entryFile);
    if (! file.existsAsFile())
        return {};
    auto entry = juce::JSON::parse (file);
    return entry.isObject() && (int)entry["version"] == version ? entry : juce::var();
}

bool AnalysisLibrary::update (const juce::String& digest, const juce::String& songName,
                              const std::function<void (juce::DynamicObject&)>& change)
{
    if (! isAvailable() || digest.isEmpty())
        return false;
    auto entry = read (digest);
    if (! entry.isObject())
        entry = juce::var (new juce::DynamicObject());
    auto* o = entry.getDynamicObject();
    o->setProperty ("version", version);
    o->setProperty ("digest", digest);
    if (songName.isNotEmpty())
        o->setProperty ("name", songName);
    change (*o);
    const auto folder = songFolder (digest);
    return folder.createDirectory().wasOk() &&
           folder.getChildFile (entryFile).replaceWithText (juce::JSON::toString (entry) + "\n");
}

bool AnalysisLibrary::set (const juce::String& digest, const juce::String& songName, const juce::Identifier& property,
                           const juce::var& value)
{
    return update (digest, songName, [&] (juce::DynamicObject& o) { o.setProperty (property, value); });
}

bool AnalysisLibrary::setIn (const juce::String& digest, const juce::String& songName, const juce::Identifier& property,
                             const juce::String& name, const juce::var& value)
{
    return update (digest, songName,
                   [&] (juce::DynamicObject& o)
                   {
                       auto inner = o.getProperty (property);
                       if (! inner.isObject())
                           inner = juce::var (new juce::DynamicObject());
                       inner.getDynamicObject()->setProperty (name, value);
                       o.setProperty (property, inner);
                   });
}

std::map<juce::String, juce::File> AnalysisLibrary::findStems (const juce::String& digest,
                                                               const juce::String& model) const
{
    std::map<juce::String, juce::File> found;
    if (! isAvailable() || digest.isEmpty() || model.isEmpty())
        return found;
    const auto folder = songFolder (digest).getChildFile ("stems").getChildFile (model);
    const auto names = juce::JSON::parse (folder.getChildFile (stemsListFile));
    if (auto* list = names.getArray())
        for (const auto& name : *list)
        {
            const auto file = folder.getChildFile (name.toString() + ".flac");
            if (! file.existsAsFile())
                return {};
            found[name.toString()] = file;
        }
    return found;
}

juce::StringArray AnalysisLibrary::stemModels (const juce::String& digest) const
{
    juce::StringArray models;
    if (! isAvailable() || digest.isEmpty())
        return models;
    for (const auto& folder :
         songFolder (digest).getChildFile ("stems").findChildFiles (juce::File::findDirectories, false))
        if (! findStems (digest, folder.getFileName()).empty())
            models.add (folder.getFileName());
    models.sort (true);
    return models;
}

juce::Result AnalysisLibrary::keepStems (const juce::String& digest, const juce::String& model,
                                         const std::map<juce::String, juce::File>& stems,
                                         const std::function<bool()>& shouldStop) const
{
    if (! isAvailable() || digest.isEmpty() || stems.empty())
        return juce::Result::fail ("There is nothing to keep.");
    const auto folder = songFolder (digest).getChildFile ("stems").getChildFile (model);
    folder.getChildFile (stemsListFile).deleteFile(); // incomplete until written again
    if (auto made = folder.createDirectory(); made.failed())
        return made;
    juce::Array<juce::var> names;
    for (const auto& [name, file] : stems)
    {
        if (auto encoded = encodeFlac (file, folder.getChildFile (name + ".flac"), shouldStop); encoded.failed())
            return encoded;
        names.add (name);
    }
    if (! folder.getChildFile (stemsListFile).replaceWithText (juce::JSON::toString (juce::var (names))))
        return juce::Result::fail ("Spinus cannot write in " + folder.getFullPathName());
    return juce::Result::ok();
}

juce::Result AnalysisLibrary::restoreStems (const juce::String& digest, const juce::String& model,
                                            const juce::File& folder, std::map<juce::String, juce::File>& wavs,
                                            const std::function<bool()>& shouldStop) const
{
    const auto kept = findStems (digest, model);
    if (kept.empty())
        return juce::Result::fail ("The stems are no longer kept.");
    const auto into = folder.getChildFile (keyOf (digest) + "-" + model);
    std::atomic<double> progress { 0.0 };
    std::map<juce::String, juce::File> restored;
    for (const auto& [name, flac] : kept)
    {
        const auto wav = into.getChildFile (name + ".wav");
        if (auto decoded = prepareSong (flac, wav, shouldStop, progress); decoded.failed())
            return decoded;
        restored[name] = wav;
    }
    wavs = std::move (restored);
    return juce::Result::ok();
}

std::vector<AnalysisLibrary::Entry> AnalysisLibrary::list() const
{
    std::vector<Entry> entries;
    if (! isAvailable() || ! root.isDirectory())
        return entries;
    for (const auto& folder : root.findChildFiles (juce::File::findDirectories, false))
    {
        Entry entry;
        entry.key = folder.getFileName();
        entry.folder = folder;
        const auto json = juce::JSON::parse (folder.getChildFile (entryFile));
        entry.current = json.isObject() && (int)json["version"] == version;
        entry.name = json["name"].toString();
        if (entry.name.isEmpty())
            entry.name = entry.key;
        entry.bytes = sizeOf (folder);
        entry.stemBytes = sizeOf (folder.getChildFile ("stems"));
        entry.stemModels = stemModels (entry.key);
        entry.modified = folder.getChildFile (entryFile).getLastModificationTime();
        entries.push_back (std::move (entry));
    }
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.modified > b.modified; });
    return entries;
}

juce::int64 AnalysisLibrary::totalBytes() const { return isAvailable() ? sizeOf (root) : 0; }

bool AnalysisLibrary::removeSong (const juce::String& key) const
{
    const auto folder = root.getChildFile (keyOf (key));
    return isAvailable() && key.isNotEmpty() && folder.isAChildOf (root) && folder.deleteRecursively();
}

bool AnalysisLibrary::removeStems (const juce::String& key) const
{
    const auto folder = root.getChildFile (keyOf (key)).getChildFile ("stems");
    return isAvailable() && key.isNotEmpty() && folder.isAChildOf (root) && folder.deleteRecursively();
}

bool AnalysisLibrary::removeAllStems() const
{
    bool ok = true;
    for (const auto& entry : list())
        ok = removeStems (entry.key) && ok;
    return ok;
}

bool AnalysisLibrary::removeAll() const { return isAvailable() && root.deleteRecursively(); }

//==============================================================================
BackgroundTask::BackgroundTask (const juce::String& name, Work w, std::function<void (const juce::Result&)> d)
    : juce::Thread (name), work (std::move (w)), done (std::move (d))
{
    startThread();
}

BackgroundTask::~BackgroundTask()
{
    *alive = false;
    stopThread (30000);
}

void BackgroundTask::run()
{
    auto result = work ([this] { return threadShouldExit(); });
    if (threadShouldExit())
        return;
    juce::MessageManager::callAsync (
        [flag = alive, then = done, result]
        {
            if (*flag && then)
                then (result);
        });
}

} // namespace spinus
