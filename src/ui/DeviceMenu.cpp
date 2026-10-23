#include "DeviceMenu.h"

#include "MainView.h"
#include "Workspace.h"

namespace spinus::ui
{

void addDeviceItems (juce::PopupMenu& menu, const plugins::DeviceList& list, bool instruments,
                     const std::function<void (const plugins::DeviceList::Entry&)>& onChoose)
{
    using Origin = plugins::DeviceList::Origin;
    std::map<juce::String, juce::PopupMenu> byMaker;

    for (auto origin : { Origin::spinus, Origin::bundled })
    {
        bool first = true;
        for (auto& entry : list.getEntries())
        {
            if (entry.instrument != instruments || entry.origin != origin)
                continue;
            if (first && origin == Origin::bundled)
                menu.addSectionHeader ("Free open source");
            first = false;
            if (origin != Origin::bundled)
                menu.addItem (entry.name, [onChoose, entry] { onChoose (entry); });
            else
                menu.addItem (entry.name + "  (" + entry.maker + (entry.available ? ")" : ", not downloaded)"),
                              entry.available, false, [onChoose, entry] { onChoose (entry); });
        }
    }

    for (auto& entry : list.getEntries())
        if (entry.instrument == instruments && entry.origin == Origin::installed)
            byMaker[entry.maker].addItem (entry.name + "  (" + entry.badge + ")",
                                          [onChoose, entry] { onChoose (entry); });

    if (byMaker.empty())
        return;

    juce::PopupMenu installed;
    for (auto& [maker, items] : byMaker)
        installed.addSubMenu (maker, items);
    menu.addSeparator();
    menu.addSubMenu ("Installed plugins", installed);
}

bool createAutomationClip (juce::Component& from, Session& session, const model::ParameterTarget& target)
{
    const auto clip = session.createAutomationClip (target);
    if (! clip.isValid())
        return false;
    // A new clip is a flat line at the current value: it changes nothing until it is shaped.
    if (auto* view = from.findParentComponentOfClass<MainView>())
    {
        view->openAutomationClip (clip);
        view->getHintBar().showMessage ("Automation clip added as a flat line: shape it in the Automation editor "
                                        "(click to add points, drag them), and play in song mode");
    }
    else if (auto* workspace = from.findParentComponentOfClass<Workspace>())
        workspace->showPanel ("playlist");
    return true;
}

juce::PopupMenu makeAutomateParameterMenu (juce::Component& from, Session& session, model::DeviceId device)
{
    juce::PopupMenu menu;
    // The control moved last in this device's window comes first (FL: "last tweaked").
    if (const auto last = session.getLastMovedControl(); last && last->device == device)
    {
        menu.addItem ("Last moved: " + session.describeLastMovedControl(),
                      [safe = juce::Component::SafePointer<juce::Component> (&from), &session, target = *last]
                      {
                          if (safe != nullptr)
                              createAutomationClip (*safe, session, target);
                      });
        menu.addSeparator();
    }
    const auto parameters = session.getDeviceParameters (device);
    constexpr size_t groupSize = 40;

    auto add = [&] (juce::PopupMenu& to, size_t i)
    {
        const auto& [id, name] = parameters[i];
        // The current value helps tell the controls apart ("Cutoff (1.20 kHz)").
        auto label = name.isNotEmpty() ? name : id;
        if (const auto value = session.getDeviceParameterValueText (device, id); value.isNotEmpty())
            label << "  (" << value << ")";
        to.addItem (label,
                    [safe = juce::Component::SafePointer<juce::Component> (&from), &session, device, parameterId = id]
                    {
                        if (safe == nullptr)
                            return;
                        model::ParameterTarget target;
                        target.kind = model::ParameterTarget::Kind::deviceParameter;
                        target.device = device;
                        target.parameterId = parameterId;
                        createAutomationClip (*safe, session, target);
                    });
    };

    if (parameters.size() <= groupSize)
    {
        for (size_t i = 0; i < parameters.size(); ++i)
            add (menu, i);
        return menu;
    }

    for (size_t start = 0; start < parameters.size(); start += groupSize)
    {
        juce::PopupMenu group;
        const auto end = juce::jmin (parameters.size(), start + groupSize);
        for (auto i = start; i < end; ++i)
            add (group, i);
        menu.addSubMenu (parameters[start].second + " ... " + parameters[end - 1].second, group);
    }
    return menu;
}

} // namespace spinus::ui
