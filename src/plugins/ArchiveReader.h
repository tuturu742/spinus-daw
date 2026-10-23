#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

namespace spinus::plugins
{

/** Unpacks .zip and .tar.gz files with JUCE alone (its zip and gzip readers and a small tar reader: ustar,
    GNU long names and pax paths). The free plugin downloads use it on Windows, where Linux's libarchive is
    not there; it is compiled everywhere so it is tested everywhere.

    `destination` says where each entry (its path in the archive, with "/") goes, or an invalid File to
    skip it. Only regular files and folders are written; links and devices are skipped. */
namespace archives
{
using Destination = std::function<juce::File (const juce::String& entry)>;

juce::Result unpackZip (const juce::File& file, const Destination&, const std::atomic<bool>* cancel = nullptr);
juce::Result unpackTarGz (const juce::File& file, const Destination&, const std::atomic<bool>* cancel = nullptr);
} // namespace archives

} // namespace spinus::plugins
