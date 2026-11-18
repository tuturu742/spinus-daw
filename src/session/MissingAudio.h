#pragma once

#include "model/Model.h"

#include <juce_core/juce_core.h>

#include <map>

namespace spinus::missingaudio
{

/** Where an audio clip's file is: relative to the project's audio/ folder, or an absolute path. An untitled
    project (no folder) finds only absolute paths. */
juce::File resolve (const juce::String& file, const juce::File& projectFolder);

/** The files the project's audio clips (Playlist and Launcher) play that are not there, each once, as the
    clips name them, in the order they first appear. */
juce::StringArray find (const model::Project&, const juce::File& projectFolder);

/** Looks for each missing file in `folder` and the folders inside it by its name, the same case first, then any
    case: the ones found, by the name the clips use. Stops after 200,000 files, so a whole disk cannot hang it. */
std::map<juce::String, juce::File> search (const juce::StringArray& missing, const juce::File& folder);

} // namespace spinus::missingaudio
