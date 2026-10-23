#pragma once

#include "Model.h"

namespace spinus::model
{

/** Checks the invariants every project must keep, and returns one readable message per problem
    (empty = valid). Commands keep projects valid; this guards file loading and tests.

    Invariants: IDs are unique and below `nextId`; every reference (channel → insert, insert output,
    sends, sidechain sources, clip → lane/pattern, notes → channel, automation targets) points at an
    existing object; mixer routing (outputs, sends and sidechains) has no cycles;
    sane note, clip and automation values.
*/
juce::StringArray validate (const Project&);

/** True if routing `from` into `to` (as output, send or sidechain source) would create a cycle, or `to`
    is `from`. Sidechains count as routing: an insert waits for its sidechain source's sound. */
bool wouldCreateRoutingCycle (const Project&, InsertId from, InsertId to);

} // namespace spinus::model
