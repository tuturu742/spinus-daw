// spinus-plugin-host-system: in the Flatpak, starts spinus-plugin-host with the computer's own libraries,
// for a plugin of the computer's that needs libraries newer than the Flatpak's. Started by
// spinus::plugins::sandbox::SandboxPluginFormat in place of spinus-plugin-host, with the same arguments.

#include "plugins/HostLibraries.h"

#include <iostream>
#include <unistd.h>
#include <vector>

int main (int argc, char* argv[])
{
    const auto host = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                          .getSiblingFile ("spinus-plugin-host")
                          .getFullPathName();
    juce::StringArray command;
    if (const auto& libraries = spinus::plugins::getHostLibraries())
    {
        for (const auto& variable : libraries->environment)
            setenv (variable.upToFirstOccurrenceOf ("=", false, false).toRawUTF8(),
                    variable.fromFirstOccurrenceOf ("=", false, false).toRawUTF8(), 1);
        command.addArray (
            { libraries->loader.getFullPathName(), "--library-path", libraries->folders.joinIntoString (":") });
    }
    else
        std::cerr << "spinus-plugin-host-system: the computer's libraries were not found; using the Flatpak's\n";
    command.add (host);
    for (int i = 1; i < argc; ++i)
        command.add (juce::String::fromUTF8 (argv[i]));

    std::vector<std::string> strings;
    strings.reserve (static_cast<size_t> (command.size()));
    for (const auto& part : command)
        strings.push_back (part.toStdString());
    std::vector<char*> args;
    args.reserve (strings.size() + 1);
    for (auto& s : strings)
        args.push_back (s.data());
    args.push_back (nullptr);
    execv (args[0], args.data());
    std::cerr << "spinus-plugin-host-system: could not start " << args[0] << "\n";
    return 127;
}
