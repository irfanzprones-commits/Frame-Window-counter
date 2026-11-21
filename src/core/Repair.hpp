// Auto-repair of a failed verification (v0.1.3): the click that was active when the run died is moved tick by tick,
// first LATER (+1 .. +max), then EARLIER (-1 .. -max). Game-independent: the Geode layer only asks "which click, which shifts"
// and re-runs the verification on every candidate; nothing here touches the game.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "Model.hpp"
#include "Session.hpp"

namespace gdfwr {

inline constexpr int kRepairDefaultShift = 10;   // largest shift tried in each direction
inline constexpr int kRepairMaxClicks = 400;     // v0.3.0 (was 5): trouble spots fitted one after the other before giving up. A recording made at a low
                                                 // frame rate can be off by a few ticks at EVERY click, so every one of them may need its own fit.

// One click of the final run: a press and (unless the run ends while holding) its release, both of the same player.
struct RepairClick {
    uint32_t pressSeq = 0, releaseSeq = 0;
    bool hasRelease = false;
    int player = 0;
    int index = 0;           // 0-based number of this click inside its player's stream
    int pressTick = 0, releaseTick = 0;
};

// The clicks that can be to blame when the run dies at failTick, most likely first:
//  - the click that owns the LAST edge (press or release) at or before that tick: it was too early, too short or too long;
//  - the first click that is pressed AFTER that tick: it was too late (the player died before the jump even started).
// If the player is holding at failTick (the last edge is a press) the first one is tried first, otherwise the second.
// Empty when the run has no click near that tick.
std::vector<RepairClick> culpritClicks(const Macro& m, int failTick);

// How long after a death a new death still counts as "the same trouble spot" (a shift of up to maxShift ticks moves the moment of
// death by about that much, and a spike is hit over several ticks). Later than that the click has fixed it and something else kills the run.
inline int repairSpotMargin(int maxShift) { return 2 * maxShift + 12; }

// Shifts to try, in order: +1, +2 ... +maxShift, then -1, -2 ... -maxShift.
std::vector<int> repairShiftOrder(int maxShift);

// Moves the whole click (press and release together, so the hold time stays the same) by `shift` ticks inside `m`.
// Fails (m is left unchanged) when the move would break the order against the neighbouring clicks or go below tick 0.
EditOutcome shiftClick(Macro& m, const RepairClick& c, int shift);

// "click 7 (player 1): moved 3 ticks later (tick 532 -> 535)"
std::string describeShift(const RepairClick& c, int shift);

}  // namespace gdfwr
