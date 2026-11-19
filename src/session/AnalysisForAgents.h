#pragma once

#include "AnalysisLibrary.h"

namespace spinus::agents
{

/** What the analysis library tells agents (the MCP tools list_analyzed_songs and song_analysis). It works
    without the AI service: it only reads song.json.

    Songs are named by their file name and library key, never by where they are on this computer. By default
    an agent gets what was measured about a song (tempo, bars, key, loudness, and the Track creator's
    structure, chords, groove, ranges and parts); the exact notes and drum hits only when the user allows it in
    the Analyzer ("Provide full data through MCP", off by default), since they can copy a song's melodies and
    go to the agent's provider. */

/** Whether the user allows the exact notes and drum hits (analyzer.json in the AI settings folder). */
bool fullDataAllowed (const juce::File& aiFolder);
void setFullDataAllowed (const juce::File& aiFolder, bool allowed);

/** Every current song in the library: [{song (its key), name, measured: [...], analyzed (ISO date)}]. */
juce::var listSongs (const AnalysisLibrary&);

/** One song, found by its key or its name (whole, without the extension, or a unique part of it; any case).
    `part` asks for one part's exact notes ("bass", "htdemucs/vocals", "drums"), between `from` and `to`
    seconds (negative: no limit); refused unless `fullData`. Empty `error` when found. */
juce::var describeSong (const AnalysisLibrary&, const juce::String& song, bool fullData, const juce::String& part,
                        double from, double to, juce::String& error);

/** The most notes or hits one answer carries; asking for a stretch of time gives the rest. */
constexpr int maxEvents = 3000;

} // namespace spinus::agents
