#include "Trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "Session.hpp"

namespace gdfwr {

namespace {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
std::string sf(const char* f, ...) {
    char buf[512];
    va_list a;
    va_start(a, f);
    int n = std::vsnprintf(buf, sizeof buf, f, a);
    va_end(a);
    if (n < 0) return {};
    if (size_t(n) < sizeof buf) return std::string(buf, size_t(n));
    std::string s(size_t(n) + 1, '\0');
    va_start(a, f);
    std::vsnprintf(s.data(), s.size(), f, a);
    va_end(a);
    s.resize(size_t(n));
    return s;
}

bool sameVal(float a, float b, float eps) { return std::fabs(a - b) <= eps; }   // a NaN is never "the same"

std::string flagLetters(uint8_t f) {
    std::string s;
    if (f & TraceFlag::HalfTick) s += 'H';
    if (f & TraceFlag::LastTick) s += 'L';
    return s.empty() ? "-" : s;
}

}  // namespace

const char* traceEndText(TraceEnd e) {
    switch (e) {
        case TraceEnd::Running: return "the replay was still running";
        case TraceEnd::Completed: return "the level was completed";
        case TraceEnd::Died: return "the player died or the level was restarted";
        case TraceEnd::Left: return "the level was left";
        case TraceEnd::Analyze: return "Analyze was started (the replay was stopped)";
    }
    return "?";
}

// ---- RealTrace -------------------------------------------------------------------------------------------------------------------------
void RealTrace::begin(uint64_t hash, int64_t level, std::string name, size_t eventCount) {
    *this = RealTrace{};
    recording = true;
    macroHash = hash;
    levelId = level;
    levelName = std::move(name);
    events = eventCount;
    calls.reserve(30000);
}

int RealTrace::beginCall(int tick, double clock, float dt, float timeWarp, uint8_t flags, Sample const& s) {
    if (!recording) return -1;
    if (calls.size() >= kTraceMaxCalls) { truncated = true; return -1; }
    TraceCall c;
    c.tick = int32_t(tick);
    c.clock = clock;
    c.clockAfter = clock;
    c.dt = dt;
    c.timeWarp = timeWarp;
    c.frame = frame;
    c.flags = flags;
    c.s = s;
    calls.push_back(c);
    return int(calls.size()) - 1;
}

void RealTrace::noteFed(int idx, int first, int count) {
    if (idx < 0 || size_t(idx) >= calls.size() || count <= 0) return;
    calls[size_t(idx)].fedFirst = int32_t(first);
    calls[size_t(idx)].fedCount = uint16_t(std::min(count, 65535));
}

void RealTrace::endCall(int idx, double clockAfter) {
    if (idx < 0 || size_t(idx) >= calls.size()) return;
    calls[size_t(idx)].clockAfter = clockAfter;
}

void RealTrace::noteFrame(float dt) {
    if (!recording) return;
    frameDt.push_back(dt);   // calls with frame == i ran in the display frame whose dt is frameDt[i] (RecHud::update runs after the PlayLayer's update)
    frame++;
}

// ---- the real trace as one sample per tick -----------------------------------------------------------------------------------------------
int TickSeries::ticksWithData() const {
    int n = 0;
    for (auto h : have) n += h ? 1 : 0;
    return n;
}

TickSeries tickSeries(RealTrace const& tr) {
    TickSeries ts;
    int maxTick = -1;
    for (auto const& c : tr.calls) if (c.tick >= 0 && c.tick <= kTraceMaxTick) maxTick = std::max(maxTick, int(c.tick));
    if (maxTick < 0) return ts;
    size_t n = size_t(maxTick) + 1;
    ts.s.assign(n, Sample{});
    ts.have.assign(n, 0);
    ts.call.assign(n, -1);
    ts.count.assign(n, 0);
    for (size_t i = 0; i < tr.calls.size(); i++) {
        auto const& c = tr.calls[i];
        if (c.tick < 0 || c.tick > kTraceMaxTick) continue;
        size_t t = size_t(c.tick);
        if (!ts.have[t]) { ts.have[t] = 1; ts.s[t] = c.s; ts.call[t] = int32_t(i); }   // the FIRST call of a tick = the state at the start of it
        if (ts.count[t] < 65535) ts.count[t]++;
    }
    return ts;
}

TraceStats traceStats(RealTrace const& tr) {
    TraceStats st;
    st.calls = tr.calls.size();
    if (tr.calls.empty()) return st;
    auto ts = tickSeries(tr);
    int first = -1, last = -1;
    for (int t = 0; t < ts.size(); t++) if (ts.have[size_t(t)]) { if (first < 0) first = t; last = t; }
    st.firstTick = std::max(first, 0);
    st.lastTick = std::max(last, 0);
    st.ticksSeen = ts.ticksWithData();
    st.skippedTicks = first < 0 ? 0 : (last - first + 1) - st.ticksSeen;
    st.dtMin = st.dtMax = tr.calls.front().dt;
    st.twMin = st.twMax = tr.calls.front().timeWarp;
    std::vector<double> steps;
    steps.reserve(tr.calls.size());
    std::vector<std::pair<uint32_t, int>> groups;   // (frame id, calls)
    bool havePrev = false;
    int prevTick = 0;
    double prevClock = 0;
    for (size_t i = 0; i < tr.calls.size(); i++) {
        auto const& c = tr.calls[i];
        if (c.flags & TraceFlag::HalfTick) st.halfCalls++;
        if (c.flags & TraceFlag::LastTick) st.lastCalls++;
        if (std::fabs(c.clock - double(std::llround(c.clock))) > 1e-3) st.fractional++;
        bool wild = c.tick < 0 || c.tick > kTraceMaxTick;   // a call that claims an impossible tick is counted once and is not part of the tick / step statistics
        if (wild) st.wild++;
        st.dtMin = std::min(st.dtMin, c.dt);
        st.dtMax = std::max(st.dtMax, c.dt);
        st.twMin = std::min(st.twMin, c.timeWarp);
        st.twMax = std::max(st.twMax, c.timeWarp);
        double adv = c.clockAfter - c.clock;
        if (std::fabs(adv - 1.0) <= 2e-3) st.clockInside1++;
        else if (std::fabs(adv - 0.5) <= 2e-3) st.clockInsideHalf++;
        else if (std::fabs(adv) <= 1e-6) st.clockInsideNone++;
        else st.clockInsideOther++;
        if (!wild) {
            if (havePrev) {
                if (c.tick == prevTick) st.repeatedCalls++;
                else if (c.tick < prevTick) st.backwards++;
                steps.push_back(c.clock - prevClock);
            }
            havePrev = true;
            prevTick = c.tick;
            prevClock = c.clock;
        }
        if (groups.empty() || groups.back().first != c.frame) groups.push_back({c.frame, 0});
        groups.back().second++;
    }
    if (!steps.empty()) {
        st.stepPairs = steps.size();
        for (double d : steps) if (std::fabs(d - 1.0) <= 2e-3) st.stepNormal++;
        std::vector<double> tmp = steps;
        std::nth_element(tmp.begin(), tmp.begin() + std::ptrdiff_t(tmp.size() / 2), tmp.end());
        st.stepMedian = tmp[tmp.size() / 2];
        st.stepMin = *std::min_element(steps.begin(), steps.end());
        st.stepMax = *std::max_element(steps.begin(), steps.end());
        st.tps = st.stepMedian > 1e-9 ? double(kTps) / st.stepMedian : 0.0;
    }
    // display frames: FPS from the frame times, steps per frame from the call counts
    double sumDt = 0, minFps = 1e18;
    for (auto const& g : groups) {
        if (size_t(g.first) >= tr.frameDt.size()) continue;
        double dt = double(tr.frameDt[size_t(g.first)]);
        if (dt <= 0) continue;
        st.frames++;
        sumDt += dt;
        minFps = std::min(minFps, 1.0 / dt);
    }
    if (st.frames > 0 && sumDt > 0) { st.fpsAvg = double(st.frames) / sumDt; st.fpsMin = minFps; }
    std::map<int, int> hist;
    size_t from = groups.size() >= 3 ? 1 : 0, to = groups.size() >= 3 ? groups.size() - 1 : groups.size();   // the first and the last frame are partial
    for (size_t i = from; i < to; i++) hist[groups[i].second]++;
    for (auto const& kv : hist) st.stepsPerFrame.push_back(kv);
    return st;
}

// ---- comparison ------------------------------------------------------------------------------------------------------------------------------
bool sampleDiffers(Sample const& a, Sample const& b, float eps) {
    return !(sameVal(a.x1, b.x1, eps) && sameVal(a.y1, b.y1, eps) && sameVal(a.vy1, b.vy1, eps) && sameVal(a.x2, b.x2, eps) &&
             sameVal(a.y2, b.y2, eps) && sameVal(a.vy2, b.vy2, eps));
}

bool verticalDiffers(Sample const& a, Sample const& b, float eps) {
    return !(sameVal(a.y1, b.y1, eps) && sameVal(a.vy1, b.vy1, eps) && sameVal(a.y2, b.y2, eps) && sameVal(a.vy2, b.vy2, eps));
}

namespace {
// The shift test and its verdict: silent[t] against real[t + k], k = -3..+3, on the whole state (x, y, yVel of both players) or on the vertical
// movement only (y, yVel). The verdict rules are the same for both.
struct ShiftAnalysis {
    std::vector<ShiftStat> shifts;     // k = -3 .. +3, in that order
    int firstOver = -1;                // first tick that differs by more than epsilon at k = 0
    int bestK = 0;                     // most equal ticks (ties: the smaller |k|, then the positive one)
    TraceVerdict verdict = TraceVerdict::NoData;
    int verdictK = 0;
};

ShiftAnalysis analyseShifts(std::vector<Sample> const& sim, TickSeries const& real, float eps, bool vertical) {
    ShiftAnalysis A;
    auto differs = [&](Sample const& a, Sample const& b) { return vertical ? verticalDiffers(a, b, eps) : sampleDiffers(a, b, eps); };
    const int nShift = 2 * kTraceMaxShift + 1;
    A.shifts.resize(size_t(nShift));
    for (int i = 0; i < nShift; i++) A.shifts[size_t(i)].k = i - kTraceMaxShift;
    const int simTicks = int(sim.size());
    const int overlap = std::min(simTicks, real.size());
    for (int t = 0; t < overlap; t++) {
        if (real.has(t) && differs(sim[size_t(t)], real.s[size_t(t)])) { A.firstOver = t; break; }
    }
    for (auto& sh : A.shifts) {
        int run = 0;
        for (int t = 0; t < simTicks; t++) {
            int u = t + sh.k;
            if (!real.has(u)) { run = 0; continue; }
            sh.compared++;
            bool tail = A.firstOver >= 0 && t >= A.firstOver;
            if (tail) sh.tailCompared++;
            if (!differs(sim[size_t(t)], real.s[size_t(u)])) {
                sh.matched++;
                if (tail) sh.tailMatched++;
                run++;
                if (run > sh.runLen) { sh.runLen = run; sh.runStart = t - run + 1; }
            } else {
                run = 0;
                if (sh.firstMismatch < 0) sh.firstMismatch = t;
            }
        }
    }
    // the shift with the most equal ticks (ties: the smaller |k|, then the positive one)
    size_t best = size_t(kTraceMaxShift);
    for (size_t i = 0; i < A.shifts.size(); i++) {
        auto const& a = A.shifts[i];
        auto const& b = A.shifts[best];
        if (a.matched > b.matched || (a.matched == b.matched && (std::abs(a.k) < std::abs(b.k) || (std::abs(a.k) == std::abs(b.k) && a.k > b.k)))) best = i;
    }
    A.bestK = A.shifts[best].k;
    // the verdict
    if (A.firstOver < 0) {
        A.verdict = TraceVerdict::Identical;
        return A;
    }
    int wholeK = 0;
    double wholeFrac = 0;
    for (auto const& sh : A.shifts) {
        if (sh.k == 0 || sh.compared < 8) continue;
        double f = double(sh.matched) / double(sh.compared);
        if (f > wholeFrac + 1e-12 || (std::fabs(f - wholeFrac) <= 1e-12 && std::abs(sh.k) < std::abs(wholeK))) { wholeFrac = f; wholeK = sh.k; }
    }
    ShiftStat const& s0 = A.shifts[size_t(kTraceMaxShift)];
    if (wholeK != 0 && wholeFrac >= kTraceShiftWholeFrac) {
        A.verdict = TraceVerdict::ShiftedWhole;
        A.verdictK = wholeK;
    } else if (s0.tailCompared < kTraceTailMin) {
        A.verdict = TraceVerdict::TooShortToTell;
    } else {
        int tailK = 0;
        double tailFrac = 0;
        for (auto const& sh : A.shifts) {
            if (sh.k == 0 || sh.tailCompared < kTraceTailMin) continue;
            double f = double(sh.tailMatched) / double(sh.tailCompared);
            if (f > tailFrac + 1e-12 || (std::fabs(f - tailFrac) <= 1e-12 && std::abs(sh.k) < std::abs(tailK))) { tailFrac = f; tailK = sh.k; }
        }
        if (tailK != 0 && tailFrac >= kTraceShiftTailFrac && A.shifts[size_t(tailK + kTraceMaxShift)].tailMatched > s0.tailMatched) {
            A.verdict = TraceVerdict::ShiftedFromFirstDiff;
            A.verdictK = tailK;
        } else {
            A.verdict = TraceVerdict::NotAShift;
        }
    }
    return A;
}
}  // namespace

TraceCompare compareSamples(std::vector<Sample> const& sim, TickSeries const& real, float eps, int deathTick, Sample const* deathAfter) {
    TraceCompare c;
    c.eps = eps;
    c.simTicks = int(sim.size());
    c.realTicks = real.size();
    const int nShift = 2 * kTraceMaxShift + 1;
    c.shifts.resize(size_t(nShift));
    for (int i = 0; i < nShift; i++) c.shifts[size_t(i)].k = i - kTraceMaxShift;
    c.vshifts = c.shifts;
    if (sim.empty()) { c.why = "the silent replay recorded no samples"; return c; }
    if (real.ticksWithData() == 0) { c.why = "the real playback trace has no ticks"; return c; }
    int overlap = std::min(c.simTicks, c.realTicks);
    c.simBeyondReal = c.simTicks - overlap;
    for (int t = 0; t < overlap; t++) {
        if (!real.has(t)) { c.missingReal++; continue; }
        c.compared++;
        Sample const& a = sim[size_t(t)];
        Sample const& b = real.s[size_t(t)];
        if (c.firstAny < 0 && sampleDiffers(a, b, 0.f)) c.firstAny = t;
        if (c.xFirstOver < 0 && !(sameVal(a.x1, b.x1, eps) && sameVal(a.x2, b.x2, eps))) c.xFirstOver = t;
        if (sampleDiffers(a, b, eps)) {
            c.overCount++;
            if (c.firstOver < 0) { c.firstOver = t; c.simAt = a; c.realAt = b; }
        }
    }
    if (c.compared == 0) { c.why = "no tick of the silent replay is also a tick of the real trace"; return c; }
    c.ok = true;
    {
        ShiftAnalysis whole = analyseShifts(sim, real, eps, false);   // x, y, yVel of both players
        c.shifts = std::move(whole.shifts);
        c.bestK = whole.bestK;
        c.verdict = whole.verdict;
        c.verdictK = whole.verdictK;
        ShiftAnalysis vert = analyseShifts(sim, real, eps, true);     // y, yVel only
        c.vshifts = std::move(vert.shifts);
        c.vFirstOver = vert.firstOver;
        c.vbestK = vert.bestK;
        c.vverdict = vert.verdict;
        c.vverdictK = vert.verdictK;
    }
    if (deathTick >= 0 && deathAfter) {
        c.deathKnown = true;
        c.deathTick = deathTick;
        c.deathSim = *deathAfter;
        if (real.has(deathTick + 1)) {
            c.deathRealHave = true;
            c.realContinued = true;
            c.deathReal = real.s[size_t(deathTick + 1)];
            c.deathEqual = !sampleDiffers(*deathAfter, c.deathReal, eps);
        }
    }
    return c;
}

// ---- clicks nearest the death -------------------------------------------------------------------------------------------------------------
namespace {

// the index of the most equal ticks in a per-shift count (ties: the smaller |k|, then the positive one)
size_t bestShiftIndex(std::vector<int> const& matched) {
    size_t best = size_t(kTraceMaxShift);
    for (size_t i = 0; i < matched.size(); i++) {
        int ki = int(i) - kTraceMaxShift, kb = int(best) - kTraceMaxShift;
        if (matched[i] > matched[best] || (matched[i] == matched[best] && (std::abs(ki) < std::abs(kb) || (std::abs(ki) == std::abs(kb) && ki > kb)))) best = i;
    }
    return best;
}

// The paragraph about the VERTICAL movement (y, yVel; x ignored) that follows a "NOT a tick shift" result. What the whole-state test cannot see: a click that takes
// effect k ticks earlier in one run than in the other, while the horizontal movement stays in step.
std::string verticalVerdictText(TraceCompare const& C) {
    std::string xs = C.xFirstOver < 0 ? "x is equal at every compared tick" : sf("x first differs at tick %d", C.xFirstOver);
    auto whenText = [](int k) { return k > 0 ? std::string("EARLIER in the silent replay than in the real playback") : std::string("LATER in the silent replay than in the real playback"); };
    switch (C.vverdict) {
        case TraceVerdict::Identical:
            return sf("VERTICAL ONLY (y and yVel; x ignored): no difference at any compared tick - only x differs (first at tick %d). That points at the horizontal movement, not at when a click counts (suspected, not proven).",
                      C.xFirstOver);
        case TraceVerdict::ShiftedWhole:
            return sf("VERTICAL ONLY (y and yVel; x ignored): the vertical movement of the silent replay equals the real one SHIFTED by k=%+d (silent[t] == real[t%+d]) over the whole run; %s. "
                      "The vertical change comes %d tick(s) %s. %s",
                      C.vverdictK, C.vverdictK, xs.c_str(), std::abs(C.vverdictK), whenText(C.vverdictK).c_str(),
                      C.xFirstOver < 0 ? "That is the pattern of an input that takes effect that many ticks apart in the two runs while the horizontal movement stays in step (suspected, not proven)."
                                       : "x differs as well, so this explains only part of the difference (suspected, not proven).");
        case TraceVerdict::ShiftedFromFirstDiff:
            return sf("VERTICAL ONLY (y and yVel; x ignored): equal until tick %d; from tick %d on the vertical movement of the silent replay equals the real one SHIFTED by k=%+d (silent[t] == real[t%+d]); %s. "
                      "The vertical change comes %d tick(s) %s. %s",
                      C.vFirstOver - 1, C.vFirstOver, C.vverdictK, C.vverdictK, xs.c_str(), std::abs(C.vverdictK), whenText(C.vverdictK).c_str(),
                      C.xFirstOver < 0 ? "That is the pattern of an input that takes effect that many ticks apart in the two runs while the horizontal movement stays in step (suspected, not proven)."
                                       : "x differs as well, so this explains only part of the difference (suspected, not proven).");
        case TraceVerdict::TooShortToTell:
            return sf("Vertical only (y and yVel; x ignored): they first differ at tick %d, but too few ticks follow it to tell a shift from different physics.", C.vFirstOver);
        default:
            return sf("Vertical only (y and yVel; x ignored): no shift in -%d..+%d explains it either (%s). The two runs differ in more than timing: a different frame structure or different physics at that point (suspected, not proven).",
                      kTraceMaxShift, kTraceMaxShift, xs.c_str());
    }
}

LocalCompare localCompare(std::vector<Sample> const& sim, TickSeries const& real, int lo, int hi, float eps) {
    LocalCompare lc;
    const int nShift = 2 * kTraceMaxShift + 1;
    lc.matchedPerShift.assign(size_t(nShift), 0);
    lc.vMatchedPerShift.assign(size_t(nShift), 0);
    lo = std::max(lo, 0);
    hi = std::min(hi, int(sim.size()) - 1);
    lc.lo = lo;
    lc.hi = hi;
    for (int t = lo; t <= hi; t++) {
        Sample const& a = sim[size_t(t)];
        if (real.has(t)) {
            lc.compared++;
            if (lc.firstOver < 0 && sampleDiffers(a, real.s[size_t(t)], eps)) { lc.firstOver = t; lc.simAt = a; lc.realAt = real.s[size_t(t)]; }
        }
        for (int i = 0; i < nShift; i++) {
            int u = t + i - kTraceMaxShift;
            if (!real.has(u)) continue;
            if (!sampleDiffers(a, real.s[size_t(u)], eps)) lc.matchedPerShift[size_t(i)]++;
            if (!verticalDiffers(a, real.s[size_t(u)], eps)) lc.vMatchedPerShift[size_t(i)]++;
        }
    }
    size_t best = bestShiftIndex(lc.matchedPerShift);
    lc.bestK = int(best) - kTraceMaxShift;
    lc.bestMatched = lc.matchedPerShift[best];
    size_t vbest = bestShiftIndex(lc.vMatchedPerShift);
    lc.vBestK = int(vbest) - kTraceMaxShift;
    lc.vBestMatched = lc.vMatchedPerShift[vbest];
    return lc;
}

// "2394(L f612) 2395(L f613 +1)": the real calls around a tick (the clock is added when it is not a whole tick)
std::string callsAround(RealTrace const& tr, TickSeries const& real, int tick) {
    int firstCall = -1;
    for (int t = std::max(0, tick - 1); t <= tick + 1 && firstCall < 0; t++) if (real.has(t)) firstCall = real.call[size_t(t)];
    if (firstCall < 0) return "(no real call near this tick)";
    std::string s;
    int shown = 0;
    for (size_t i = size_t(firstCall); i < tr.calls.size() && shown < 8; i++) {
        auto const& c = tr.calls[i];
        if (c.tick > tick + 1) break;
        if (c.tick < tick - 1) continue;
        if (!s.empty()) s += " ";
        s += std::to_string(c.tick);
        if (std::fabs(c.clock - double(std::llround(c.clock))) > 1e-3) s += sf("@%.3f", c.clock);
        s += "(" + flagLetters(c.flags) + sf(" f%u", unsigned(c.frame));
        if (c.fedCount) s += sf(" +%u", unsigned(c.fedCount));
        s += ")";
        shown++;
    }
    return s;
}

}  // namespace

std::vector<ClickFacts> nearestClickFacts(Macro const& macro, std::vector<Sample> const& sim, TickSeries const& real, RealTrace const& tr, int refTick,
                                          int n, float eps) {
    std::vector<ClickFacts> out;
    auto ev = finalRunEvents(macro);
    struct Ck { int player, number, pressIdx, releaseIdx, dist; };
    std::vector<Ck> cks;
    int num[2] = {0, 0};
    for (size_t i = 0; i < ev.size(); i++) {
        if (ev[i].type != EventType::Press) continue;
        int p = ev[i].player ? 1 : 0;
        Ck c{p, ++num[p], int(i), -1, 0};
        for (size_t j = i + 1; j < ev.size(); j++) {
            if ((ev[j].player ? 1 : 0) != p) continue;
            if (ev[j].type == EventType::Release) c.releaseIdx = int(j);
            break;   // the next event of this player is the release (or, for a broken macro, another press)
        }
        int pt = ev[i].editedTick;
        int rt = c.releaseIdx >= 0 ? ev[size_t(c.releaseIdx)].editedTick : -1;
        c.dist = refTick < pt ? pt - refTick : (rt >= 0 && refTick > rt ? refTick - rt : 0);
        cks.push_back(c);
    }
    std::stable_sort(cks.begin(), cks.end(), [](Ck const& a, Ck const& b) { return a.dist < b.dist; });
    if (int(cks.size()) > n) cks.resize(size_t(std::max(n, 0)));
    // which real call applied which input
    std::vector<int> eventCall(ev.size(), -1);
    for (size_t i = 0; i < tr.calls.size(); i++) {
        auto const& c = tr.calls[i];
        for (int k = 0; k < int(c.fedCount); k++) {
            int idx = int(c.fedFirst) + k;
            if (c.fedFirst >= 0 && idx >= 0 && size_t(idx) < eventCall.size() && eventCall[size_t(idx)] < 0) eventCall[size_t(idx)] = int(i);
        }
    }
    auto edge = [&](int idx, bool press) {
        EdgeFacts e;
        e.press = press;
        e.tick = ev[size_t(idx)].editedTick;
        int call = eventCall[size_t(idx)];
        if (call >= 0) {
            e.realApplied = true;
            e.realCall = call;
            e.realTick = int(tr.calls[size_t(call)].tick);
            e.realFlags = tr.calls[size_t(call)].flags;
        }
        e.local = localCompare(sim, real, e.tick - kTraceEdgeBefore, e.tick + kTraceEdgeAfter, eps);
        e.callsAround = callsAround(tr, real, e.tick);
        return e;
    };
    for (auto const& c : cks) {
        ClickFacts f;
        f.player = c.player;
        f.number = c.number;
        f.pressTick = ev[size_t(c.pressIdx)].editedTick;
        f.distance = c.dist;
        f.press = edge(c.pressIdx, true);
        if (c.releaseIdx >= 0) {
            f.hasRelease = true;
            f.releaseTick = ev[size_t(c.releaseIdx)].editedTick;
            f.release = edge(c.releaseIdx, false);
        }
        out.push_back(std::move(f));
    }
    return out;
}

// ---- text -------------------------------------------------------------------------------------------------------------------------------------
std::string sampleText(Sample const& s, int player, bool withNames) {
    float x = player ? s.x2 : s.x1, y = player ? s.y2 : s.y1, v = player ? s.vy2 : s.vy1;
    return withNames ? sf("x %.4f  y %.4f  yVel %.4f", double(x), double(y), double(v)) : sf("%.4f, %.4f, %.4f", double(x), double(y), double(v));
}

std::string stepsPerFrameText(std::vector<std::pair<int, int>> const& v, size_t maxItems) {
    if (v.empty()) return "n/a";
    std::vector<std::pair<int, int>> byCount = v;
    std::stable_sort(byCount.begin(), byCount.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    if (byCount.size() > maxItems) byCount.resize(maxItems);
    std::sort(byCount.begin(), byCount.end());
    std::string s;
    for (auto const& kv : byCount) { if (!s.empty()) s += ", "; s += sf("%d x %d", kv.first, kv.second); }
    if (v.size() > byCount.size()) s += sf(", ... (%zu different sizes: %d..%d)", v.size(), v.front().first, v.back().first);
    return s;
}

std::vector<std::string> wrapLines(std::string const& text, size_t width, std::string const& indent) {
    std::vector<std::string> out;
    if (width < indent.size() + 2) width = indent.size() + 2;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        while (line.size() > width) {
            size_t cut = line.rfind(' ', width);
            if (cut == std::string::npos || cut < width / 2) cut = width;
            out.push_back(line.substr(0, cut));
            size_t next = cut;
            while (next < line.size() && line[next] == ' ') next++;
            line = indent + line.substr(next);
        }
        out.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

std::vector<std::string> describeRealTrace(RealTrace const& tr) {
    std::vector<std::string> v;
    if (tr.calls.empty()) { v.push_back("real playback: no processCommands call was recorded"); return v; }
    auto st = traceStats(tr);
    v.push_back(sf("real playback recorded: %zu calls, ticks %d..%d (%d different ticks), ended: %s%s", st.calls, st.firstTick, st.lastTick, st.ticksSeen,
                   traceEndText(tr.end), tr.truncated ? sf(" (TRACE CUT at %zu calls: the rest of the playback is not in it)", kTraceMaxCalls).c_str() : ""));
    v.push_back(sf("  frame structure: %zu display frames with steps; steps per frame: %s; half-tick calls %zu, last-tick calls %zu, calls on a fractional clock %zu",
                   st.frames, stepsPerFrameText(st.stepsPerFrame).c_str(), st.halfCalls, st.lastCalls, st.fractional));
    v.push_back(sf("  tick counter: skipped ticks %d, calls on the same tick as the call before %d, calls that went back %d, calls out of range %d", st.skippedTicks,
                   st.repeatedCalls, st.backwards, st.wild));
    v.push_back(sf("  level clock: the game's own processCommands moved it by 1 tick in %zu of %zu calls (by 0.5 tick in %zu, not at all in %zu, other %zu)",
                   st.clockInside1, st.calls, st.clockInsideHalf, st.clockInsideNone, st.clockInsideOther));
    if (st.stepPairs > 0)
        v.push_back(sf("  step length: consecutive calls are %.4f ticks apart (median; min %.4f, max %.4f; %zu of %zu within 0.2 %% of 1.0) = %.1f TPS", st.stepMedian,
                       st.stepMin, st.stepMax, st.stepNormal, st.stepPairs, st.tps));
    v.push_back(sf("  FPS (display frames that ran steps): avg %.1f, min %.1f; dt of the calls %.8f .. %.8f (1/240 = %.8f); time warp %.3f .. %.3f", st.fpsAvg, st.fpsMin,
                   double(st.dtMin), double(st.dtMax), 1.0 / 240.0, double(st.twMin), double(st.twMax)));
    return v;
}

namespace {

std::string num9(float v) { return sf("%.9g", double(v)); }

std::string callsCsvOf(RealTrace const& tr) {
    std::string s = "call,tick,clock,clock_after,dt,time_warp,frame,flags,fed_first,fed_count,x1,y1,yvel1,x2,y2,yvel2\n";
    s.reserve(tr.calls.size() * 110 + 100);
    for (size_t i = 0; i < tr.calls.size(); i++) {
        auto const& c = tr.calls[i];
        s += sf("%zu,%d,%.6f,%.6f,%.9g,%.9g,%u,%s,%d,%u,", i, int(c.tick), c.clock, c.clockAfter, double(c.dt), double(c.timeWarp), unsigned(c.frame),
                flagLetters(c.flags).c_str(), int(c.fedFirst), unsigned(c.fedCount));
        s += num9(c.s.x1) + "," + num9(c.s.y1) + "," + num9(c.s.vy1) + "," + num9(c.s.x2) + "," + num9(c.s.y2) + "," + num9(c.s.vy2) + "\n";
    }
    return s;
}

std::string compareCsvOf(std::vector<Sample> const& sim, TickSeries const& real, RealTrace const& tr, float eps) {
    std::string s =
        "tick,silent_x1,silent_y1,silent_yvel1,real_x1,real_y1,real_yvel1,diff_x1,diff_y1,diff_yvel1,differs,real_calls,real_flags,real_frame,real_fed,"
        "silent_x2,silent_y2,silent_yvel2,real_x2,real_y2,real_yvel2\n";
    int n = std::max(int(sim.size()), real.size());
    s.reserve(size_t(n) * 150 + 200);
    for (int t = 0; t < n; t++) {
        bool hs = t < int(sim.size()), hr = real.has(t);
        s += std::to_string(t) + ",";
        if (hs) s += num9(sim[size_t(t)].x1) + "," + num9(sim[size_t(t)].y1) + "," + num9(sim[size_t(t)].vy1) + ","; else s += ",,,";
        if (hr) s += num9(real.s[size_t(t)].x1) + "," + num9(real.s[size_t(t)].y1) + "," + num9(real.s[size_t(t)].vy1) + ","; else s += ",,,";
        if (hs && hr) {
            auto const& a = sim[size_t(t)];
            auto const& b = real.s[size_t(t)];
            s += num9(a.x1 - b.x1) + "," + num9(a.y1 - b.y1) + "," + num9(a.vy1 - b.vy1) + "," + (sampleDiffers(a, b, eps) ? "1" : "0") + ",";
        } else {
            s += ",,,,";
        }
        if (hr) {
            auto const& c = tr.calls[size_t(real.call[size_t(t)])];
            s += sf("%u,%s,%u,%u,", unsigned(real.count[size_t(t)]), flagLetters(c.flags).c_str(), unsigned(c.frame), unsigned(c.fedCount));
        } else {
            s += ",,,,";
        }
        if (hs) s += num9(sim[size_t(t)].x2) + "," + num9(sim[size_t(t)].y2) + "," + num9(sim[size_t(t)].vy2) + ","; else s += ",,,";
        if (hr) s += num9(real.s[size_t(t)].x2) + "," + num9(real.s[size_t(t)].y2) + "," + num9(real.s[size_t(t)].vy2); else s += ",,";
        s += "\n";
    }
    return s;
}

}  // namespace

// ---- the report ----------------------------------------------------------------------------------------------------------------------------------
TraceReport buildTraceReport(TraceInput const& in) {
    TraceReport R;
    SimRun const& S = in.sim;
    auto both = [&](std::string const& s) { R.result.push_back(s); R.log.push_back(s); };   // result screen + log
    auto all = [&](std::string const& s) { R.popup.push_back(s); R.result.push_back(s); R.log.push_back(s); };

    // ---- what the two sides did ------------------------------------------------------------------------------------------------------------
    std::string simEnd = S.died ? sf("died during tick %d", S.deathTick) : (S.endTick >= 0 ? sf("reached the end of the level at tick %d", S.endTick) : std::string("ended without dying and without reaching the end"));
    std::string simLine = sf("Silent replay: %zu ticks (0..%d), %s%s", S.samples.size(), int(S.samples.size()) - 1, simEnd.c_str(),
                             S.died && !S.killer.empty() ? (" - " + S.killer).c_str() : "");
    bool haveReal = in.real && in.unavailable.empty() && in.real->any();
    TraceStats st;
    TickSeries series;
    if (haveReal) { st = traceStats(*in.real); series = tickSeries(*in.real); }
    R.stats = st;

    R.log.push_back(sf("===== silent replay vs real playback (GDFWR %s) =====", in.modVersion.c_str()));
    R.log.push_back(sf("level: %s [%lld], %d inputs in the macro, two-player %s; tick t = the state at the START of tick t, before its inputs are applied (both sides)",
                       in.file.levelName.c_str(), static_cast<long long>(in.file.levelId), in.file.inputs, in.file.twoPlayer ? "yes" : "no"));
    R.log.push_back(simLine);
    if (haveReal) {
        R.log.push_back(sf("Real playback: %zu calls, ticks %d..%d, ended: %s%s", st.calls, st.firstTick, st.lastTick, traceEndText(in.real->end),
                           in.real->truncated ? " (trace cut: the playback was longer than the trace limit)" : ""));
    }

    // ---- no comparison -------------------------------------------------------------------------------------------------------------------------
    std::string settings1, settings2, settings3, settings4;   // filled below, used by every path
    auto stepModeText = [&] { return (S.stepMode.empty() ? std::string("?") : S.stepMode) + (S.stepKick ? " + start kick" : ""); };
    auto fpsReal = [&] { return haveReal && st.frames > 0 ? sf("real playback avg %.1f, min %.1f", st.fpsAvg, st.fpsMin) : std::string("real playback: no data"); };
    auto tpsReal = [&] { return haveReal && st.tps > 0 ? sf("real playback %.1f", st.tps) : std::string("real playback: no data"); };
    std::string recFps = in.file.fpsAvg > 0 ? sf("; the recording was made at avg %.1f, min %.1f FPS", double(in.file.fpsAvg), double(in.file.fpsMin)) : std::string();
    std::string cbsFile = in.file.clickResolution > 1 ? sf("the file says clicks are exact to %d ticks only (COARSE_INPUT)", in.file.clickResolution) : std::string("the file says clicks are exact to 1 tick");
    if (in.file.cbf) cbsFile += ", recorded with click-between-steps";
    settings1 = "Step mode of the silent replay: " + stepModeText() + (S.stepProbe.empty() ? std::string() : " (start-up check: " + S.stepProbe + ")");
    settings2 = "FPS: " + fpsReal() + recFps;
    settings3 = sf("TPS: %s; silent replay 240 (every step is 1/240 s); the file says %d", tpsReal().c_str(), in.file.tps);
    settings4 = std::string("Click between steps (setting \"Clicks are exact to 1 tick\"): ") + (in.clickBetweenSteps ? "ON" : "OFF") + "; " + cbsFile;
    std::string settingsShort = "Step mode: " + stepModeText() + "   FPS (" + fpsReal() + ")   TPS (" + tpsReal() + ")   click-between-steps: " + (in.clickBetweenSteps ? "ON" : "OFF") +
                                (in.file.clickResolution > 1 ? sf(" (file: clicks exact to %d ticks)", in.file.clickResolution) : std::string());

    if (!haveReal) {
        std::string why = !in.unavailable.empty() ? in.unavailable : "No real playback was recorded for this macro in this session.";
        R.headline = "no comparison: " + why;
        std::string head = "REAL PLAYBACK vs SILENT REPLAY: no comparison. " + why;
        for (auto const& l : wrapLines(head, 120)) all(l);
        all("To measure it: press Replay in the GDFWR panel, let the macro play (to the end of the level, or at least past the jump that fails), then press Analyze.");
        for (auto const* s : {&settings1, &settings3, &settings4}) both(*s);
        R.popup.push_back(settingsShort);
        return R;
    }

    for (auto const& l : describeRealTrace(*in.real)) R.log.push_back(l);

    // ---- the comparison ---------------------------------------------------------------------------------------------------------------------
    R.cmp = compareSamples(S.samples, series, in.eps, S.died ? S.deathTick : -1, S.hasDeathSample ? &S.deathSample : nullptr);
    TraceCompare const& C = R.cmp;
    if (!C.ok) {
        R.headline = "no comparison: " + C.why;
        for (auto const& l : wrapLines("REAL PLAYBACK vs SILENT REPLAY: no comparison - " + C.why + ".", 120)) all(l);
        both(simLine);
        both(settings1);
        R.popup.push_back(settingsShort);
        return R;
    }
    R.compared = true;
    bool twoP = in.file.twoPlayer || C.simAt.x2 != 0 || C.realAt.x2 != 0 || C.simAt.y2 != 0 || C.realAt.y2 != 0;

    std::vector<std::string> head;   // the short block (popup + result + log)
    head.push_back(sf("REAL PLAYBACK vs SILENT REPLAY (same macro, both from the level start, tick = start of the tick before its inputs; values differ when |a - b| > %g)", double(C.eps)));
    head.push_back(sf("compared %d ticks (0..%d) of the silent replay (%d ticks); the real playback ran to tick %d%s", C.compared, std::min(C.simTicks, C.realTicks) - 1,
                      C.simTicks, st.lastTick, C.missingReal ? sf("; %d silent tick(s) have no real data", C.missingReal).c_str() : ""));
    if (C.firstOver >= 0) {
        R.headline = sf("first difference at tick %d", C.firstOver);
        head.push_back(sf("FIRST TICK WHERE THEY DIFFER: tick %d%s", C.firstOver,
                          C.firstAny >= 0 && C.firstAny < C.firstOver ? sf("   (a difference of any size, below the limit, starts at tick %d)", C.firstAny).c_str() : ""));
        for (int p = 0; p < (twoP ? 2 : 1); p++) {
            head.push_back(sf("  silent replay%s: %s", twoP ? (p ? " P2" : " P1") : "", sampleText(C.simAt, p).c_str()));
            head.push_back(sf("  real playback%s: %s", twoP ? (p ? " P2" : " P1") : "", sampleText(C.realAt, p).c_str()));
            float dx = (p ? C.simAt.x2 - C.realAt.x2 : C.simAt.x1 - C.realAt.x1), dy = (p ? C.simAt.y2 - C.realAt.y2 : C.simAt.y1 - C.realAt.y1),
                  dv = (p ? C.simAt.vy2 - C.realAt.vy2 : C.simAt.vy1 - C.realAt.vy1);
            head.push_back(sf("  difference (silent - real): dx %+.4f  dy %+.4f  dyVel %+.4f", double(dx), double(dy), double(dv)));
        }
        head.push_back(sf("  %d of the %d compared ticks differ by more than the limit", C.overCount, C.compared));
    } else {
        R.headline = "identical at every compared tick";
        head.push_back(sf("NO DIFFERENCE: the two traces are equal (within %g) at all %d compared ticks%s.", double(C.eps), C.compared,
                          C.firstAny >= 0 ? sf(" (not bit-identical from tick %d on, below the limit)", C.firstAny).c_str() : " (bit-identical)"));
    }
    if (C.deathKnown) {
        head.push_back(sf("The silent replay DIED during tick %d%s.", C.deathTick,
                          C.realContinued ? sf("; the real playback went on (it has tick %d)", C.deathTick + 1).c_str() : "; the real playback has no later tick"));
        if (C.deathRealHave) {
            head.push_back("  silent replay right after the dying step: " + sampleText(C.deathSim, 0));
            head.push_back("  real playback at the start of tick " + std::to_string(C.deathTick + 1) + ":   " + sampleText(C.deathReal, 0) +
                           (C.deathEqual ? "   -> EQUAL: the movement of that step is the same, only the decision to die differs" : "   -> DIFFERENT: the movement of that step already differs"));
        }
        if (C.verdict == TraceVerdict::Identical && C.realContinued)
            head.push_back("The states at the start of every tick are equal, so the difference is INSIDE the step (what is hit, or when an input counts), not a different clock.");
    }
    // the shift test, on the whole state and on the vertical movement only
    std::vector<std::string> shiftLines;
    auto shiftLine = [&](ShiftStat const& sh, int firstOver) {
        std::string line = sf("  k=%+d: %d of %d ticks equal", sh.k, sh.matched, sh.compared);
        line += sh.firstMismatch >= 0 ? sf(", first unequal tick %d", sh.firstMismatch) : std::string(", none unequal");
        if (sh.runLen > 0) line += sf(", longest equal run %d ticks (%d..%d)", sh.runLen, sh.runStart, sh.runStart + sh.runLen - 1);
        if (firstOver >= 0) line += sf("; from tick %d on: %d of %d", firstOver, sh.tailMatched, sh.tailCompared);
        return line;
    };
    shiftLines.push_back(sf("Shift test: silent[t] against real[t + k], k = -%d..+%d (k > 0: the silent replay is AHEAD of the real game, k < 0: BEHIND)", kTraceMaxShift, kTraceMaxShift));
    for (auto const& sh : C.shifts) shiftLines.push_back(shiftLine(sh, C.firstOver));
    shiftLines.push_back(sf("Vertical-only shift test (y and yVel; x ignored), the same k: silent[t] against real[t + k] (k > 0: the silent replay's vertical movement comes EARLIER than the real one)"));
    for (auto const& sh : C.vshifts) shiftLines.push_back(shiftLine(sh, C.vFirstOver));
    std::string verdict;
    switch (C.verdict) {
        case TraceVerdict::Identical:
            verdict = "RESULT: no shift and no difference at the start of any tick.";
            break;
        case TraceVerdict::ShiftedWhole:
            verdict = sf("RESULT: the silent replay equals the real trace SHIFTED by k=%+d (silent[t] == real[t%+d]) over the whole run: it is %d tick(s) %s the real game. "
                         "A constant shift like this points at a clock or start offset (suspected, not proven).",
                         C.verdictK, C.verdictK, std::abs(C.verdictK), C.verdictK > 0 ? "AHEAD of" : "BEHIND");
            break;
        case TraceVerdict::ShiftedFromFirstDiff:
            verdict = sf("RESULT: equal until tick %d; from tick %d on the silent replay equals the real trace SHIFTED by k=%+d (it is %d tick(s) %s the real game): "
                         "it lost or gained a tick there (suspected cause: a step that one side takes and the other does not).",
                         C.firstOver - 1, C.firstOver, C.verdictK, std::abs(C.verdictK), C.verdictK > 0 ? "AHEAD of" : "BEHIND");
            break;
        case TraceVerdict::TooShortToTell:
            verdict = sf("RESULT: they differ from tick %d, but only %d tick(s) follow it (the silent replay ended there): too few to tell a shifted clock from different physics.",
                         C.firstOver, C.shifts[size_t(kTraceMaxShift)].tailCompared);
            break;
        case TraceVerdict::NotAShift:
            if (C.overCount <= kTraceBlipMax) {
                verdict = sf("RESULT: only %d tick(s) differ (the first is tick %d); before and after them the two runs are equal again. That is a one-off difference, not a shifted clock.",
                             C.overCount, C.firstOver);
                break;
            }
            verdict = sf("RESULT: NOT a tick shift: no k in -%d..+%d makes the whole state (x, y, yVel) equal after tick %d (the best over the whole run is k=%+d with %d of %d ticks equal).",
                         kTraceMaxShift, kTraceMaxShift, C.firstOver, C.bestK, C.shifts[size_t(C.bestK + kTraceMaxShift)].matched, C.shifts[size_t(C.bestK + kTraceMaxShift)].compared);
            verdict += "\n" + verticalVerdictText(C);
            break;
        case TraceVerdict::NoData:
            verdict = "RESULT: nothing to compare.";
            break;
    }

    // popup: short
    for (auto const& l : head) { R.popup.push_back(l); }
    for (auto const& l : wrapLines(verdict, 120)) R.popup.push_back(l);
    R.popup.push_back(settingsShort);
    R.popup.push_back("The full report is in the Geode log (lines starting with GDFWR-TRACE) and on the result screen; the numbers are in two .csv files next to your recordings.");
    // result screen + log: everything but the per-click block
    for (auto const& l : head) { R.result.push_back(l); R.log.push_back(l); }
    for (auto const& l : shiftLines) { R.result.push_back(l); R.log.push_back(l); }
    for (auto const& l : wrapLines(verdict, 120)) { R.result.push_back(l); R.log.push_back(l); }

    // context table around the first difference (log only)
    if (C.firstOver >= 0) {
        R.log.push_back(sf("context around tick %d (k = 0; P1 x, y, yVel):", C.firstOver));
        for (int t = std::max(0, C.firstOver - 3); t <= C.firstOver + 3 && t < C.simTicks; t++) {
            if (!series.has(t)) { R.log.push_back(sf("  tick %d: silent (%s)   real: no data", t, sampleText(S.samples[size_t(t)], 0, false).c_str())); continue; }
            bool d = sampleDiffers(S.samples[size_t(t)], series.s[size_t(t)], C.eps);
            R.log.push_back(sf("  tick %d: silent (%s)   real (%s)   %s", t, sampleText(S.samples[size_t(t)], 0, false).c_str(), sampleText(series.s[size_t(t)], 0, false).c_str(),
                               d ? (t == C.firstOver ? "DIFFERENT  <-- first" : "DIFFERENT") : "equal"));
        }
    }

    // frame structure, both sides (result screen + log)
    std::vector<std::string> structure;
    structure.push_back(sf("Frame structure, REAL playback: %zu calls in %zu display frames; steps per frame: %s; half-tick calls %zu, last-tick calls %zu; calls on a fractional clock %zu; "
                           "skipped ticks %d; calls on an already seen tick %d",
                           st.calls, st.frames, stepsPerFrameText(st.stepsPerFrame).c_str(), st.halfCalls, st.lastCalls, st.fractional, st.skippedTicks, st.repeatedCalls));
    structure.push_back(sf("  the game's own processCommands moved the level clock by 1 tick in %zu of %zu calls (0.5 tick: %zu, not at all: %zu, other: %zu); dt of the calls %.8f..%.8f; time warp %.3f..%.3f",
                           st.clockInside1, st.calls, st.clockInsideHalf, st.clockInsideNone, st.clockInsideOther, double(st.dtMin), double(st.dtMax), double(st.twMin), double(st.twMax)));
    std::string simFs = S.stepMode == "Update" ? "Frame structure, SILENT replay: GJBaseGameLayer::update(1/240) once per tick" : "Frame structure, SILENT replay: processCommands(1/240, half-tick false, last-tick true) once per tick";
    structure.push_back(sf("%s: %llu steps; the hook saw %llu processCommands calls (half-tick %llu, last-tick %llu; dt %.8f..%.8f); the level clock was moved by the game itself in %llu of %llu "
                           "steps and by the mod in %llu (the game's own move: %.3f..%.3f ticks)",
                           simFs.c_str(), static_cast<unsigned long long>(S.steps), static_cast<unsigned long long>(S.calls), static_cast<unsigned long long>(S.halfCalls),
                           static_cast<unsigned long long>(S.lastCalls), double(S.dtMin), double(S.dtMax), static_cast<unsigned long long>(S.steps - std::min(S.steps, S.clockForced)),
                           static_cast<unsigned long long>(S.steps), static_cast<unsigned long long>(S.clockForced), S.clockAdvMin, S.clockAdvMax));
    for (auto const& l : structure) for (auto const& w : wrapLines(l, 120)) { R.result.push_back(w); R.log.push_back(w); }
    for (auto const* s : {&settings1, &settings2, &settings3, &settings4}) for (auto const& w : wrapLines(*s, 120)) { R.result.push_back(w); R.log.push_back(w); }

    // ---- the clicks nearest the death (log only) --------------------------------------------------------------------------------------------
    int ref = S.died ? S.deathTick : C.firstOver;
    if (ref >= 0 && in.macro) {
        auto clicks = nearestClickFacts(*in.macro, S.samples, series, *in.real, ref, kTraceNearClicks, in.eps);
        R.log.push_back(sf("clicks nearest tick %d (%s): the same facts around each edge (window edge-%d .. edge+%d, k = 0 unless said)", ref, S.died ? "the death of the silent replay" : "the first difference",
                           kTraceEdgeBefore, kTraceEdgeAfter));
        for (auto const* s : {&settings1, &settings2, &settings3, &settings4}) R.log.push_back("  " + *s);
        for (auto const& c : clicks) {
            std::string where = c.distance == 0 ? "the player was holding it at that tick" : (ref < c.pressTick ? sf("%d tick(s) before the press", c.distance) : sf("%d tick(s) after the release", c.distance));
            R.log.push_back(sf("  click #%d (P%d): press %d, %s - %s", c.number, c.player + 1, c.pressTick, c.hasRelease ? sf("release %d", c.releaseTick).c_str() : "no release", where.c_str()));
            for (EdgeFacts const* e : {&c.press, c.hasRelease ? &c.release : nullptr}) {
                if (!e) continue;
                std::string applied = e->realApplied ? sf("real playback applied it in the call at tick %d (%s call)%s", e->realTick, flagLetters(e->realFlags).c_str(),
                                                          e->realTick != e->tick ? sf("  <-- NOT the macro tick (%+d)", e->realTick - e->tick).c_str() : "")
                                                     : std::string("real playback: not applied in the trace");
                R.log.push_back(sf("    %s %d: %s; silent replay applies it at tick %d", e->press ? "press" : "release", e->tick, applied.c_str(), e->tick));
                R.log.push_back("      real calls around it (tick(flags frame +inputs)): " + e->callsAround);
                auto const& lc = e->local;
                if (lc.hi < lc.lo) {   // the silent replay ended (it died) before this edge: there is no silent state to compare
                    R.log.push_back(sf("      the silent replay ended at tick %d, before this tick: nothing to compare here", lc.hi));
                    continue;
                }
                std::string diff;
                if (lc.compared == 0) diff = "no real data in the window";
                else if (lc.firstOver < 0) diff = sf("equal at all %d compared ticks", lc.compared);
                else diff = sf("first difference at tick %d: silent (%s) real (%s)", lc.firstOver, sampleText(lc.simAt, 0, false).c_str(), sampleText(lc.realAt, 0, false).c_str());
                R.log.push_back(sf("      ticks %d..%d: %s; best shift in this window k=%+d (%d of %d ticks equal; k=0: %d)", lc.lo, lc.hi, diff.c_str(), lc.bestK, lc.bestMatched,
                                   lc.hi - lc.lo + 1, lc.matchedPerShift.empty() ? 0 : lc.matchedPerShift[size_t(kTraceMaxShift)]));
                R.log.push_back(sf("      vertical only (y, yVel; x ignored): best shift in this window k=%+d (%d of %d ticks equal; k=0: %d)", lc.vBestK, lc.vBestMatched,
                                   lc.hi - lc.lo + 1, lc.vMatchedPerShift.empty() ? 0 : lc.vMatchedPerShift[size_t(kTraceMaxShift)]));
            }
        }
    }
    R.callsCsv = callsCsvOf(*in.real);
    R.compareCsv = compareCsvOf(S.samples, series, *in.real, in.eps);
    return R;
}

}  // namespace gdfwr
