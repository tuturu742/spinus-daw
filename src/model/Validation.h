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

/** Makes a project read from a damaged file valid, keeping all it can: entries without an ID of their own,
    devices without a type, clips on missing tracks or playing missing patterns, notes for missing channels
    and invalid notes are left out; references to missing inserts go to the Master; settings outside 0 to 1, a
    bad time signature and nextId are set right. Routing loops are broken: in a loop of outputs alone an insert
    plays into the Master instead; loops through sends or sidechains lose as few sends and sidechains as break
    them all, always the same ones for the same project (preferring those that lead back up the mixer).
    Returns what was left out or set back, in plain words for the user ("a clip at bar 5 on 'Drums' (its
    pattern is missing)"); empty if nothing was. */
juce::StringArray repair (Project&);

/** True if routing `from` into `to` (as output, send or sidechain source) would create a cycle, or `to`
    is `from`. Sidechains count as routing: an insert waits for its sidechain source's sound. */
bool wouldCreateRoutingCycle (const Project&, InsertId from, InsertId to);

} // namespace spinus::model
