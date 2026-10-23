#include "ArchiveReader.h"

namespace spinus::plugins::archives
{

namespace
{
juce::Result writeEntry (juce::InputStream& in, juce::int64 size, const juce::File& target)
{
    target.getParentDirectory().createDirectory();
    target.deleteFile();
    juce::FileOutputStream out (target);
    if (! out.openedOk() || out.writeFromInputStream (in, size) != size || (out.flush(), out.getStatus().failed()))
        return juce::Result::fail ("Could not write " + target.getFullPathName() + ".");
    return juce::Result::ok();
}
} // namespace

juce::Result unpackZip (const juce::File& file, const Destination& destination, const std::atomic<bool>* cancel)
{
    juce::ZipFile zip (file);
    if (zip.getNumEntries() == 0)
        return juce::Result::fail ("Could not open " + file.getFileName() + ".");
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        if (cancel != nullptr && cancel->load())
            return juce::Result::fail ("Cancelled.");
        const auto* entry = zip.getEntry (i);
        if (entry->filename.endsWith ("/") || entry->isSymbolicLink)
            continue;
        const auto target = destination (entry->filename);
        if (target == juce::File())
            continue;
        // Read the entry here: uncompressEntry would place it by its own name, not where `destination` says.
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        if (in == nullptr)
            return juce::Result::fail ("Could not unpack " + file.getFileName() + ".");
        if (auto written = writeEntry (*in, (juce::int64)entry->uncompressedSize, target); written.failed())
            return written;
    }
    return juce::Result::ok();
}

juce::Result unpackTarGz (const juce::File& file, const Destination& destination, const std::atomic<bool>* cancel)
{
    auto raw = file.createInputStream();
    if (raw == nullptr)
        return juce::Result::fail ("Could not open " + file.getFileName() + ".");
    juce::GZIPDecompressorInputStream in (raw.release(), true, juce::GZIPDecompressorInputStream::gzipFormat);

    const auto broken = [&file] { return juce::Result::fail ("Could not unpack " + file.getFileName() + "."); };
    const auto octal = [] (const char* text, int length)
    {
        juce::int64 value = 0;
        for (int i = 0; i < length && text[i] != 0; ++i)
            if (text[i] >= '0' && text[i] <= '7')
                value = value * 8 + (text[i] - '0');
        return value;
    };
    const auto readAll = [&in] (void* into, juce::int64 size) { return in.read (into, (int)size) == size; };
    // Moves on to `position` (the next header): false when the archive ends before it.
    const auto skipTo = [&in] (juce::int64 position)
    {
        in.skipNextBytes (position - in.getPosition());
        return in.getPosition() == position;
    };
    const auto text = [] (const char* from, size_t length)
    { return juce::String (juce::CharPointer_UTF8 (from), juce::CharPointer_UTF8 (from + strnlen (from, length))); };

    juce::String longName;
    char header[512];
    while (readAll (header, 512))
    {
        if (cancel != nullptr && cancel->load())
            return juce::Result::fail ("Cancelled.");
        if (std::all_of (header, header + 512, [] (char c) { return c == 0; }))
            break; // the end
        const auto size = octal (header + 124, 12);
        const auto next = in.getPosition() + (size + 511) / 512 * 512;
        const char type = header[156];

        // GNU long names ('L') and pax headers ('x') name the entry that follows.
        if (type == 'L' || type == 'x')
        {
            juce::MemoryBlock data ((size_t)size + 1, true);
            if (size > juce::int64 { 16 } * 1024 * 1024 || ! readAll (data.getData(), size) || ! skipTo (next))
                return broken();
            const auto body = text (static_cast<const char*> (data.getData()), (size_t)size);
            if (type == 'L')
                longName = body;
            else // records "<length> <key>=<value>\n"
                for (auto& line : juce::StringArray::fromLines (body))
                    if (const auto record = line.fromFirstOccurrenceOf (" ", false, false); record.startsWith ("path="))
                        longName = record.fromFirstOccurrenceOf ("path=", false, false);
            continue;
        }

        auto name = longName;
        longName = {};
        if (name.isEmpty())
        {
            const auto base = text (header, 100);
            const auto prefix = text (header + 345, 155); // ustar
            name = prefix.isNotEmpty() ? prefix + "/" + base : base;
        }

        const bool regular = type == '0' || type == 0;
        if (regular || type == '5')
            if (const auto target = destination (name); target != juce::File())
            {
                if (regular)
                {
                    if (auto written = writeEntry (in, size, target); written.failed())
                        return written;
                }
                else
                {
                    target.createDirectory();
                }
            }
        if (! skipTo (next))
            return broken();
    }
    return juce::Result::ok();
}

} // namespace spinus::plugins::archives
