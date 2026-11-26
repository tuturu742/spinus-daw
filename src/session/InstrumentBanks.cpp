#include "InstrumentBanks.h"

#include "InstrumentPresets.h"
#include "Session.h"
#include "engine/EngineAdapter.h"

#include <tracktion_engine/tracktion_engine.h>

namespace spinus::instrumentbanks
{

namespace
{
constexpr int deepest = 4; // wrappings looked through (a host's XML around base64 around the plugin's own data)
constexpr size_t largestXml = 67'108'864, longestRun = 16'777'216; // bytes read as one XML text or one string

bool isPathChar (juce::juce_wchar c)
{
    return c >= 0x20 && c != '"' && c != '<' && c != '>' && c != '|' && c != '*' && c != '?' && c != 0x7f;
}

/** A path's end trimmed to its file: no trailing spaces, quotes or punctuation; it must end in ".ext". */
juce::String asFile (juce::String path)
{
    path = path.trimEnd();
    while (path.isNotEmpty() && juce::String (" ,;:)]}'").containsChar (path.getLastCharacter()))
        path = path.dropLastCharacters (1);
    path = path.replaceCharacter ('\\', '/');
    const auto name = path.fromLastOccurrenceOf ("/", false, false);
    const auto dot = name.lastIndexOfChar ('.');
    if (dot <= 0 || name.length() - dot - 1 < 1 || name.length() - dot - 1 > 10)
        return {};
    if (! name.substring (dot + 1).containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"))
        return {};
    if (path.retainCharacters ("/").length() < 2)
        return {}; // "/x.sfz" alone is too little to be a real file's place
    return path;
}

/** Paths in text: "/home/...", "C:\...", "file:///...". */
void pathsInText (const juce::String& text, juce::StringArray& found)
{
    const auto length = text.length();
    for (int i = 0; i < length; ++i)
    {
        int start = -1;
        if (text.substring (i, i + 7).equalsIgnoreCase ("file://"))
        {
            start = i + 7;
            if (start + 2 < length && text[start + 2] == ':' && text[start] == '/')
                ++start; // file:///C:/...
        }
        else
        {
            const bool unixPath =
                text[i] == '/' && (i == 0 || ! isPathChar (text[i - 1]) || text[i - 1] == ' ' || text[i - 1] == '=');
            const bool windowsPath = juce::CharacterFunctions::isLetter (text[i]) && i + 2 < length &&
                                     text[i + 1] == ':' && (text[i + 2] == '\\' || text[i + 2] == '/') &&
                                     (i == 0 || ! juce::CharacterFunctions::isLetterOrDigit (text[i - 1]));
            if (unixPath || windowsPath)
                start = i;
        }
        if (start < 0)
            continue;
        int end = start;
        while (end < length && isPathChar (text[end]))
            ++end;
        auto path = text.substring (start, end);
        if (text.substring (i, i + 7).equalsIgnoreCase ("file://"))
            path = juce::URL::removeEscapeChars (path);
        // A path may run into the next one's text: cut at the last extension that makes a file.
        for (auto candidate = path; candidate.isNotEmpty();)
        {
            if (const auto file = asFile (candidate); file.isNotEmpty())
            {
                found.addIfNotAlreadyThere (file);
                break;
            }
            const auto space = candidate.lastIndexOfChar (' ');
            if (space <= 0)
                break;
            candidate = candidate.substring (0, space);
        }
        i = juce::jmax (i, end - 1);
    }
}

bool looksLikeBase64 (const juce::String& text)
{
    const auto t = text.trim();
    return t.length() >= 24 &&
           t.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=.\r\n");
}

void look (const juce::MemoryBlock& block, juce::StringArray& found, int depth);

/** How many bytes the character at `at` takes when it is readable text: printable ASCII, or a whole, valid UTF-8
    sequence of 2 to 4 bytes that fits before `size` (a path can hold any letters); 0 for anything else. Only text
    made of these is turned into a String: JUCE reads past the end of a truncated sequence. */
size_t readableAt (const char* bytes, size_t at, size_t size)
{
    const auto b = (unsigned char)bytes[at];
    if (b >= 0x20 && b < 0x7f)
        return 1;
    const size_t length = (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
    if (length == 0 || b == 0xc0 || b == 0xc1 || b > 0xf4 || at + length > size)
        return 0;
    for (size_t k = 1; k < length; ++k)
        if (((unsigned char)bytes[at + k] & 0xc0) != 0x80)
            return 0;
    return length;
}

/** Bytes as text, when all of them are (readableAt, or whitespace); empty otherwise. */
juce::String asText (const char* bytes, size_t size)
{
    for (size_t i = 0; i < size;)
    {
        if (bytes[i] == '\n' || bytes[i] == '\r' || bytes[i] == '\t' || (bytes[i] == 0 && i + 1 == size))
        {
            ++i;
            continue;
        }
        const auto step = readableAt (bytes, i, size);
        if (step == 0)
            return {};
        i += step;
    }
    return juce::String::fromUTF8 (bytes, (int)size);
}

/** Base64 text decoded: JUCE's own form ("size.chars", MemoryBlock::toBase64Encoding, as hosts built on JUCE
    write a plugin's state in their XML) or the standard one. */
bool decodeBase64 (const juce::String& text, juce::MemoryBlock& out)
{
    const auto t = text.trim().removeCharacters ("\r\n");
    if (t.upToFirstOccurrenceOf (".", false, false).containsOnly ("0123456789") && t.containsChar ('.'))
        return out.fromBase64Encoding (t) && out.getSize() > 0;
    juce::MemoryOutputStream stream (out, false);
    return juce::Base64::convertFromBase64 (stream, t) && stream.getDataSize() > 0;
}

/** Text that may be base64: looked into as well as read. */
void lookInText (const juce::String& text, juce::StringArray& found, int depth)
{
    pathsInText (text, found);
    if (looksLikeBase64 (text))
        if (juce::MemoryBlock decoded; decodeBase64 (text, decoded))
            look (decoded, found, depth + 1);
}

void lookInXml (const juce::XmlElement& xml, juce::StringArray& found, int depth)
{
    for (int i = 0; i < xml.getNumAttributes(); ++i)
        lookInText (xml.getAttributeValue (i), found, depth);
    for (auto* child : xml.getChildIterator())
    {
        if (child->isTextElement())
            lookInText (child->getText(), found, depth);
        else
            lookInXml (*child, found, depth);
    }
}

void look (const juce::MemoryBlock& block, juce::StringArray& found, int depth)
{
    if (depth > deepest || block.getSize() == 0)
        return;
    const auto* bytes = static_cast<const char*> (block.getData());
    const auto size = block.getSize();

    // A host's binary XML (JUCE's copyXmlToBinary: a magic number, the size, then the text).
    if (size > 8 && juce::ByteOrder::littleEndianInt (bytes) == 0x21324356)
    {
        const auto length = juce::jmin ((size_t)juce::ByteOrder::littleEndianInt (bytes + 4), size - 8);
        if (auto xml = juce::parseXML (asText (bytes + 8, length)))
        {
            lookInXml (*xml, found, depth);
            return;
        }
    }
    // XML text.
    size_t first = 0;
    while (first < size && juce::CharacterFunctions::isWhitespace ((juce::juce_wchar)(unsigned char)bytes[first]))
        ++first;
    if (first < size && bytes[first] == '<' && size < largestXml)
        if (auto xml = juce::parseXML (asText (bytes, size)))
        {
            lookInXml (*xml, found, depth);
            return;
        }
    // Anything else: the readable runs in it (binary data with strings inside, JSON, a plugin's own text).
    for (size_t i = 0; i < size;)
    {
        if (readableAt (bytes, i, size) == 0)
        {
            ++i;
            continue;
        }
        auto end = i;
        while (end < size && end - i < longestRun)
            if (const auto step = readableAt (bytes, end, size); step > 0 && end + step - i <= longestRun)
                end += step;
            else
                break;
        if (end - i >= 4)
            lookInText (juce::String::fromUTF8 (bytes + i, (int)(end - i)), found, depth);
        i = end;
    }
}
} // namespace

juce::StringArray filesNamedIn (const juce::MemoryBlock& state)
{
    juce::StringArray found;
    look (state, found, 0);
    return found;
}

InstrumentBank fromState (const juce::MemoryBlock& state)
{
    const auto files = filesNamedIn (state);
    if (files.isEmpty())
        return {};
    return { files.joinIntoString ("\n"),
             files[0].fromLastOccurrenceOf ("/", false, false).upToLastOccurrenceOf (".", false, false) };
}

InstrumentBank fromStateFile (const juce::File& state)
{
    juce::MemoryBlock block;
    if (state == juce::File() || ! state.loadFileAsData (block))
        return {};
    return fromState (block);
}

InstrumentBank named (const juce::String& name)
{
    const auto n = name.trim();
    if (n.isEmpty())
        return {};
    return { "named:" + n, n };
}

InstrumentBank fromProgram (const juce::String& programName)
{
    const auto n = programName.trim();
    if (n.isEmpty())
        return {};
    return { "program:" + n, n };
}

InstrumentBank of (Session& session, const model::Channel& channel)
{
    if (channel.bank.trim().isNotEmpty())
        return named (channel.bank);
    if (! channel.instrument)
        return {};
    auto* plugin =
        dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (channel.instrument->id));
    auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
    if (instance == nullptr)
        return {};
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    if (auto bank = fromState (state); bank.isKnown())
        return bank;
    if (instance->getNumPrograms() > 1)
        return fromProgram (instance->getProgramName (instance->getCurrentProgram()));
    return {};
}

juce::var keysForAgents (const InstrumentProfiles& profiles, const juce::String& instrument, const InstrumentBank& bank)
{
    if (const auto profile = profiles.get (instrument, bank))
        return InstrumentProfiles::describeForAgents (*profile);
    if (! profiles.byBank (instrument))
        return {};
    auto* o = new juce::DynamicObject();
    o->setProperty ("bank", bank.isKnown() ? bank.name : juce::String ("not known"));
    o->setProperty ("learned", false);
    o->setProperty ("note", bank.isKnown()
                                ? "what this instrument's keys do depends on the bank it plays, and this bank isn't "
                                  "learned yet: any key may play (learn it with learn_instrument)"
                                : "what this instrument's keys do depends on the bank it plays, and Spinus can't "
                                  "tell which bank this channel plays: any key may play (the user can name it: "
                                  "channel.set bank)");
    return juce::var (o);
}

juce::var keysForAgents (const InstrumentProfiles& profiles, Session& session, const model::Channel& channel)
{
    if (! channel.instrument)
        return {};
    const auto instrument = InstrumentPresets::libraryOf (*channel.instrument);
    return keysForAgents (profiles, instrument,
                          profiles.byBank (instrument) ? of (session, channel) : InstrumentBank {});
}

} // namespace spinus::instrumentbanks
