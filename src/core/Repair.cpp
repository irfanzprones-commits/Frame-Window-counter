#include "Repair.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace gdfwr {

namespace {
struct Streams {
    std::vector<Event> fin;
    std::array<std::vector<size_t>, 2> st;
    explicit Streams(const Macro& m) : fin(finalRunEvents(m)) {
        for (size_t i = 0; i < fin.size(); i++) st[fin[i].player & 1].push_back(i);
    }
    // the click whose PRESS is fin[pressIdx]
    RepairClick clickOfPress(size_t pressIdx) const {
        size_t p = fin[pressIdx].player & 1;
        auto& s = st[p];
        size_t pos = size_t(std::find(s.begin(), s.end(), pressIdx) - s.begin());
        RepairClick c;
        c.player = int(p);
        c.index = int(pos / 2);
        c.pressSeq = fin[pressIdx].seq;
        c.pressTick = fin[pressIdx].editedTick;
        if (pos + 1 < s.size() && fin[s[pos + 1]].type == EventType::Release) {
            c.hasRelease = true;
            c.releaseSeq = fin[s[pos + 1]].seq;
            c.releaseTick = fin[s[pos + 1]].editedTick;
        }
        return c;
    }
};
}  // namespace

std::vector<RepairClick> culpritClicks(const Macro& m, int failTick) {
    Streams S(m);
    int last = -1, next = -1;
    for (size_t i = 0; i < S.fin.size(); i++) if (S.fin[i].editedTick <= failTick) last = int(i);
    for (size_t i = 0; i < S.fin.size(); i++) {
        if (S.fin[i].editedTick > failTick && S.fin[i].type == EventType::Press) { next = int(i); break; }
    }
    std::optional<RepairClick> a, b;
    bool holding = false;
    if (last >= 0) {
        size_t p = S.fin[size_t(last)].player & 1;
        auto& s = S.st[p];
        size_t pos = size_t(std::find(s.begin(), s.end(), size_t(last)) - s.begin());
        if (S.fin[size_t(last)].type == EventType::Release) { if (pos > 0) a = S.clickOfPress(s[pos - 1]); }
        else { a = S.clickOfPress(size_t(last)); holding = true; }
    }
    if (next >= 0) b = S.clickOfPress(size_t(next));
    std::vector<RepairClick> out;
    auto add = [&](const std::optional<RepairClick>& c) {
        if (c && std::none_of(out.begin(), out.end(), [&](const RepairClick& o) { return o.pressSeq == c->pressSeq; })) out.push_back(*c);
    };
    if (holding) { add(a); add(b); } else { add(b); add(a); }
    return out;
}

std::vector<int> repairShiftOrder(int maxShift) {
    std::vector<int> v;
    maxShift = std::max(0, maxShift);
    // v0.3.0: by size of the correction, later before earlier on a tie (+1, -1, +2, -2 ...). Before, every later shift was tried before the first
    // earlier one, so a click that was 1 tick too LATE could be "repaired" by moving it 9 ticks later by accident.
    for (int d = 1; d <= maxShift; d++) { v.push_back(d); v.push_back(-d); }
    return v;
}

EditOutcome shiftClick(Macro& m, const RepairClick& c, int shift) {
    if (shift == 0) return {true, ""};
    Macro work = m;  // all or nothing
    EditOutcome r;
    // Moving later: the release goes first (it makes room). Moving earlier: the press goes first.
    auto movePress = [&] { return moveEvent(work, c.pressSeq, c.pressTick + shift); };
    auto moveRelease = [&] { return c.hasRelease ? moveEvent(work, c.releaseSeq, c.releaseTick + shift) : EditOutcome{true, ""}; };
    if (shift > 0) {
        if (!(r = moveRelease()).ok) return r;
        if (!(r = movePress()).ok) return r;
    } else {
        if (!(r = movePress()).ok) return r;
        if (!(r = moveRelease()).ok) return r;
    }
    if (!validateEvents(finalRunEvents(work)).ok) return {false, "The shifted click would break the press/release order."};
    m = std::move(work);
    return {true, ""};
}

std::string describeShift(const RepairClick& c, int shift) {
    int n = std::abs(shift);
    std::string s = "click " + std::to_string(c.index + 1) + " (player " + std::to_string(c.player + 1) + "): moved " + std::to_string(n) +
                    (n == 1 ? " tick " : " ticks ") + (shift > 0 ? "later" : "earlier") + " (tick " + std::to_string(c.pressTick) + " -> " +
                    std::to_string(c.pressTick + shift) + ")";
    return s;
}

}  // namespace gdfwr
