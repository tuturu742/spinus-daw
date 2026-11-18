#include "session/MissingAudio.h"

namespace spinus::missingaudio
{

juce::File resolve (const juce::String& file, const juce::File& projectFolder)
{
    if (juce::File::isAbsolutePath (file))
        return juce::File (file);
    return projectFolder == juce::File() ? juce::File() : projectFolder.getChildFile ("audio").getChildFile (file);
}

juce::StringArray find (const model::Project& project, const juce::File& projectFolder)
{
    juce::StringArray missing;
    auto check = [&] (const model::AudioClip& audio)
    {
        if (! missing.contains (audio.file) && ! resolve (audio.file, projectFolder).existsAsFile())
            missing.add (audio.file);
    };
    for (auto& clip : project.playlist.clips)
        if (auto* audio = std::get_if<model::AudioClip> (&clip.content))
            check (*audio);
    for (auto& clip : project.launcher.clips)
        if (auto* audio = std::get_if<model::AudioClip> (&clip.content))
            check (*audio);
    return missing;
}

std::map<juce::String, juce::File> search (const juce::StringArray& missing, const juce::File& folder)
{
    // A file saved on Windows names its folders with backslashes: only its name counts here.
    auto nameOf = [] (const juce::String& file)
    { return file.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false); };

    std::map<juce::String, juce::File> found, anyCase;
    if (! folder.isDirectory())
        return found;
    int looked = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
    {
        if (++looked > 200000)
            break;
        const auto name = entry.getFile().getFileName();
        for (auto& file : missing)
        {
            const auto wanted = nameOf (file);
            if (name == wanted && ! found.contains (file))
                found[file] = entry.getFile();
            else if (name.equalsIgnoreCase (wanted) && ! anyCase.contains (file))
                anyCase[file] = entry.getFile();
        }
    }
    for (auto& [file, match] : anyCase)
        found.try_emplace (file, match);
    return found;
}

} // namespace spinus::missingaudio
