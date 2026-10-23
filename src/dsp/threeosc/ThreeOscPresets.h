#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

namespace spinus::dsp
{

/** One of 3x Osc's factory presets: settings in the controls' own units (semitones, cents, seconds,
    Hz, dB; a menu's choice by its index) by parameter ID. A setting a preset leaves out has its default value,
    so every preset sets every control. */
struct ThreeOscPreset
{
    juce::String name, category, description;
    std::vector<std::pair<juce::String, float>> settings;
};

/** 3x Osc's factory presets: built into Spinus (and the "Spinus 3x Osc" plugin) as code, so they need no files.
    They are its programs too (program 0 is "Default", the settings of a new 3x Osc), which other hosts and
    Spinus's closest preset (InstrumentPresets) list. */
namespace threeOscPresets
{
/** The name of the first entry: every control at its default. */
inline constexpr const char* defaultName = "Default";

/** The factory presets, in the order shown (by category: Bass, Lead, Pad, Keys, FX). */
const std::vector<ThreeOscPreset>& all();

/** The preset with this name (any case), or nullptr. "Default" is no preset: use values (nullptr). */
const ThreeOscPreset* find (const juce::String& name);

/** Every one of 3x Osc's parameters for the preset, as values from 0 to 1 (as automation and device.setParameters
    use them); nullptr: the defaults. Any thread (no processor needed). */
std::map<juce::String, float> values (const ThreeOscPreset*);
} // namespace threeOscPresets

} // namespace spinus::dsp
