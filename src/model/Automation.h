#pragma once

#include "Model.h"

namespace spinus::model
{

/** The value (0..1) an automation clip's points give at a time (ticks from the clip's start), as the
    engine plays it: held before the first point and after the last; between two points a straight
    line, or for a curve other than 0 the bend Tracktion Engine draws (AutomationCurve::getValueAt). */
float automationValueAt (const std::vector<AutomationPoint>&, double time);

/** Shapes the Automation editor writes over a time range. */
enum class AutomationShape
{
    rampUp,
    rampDown,
    sine,
    triangle,
    square,
    saw
};

/** Points drawing `shape` from `from` to `to`, repeating every `period` ticks (ramps span the whole
    range), between the values `low` and `high`. */
std::vector<AutomationPoint> makeAutomationShape (AutomationShape, Ticks from, Ticks to, Ticks period, float low,
                                                  float high);

} // namespace spinus::model
