#include "Automation.h"

#include <cmath>

namespace spinus::model
{

namespace
{
// Tracktion Engine's curve maths (tracktion_core/utilities/tracktion_Bezier.h), which does not depend on
// the units: the same shape in ticks and 0..1 as in seconds and the parameter's range.
std::pair<double, double> bezierPoint (double x1, double y1, double x2, double y2, double c)
{
    const auto run = x2 - x1;
    const auto xc = x1 + run / 2;
    if (y2 > y1)
    {
        const auto rise = y2 - y1;
        return { xc - run / 2 * -c, y1 + rise / 2 + rise / 2 * -c };
    }
    const auto rise = y1 - y2;
    return { xc - run / 2 * -c, y2 + rise / 2 - rise / 2 * -c };
}

double bezierYFromX (double x, double x1, double y1, double xb, double yb, double x2, double y2)
{
    if (juce::exactlyEqual (x1, x2) || juce::exactlyEqual (y1, y2))
        return y1;
    if (x <= x1)
        return y1;
    if (x >= x2)
        return y2;

    const auto a = x1 - 2 * xb + x2;
    const auto b = -2 * x1 + 2 * xb;
    const auto c = x1 - x;

    double t = 0.0;
    if (juce::exactlyEqual (a, 0.0))
        t = -c / b;
    else
    {
        t = (-b + std::sqrt (b * b - 4 * a * c)) / (2 * a);
        if (t < 0.0 || t > 1.0)
            t = (-b - std::sqrt (b * b - 4 * a * c)) / (2 * a);
    }

    return (1 - t) * (1 - t) * y1 + 2 * t * (1 - t) * yb + t * t * y2;
}
} // namespace

float automationValueAt (const std::vector<AutomationPoint>& points, double time)
{
    if (points.empty())
        return 0.0f;

    // The first point after `time` (Tracktion: the next point index).
    size_t index = 0;
    while (index < points.size() && (double)points[index].time <= time)
        ++index;

    if (index == 0)
        return points.front().value;
    if (index >= points.size())
        return points.back().value;

    const auto& p1 = points[index - 1];
    const auto& p2 = points[index];
    const auto x1 = (double)p1.time, x2 = (double)p2.time;
    const auto y1 = (double)p1.value, y2 = (double)p2.value;
    const auto curve = juce::jlimit (-1.0f, 1.0f, p1.curve);

    if (juce::exactlyEqual (curve, 0.0f) || x2 <= x1)
        return (float)(y1 + (time - x1) / juce::jmax (1.0e-9, x2 - x1) * (y2 - y1));

    const auto [xb, yb] = bezierPoint (x1, y1, x2, y2, juce::jlimit (-1.0, 1.0, (double)curve * 2.0));
    if (curve >= -0.5f && curve <= 0.5f)
        return (float)bezierYFromX (time, x1, y1, xb, yb, x2, y2);

    // Steeper: flat for part of the way, then the bend (Tracktion's "bezier ends").
    const auto minic = (std::abs ((double)curve) - 0.5) * 2.0;
    const auto run = minic * (x2 - x1);
    const auto rise = minic * std::abs (y2 - y1);
    double ex1 = x1, ey1 = y1, ex2 = x2, ey2 = y2;
    if (curve > 0)
    {
        ex1 = x1 + run;
        ey2 = y1 < y2 ? y2 - rise : y2 + rise;
    }
    else
    {
        ey1 = y1 < y2 ? y1 + rise : y1 - rise;
        ex2 = x2 - run;
    }

    if (time >= x1 && time <= ex1)
        return (float)y1;
    if (time >= ex2 && time <= x2)
        return (float)y2;
    return (float)bezierYFromX (time, ex1, ey1, xb, yb, ex2, ey2);
}

std::vector<AutomationPoint> makeAutomationShape (AutomationShape shape, Ticks from, Ticks to, Ticks period, float low,
                                                  float high)
{
    std::vector<AutomationPoint> points;
    if (to <= from)
        return points;
    period = juce::jlimit<Ticks> (2, to - from, period);
    low = juce::jlimit (0.0f, 1.0f, low);
    high = juce::jlimit (0.0f, 1.0f, high);

    auto add = [&] (Ticks t, float v, float curve = 0.0f)
    { points.push_back ({ juce::jlimit (from, to, t), juce::jlimit (0.0f, 1.0f, v), curve }); };

    switch (shape)
    {
    case AutomationShape::rampUp:
        add (from, low);
        add (to, high);
        break;
    case AutomationShape::rampDown:
        add (from, high);
        add (to, low);
        break;
    case AutomationShape::sine:
        // Eight points a cycle along the sine: smooth enough as straight lines.
        for (Ticks start = from; start < to; start += period)
            for (int i = 0; i < 8; ++i)
            {
                const auto t = start + period * i / 8;
                if (t < to)
                    add (t, low + (high - low) * 0.5f *
                                      (1.0f + std::sin ((float)i / 8.0f * juce::MathConstants<float>::twoPi)));
            }
        add (to, low + (high - low) * 0.5f);
        break;
    case AutomationShape::triangle:
        for (Ticks start = from; start < to; start += period)
        {
            add (start, low);
            add (juce::jmin (to, start + period / 2), high);
        }
        add (to, low);
        break;
    case AutomationShape::square:
        // Two points at the same time make a jump.
        for (Ticks start = from; start < to; start += period)
        {
            const auto half = juce::jmin (to, start + period / 2);
            add (start, high);
            add (half, high);
            add (half, low);
            add (juce::jmin (to, start + period), low);
        }
        break;
    case AutomationShape::saw:
        for (Ticks start = from; start < to; start += period)
        {
            const auto end = juce::jmin (to, start + period);
            add (start, high);
            add (end, low);
        }
        break;
    }

    // Duplicates at the cycle joins of the shapes that do not jump there.
    points.erase (std::unique (points.begin(), points.end()), points.end());
    return points;
}

} // namespace spinus::model
