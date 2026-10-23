#pragma once

namespace spinus
{
class Session;
}

namespace spinus::ui
{

class MainView;

/** The demo song the manual's screenshots show (`spinus --snapshot` with SPINUS_SNAPSHOT_DEMO=1, and the
    "[.screenshot]" tests): a lead on Surge XT, a 3x Osc bass and drums with steps, a melody in the Piano
    Roll, an arrangement with markers and an automation clip, and a mixer with effects, sends and a
    reverb bus. Everything goes through the panels, so each part is an ordinary undoable change. */
void makeManualDemo (MainView&, Session&);

} // namespace spinus::ui
