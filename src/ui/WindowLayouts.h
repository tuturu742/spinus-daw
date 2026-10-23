#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace spinus::ui
{

/** Named window layouts: which panels are open and where, plus the Browser and Assistant.
    Three come with Spinus (Compose, Arrange, Mix); the user saves their own, or changes those three.
    Kept in a settings file, not in projects: a layout belongs to the person and their screen. */
class WindowLayouts
{
public:
    /** No file: layouts live only while Spinus runs (tests). */
    WindowLayouts() = default;
    explicit WindowLayouts (juce::File);

    /** Spinus's own, then the user's, in the order saved. */
    juce::StringArray getNames() const;
    /** The user's saved layouts (including changed built-in ones). */
    juce::StringArray getSavedNames() const;
    bool isBuiltIn (const juce::String& name) const;

    /** A LAYOUT tree (see Workspace::getLayout) with `browser` and `assistant`; invalid if there is none. */
    juce::ValueTree get (const juce::String& name) const;
    void save (const juce::String& name, const juce::ValueTree& layout);
    /** A saved layout goes; a changed built-in one returns to Spinus's. */
    void remove (const juce::String& name);

    /** The layout when Spinus last closed, for the next start. */
    juce::ValueTree getLast() const;
    void setLast (const juce::ValueTree&);

    static juce::ValueTree builtIn (const juce::String& name);

private:
    void write() const;

    juce::File file;
    juce::ValueTree saved { "LAYOUTS" };
};

} // namespace spinus::ui
