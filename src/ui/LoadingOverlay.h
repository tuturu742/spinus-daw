#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Covers the window while a project opens: its name, a bar, and the plugin being loaded. Opening runs on the
    message thread, so the overlay paints itself straight away (paintNow) instead of waiting for the loop. */
class LoadingOverlay : public juce::Component
{
public:
    LoadingOverlay();

    /** Shows "Opening <name>" over the parent, empty bar, and paints it now. */
    void start (const juce::String& name);
    /** done of total plugins created; what is loading next. Paints now. */
    void setProgress (int done, int total, const juce::String& what);
    void finish();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void paintNow();

    juce::String title, detail;
    double progress = 0.0;
    juce::ProgressBar bar { progress };
    juce::Rectangle<int> card;
};

} // namespace spinus::ui
