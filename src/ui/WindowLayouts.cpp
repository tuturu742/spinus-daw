#include "WindowLayouts.h"

namespace spinus::ui
{

namespace
{
const juce::StringArray& builtInNames()
{
    static const juce::StringArray names { "Compose", "Arrange", "Mix" };
    return names;
}

juce::ValueTree panel (const juce::String& id, float x, float y, float w, float h)
{
    juce::ValueTree p ("PANEL");
    p.setProperty ("id", id, nullptr);
    p.setProperty ("visible", true, nullptr);
    p.setProperty ("detached", false, nullptr);
    p.setProperty ("x", x, nullptr);
    p.setProperty ("y", y, nullptr);
    p.setProperty ("w", w, nullptr);
    p.setProperty ("h", h, nullptr);
    return p;
}
} // namespace

WindowLayouts::WindowLayouts (juce::File f) : file (std::move (f)) { reload(); }

void WindowLayouts::reload()
{
    if (file == juce::File())
        return;
    if (auto xml = juce::XmlDocument::parse (file))
        if (auto tree = juce::ValueTree::fromXml (*xml); tree.hasType ("LAYOUTS"))
            saved = tree;
}

juce::ValueTree WindowLayouts::builtIn (const juce::String& name)
{
    juce::ValueTree layout ("LAYOUT");
    layout.setProperty ("name", name, nullptr);
    layout.setProperty ("browser", true, nullptr);
    layout.setProperty ("assistant", false, nullptr);

    // Panels the layout doesn't list are closed. Back to front.
    if (name == "Compose")
    {
        layout.appendChild (panel ("playlist", 0.45f, 0.0f, 0.55f, 0.45f), nullptr);
        layout.appendChild (panel ("channelRack", 0.0f, 0.0f, 0.45f, 0.45f), nullptr);
        layout.appendChild (panel ("pianoRoll", 0.0f, 0.45f, 1.0f, 0.55f), nullptr);
    }
    else if (name == "Arrange")
    {
        layout.appendChild (panel ("channelRack", 0.55f, 0.5f, 0.45f, 0.5f), nullptr);
        layout.appendChild (panel ("playlist", 0.0f, 0.0f, 1.0f, 1.0f), nullptr);
    }
    else if (name == "Mix")
    {
        layout.setProperty ("browser", false, nullptr);
        layout.appendChild (panel ("playlist", 0.0f, 0.0f, 1.0f, 0.4f), nullptr);
        layout.appendChild (panel ("mixer", 0.0f, 0.4f, 1.0f, 0.6f), nullptr);
    }
    else
    {
        return {};
    }
    return layout;
}

juce::StringArray WindowLayouts::getNames() const
{
    auto names = builtInNames();
    for (const auto& layout : saved)
        if (layout.hasType ("LAYOUT"))
            names.addIfNotAlreadyThere (layout["name"].toString());
    return names;
}

juce::StringArray WindowLayouts::getSavedNames() const
{
    juce::StringArray names;
    for (const auto& layout : saved)
        if (layout.hasType ("LAYOUT"))
            names.add (layout["name"].toString());
    return names;
}

bool WindowLayouts::isBuiltIn (const juce::String& name) const { return builtInNames().contains (name); }

juce::ValueTree WindowLayouts::get (const juce::String& name) const
{
    for (const auto& layout : saved)
        if (layout.hasType ("LAYOUT") && layout["name"].toString() == name)
            return layout.createCopy();
    return builtIn (name);
}

void WindowLayouts::save (const juce::String& name, const juce::ValueTree& layout)
{
    reload(); // another Spinus may have saved layouts since
    auto copy = layout.createCopy();
    copy.setProperty ("name", name, nullptr);
    for (int i = 0; i < saved.getNumChildren(); ++i)
        if (saved.getChild (i).hasType ("LAYOUT") && saved.getChild (i)["name"].toString() == name)
        {
            saved.removeChild (i, nullptr);
            saved.addChild (copy, i, nullptr);
            write();
            return;
        }
    saved.appendChild (copy, nullptr);
    write();
}

void WindowLayouts::remove (const juce::String& name)
{
    reload();
    for (int i = saved.getNumChildren(); --i >= 0;)
        if (saved.getChild (i).hasType ("LAYOUT") && saved.getChild (i)["name"].toString() == name)
            saved.removeChild (i, nullptr);
    write();
}

juce::ValueTree WindowLayouts::getLast() const { return saved.getChildWithName ("LAST").getChild (0).createCopy(); }

void WindowLayouts::setLast (const juce::ValueTree& layout)
{
    reload(); // keeps the layouts another Spinus saved meanwhile
    saved.removeChild (saved.getChildWithName ("LAST"), nullptr);
    juce::ValueTree last ("LAST");
    last.appendChild (layout.createCopy(), nullptr);
    saved.appendChild (last, nullptr);
    write();
}

void WindowLayouts::write() const
{
    if (file == juce::File())
        return;
    if (auto xml = saved.createXml()) [[maybe_unused]]
        const auto written = xml->writeTo (file);
}

} // namespace spinus::ui
