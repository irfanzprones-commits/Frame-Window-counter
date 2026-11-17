#include "Analysis.hpp"

#include <algorithm>
#include <cmath>

namespace gdfwr {

std::vector<Action> makeActions(const std::vector<Event>& ev) {
    std::vector<Action> a;
    a.reserve(ev.size());
    for (const auto& e : ev) a.push_back({e.editedTick, e.player, e.type == EventType::Press, e.seq, bool(e.flags & EventFlag::Synthetic) && !(e.flags & EventFlag::Junction)});
    return a;
}

std::vector<int> requiredCheckpointTicks(const std::vector<Action>& a, int cap, int reach) {
    std::vector<int> t;
    for (const auto& x : a) {
        t.push_back(std::max(0, x.tick - cap));
        if (reach > cap) t.push_back(std::max(0, x.tick - reach));  // far start for the long search
    }
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    return t;
}

static std::string reversed(std::string s) { std::reverse(s.begin(), s.end()); return s; }

// Holes and trap ticks (4.9). cells[d-1] is the result at offset d on one side.
static void holesAndTraps(const std::string& cells, int& holes, int& traps) {
    bool failed = false;
    for (size_t i = 0; i < cells.size(); i++) {
        if (cells[i] == 'X') failed = true;
        else if (cells[i] == 'P' && failed) holes++;
    }
    for (size_t i = 0; i + 1 < cells.size(); i++) {
        bool prevPass = (i == 0) || cells[i - 1] == 'P';  // offset 0 counts as a pass once verified (4.10)
        if (cells[i] == 'X' && prevPass && cells[i + 1] == 'P') traps++;
    }
}

Counters countWindows(const AnalysisData& a) {
    Counters k;
    auto add = [&](const ClickResult& c, const WindowResult& w) {
        if (c.flags & ClickFlag::NotAnalysed) { k.notAnalysed++; return; }
        if (c.flags & ClickFlag::Unreliable) { k.unreliable++; return; }
        if (!w.analysed || w.na) return;
        if (c.flags & ClickFlag::Random) { k.random++; return; }
        if (w.releaseInsensitive) return;
        if (w.clamped()) { k.clamp++; return; }
        if (w.lowerBound()) { k.nPlus++; return; }
        int v = w.window();
        if (v <= 1) k.b1++; else if (v == 2) k.b2++; else if (v == 3) k.b3++; else if (v == 4) k.b4++;
        else if (v <= 6) k.b5_6++; else if (v <= 10) k.b7_10++; else if (v <= 24) k.b11_24++; else k.b25p++;
    };
    for (const auto& c : a.clicks) { add(c, c.in); add(c, c.out); }
    return k;
}

std::vector<ClickEdges> listClicks(const std::vector<Action>& a) {
    std::vector<std::vector<int>> stream(2);
    for (size_t i = 0; i < a.size(); i++) stream[a[i].player & 1].push_back(int(i));
    std::vector<ClickEdges> out;
    for (int p = 0; p < 2; p++) {
        const auto& s = stream[size_t(p)];
        if (s.empty() || !a[size_t(s[0])].press) continue;   // invalid streams are refused by the gate (same rule as the engine)
        for (size_t k = 0; k < s.size(); k += 2) {
            int rel = k + 1 < s.size() ? s[k + 1] : -1;
            if (rel >= 0 && k + 2 == s.size() && a[size_t(rel)].synthetic) rel = -1;   // a synthetic last release = "ends while holding"
            out.push_back({p, int(k / 2), a[size_t(s[k])].tick, rel >= 0 ? a[size_t(rel)].tick : -1});
        }
    }
    return out;
}

std::vector<Chunk> planChunks(const std::vector<Action>& actions, int cap, int reach, size_t maxStates, int fullScanTop) {
    std::vector<Chunk> chunks;
    const auto clicks = listClicks(actions);
    const size_t n = clicks.size();
    if (n == 0) return chunks;
    maxStates = std::max<size_t>(maxStates, 8);
    reach = std::max(reach, cap);
    auto addTicks = [&](std::vector<int>& v, const ClickEdges& c) {
        for (int edge : {c.press, c.release}) {
            if (edge < 0) continue;
            v.push_back(std::max(0, edge - cap));
            if (reach > cap) v.push_back(std::max(0, edge - reach));
        }
    };
    auto validBoundary = [&](size_t i) {   // can a chunk end after click i?
        if (i + 1 >= n) return true;
        const auto &a = clicks[i], &b = clicks[i + 1];
        if (a.player != b.player) return true;
        return a.release < 0 || b.press - a.release > cap;   // actions <= cap apart form one cluster
    };
    size_t s = 0;
    while (s < n) {
        Chunk ch;
        ch.clickFrom = int(s);
        std::vector<int> ticks;
        size_t e = s;
        for (; e < n; e++) {
            addTicks(ticks, clicks[e]);
            std::sort(ticks.begin(), ticks.end());
            ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
            if (validBoundary(e) && ticks.size() >= maxStates) { e++; break; }
        }
        ch.clickTo = int(std::min(e, n));
        ch.ticks = std::move(ticks);
        ch.lastTick = ch.ticks.empty() ? 0 : ch.ticks.back();
        chunks.push_back(std::move(ch));
        s = size_t(chunks.back().clickTo);
    }
    for (auto& c : chunks) {
        c.fullScanTop = fullScanTop <= 0 ? 0 : std::max(1, int((int64_t(fullScanTop) * (c.clickTo - c.clickFrom) + int64_t(n) - 1) / int64_t(n)));
    }
    return chunks;
}

void computeSummary(AnalysisData& d) {
    Summary s;
    auto reliable = [](const ClickResult& c) { return !(c.flags & (ClickFlag::Unreliable | ClickFlag::Random | ClickFlag::NotAnalysed)); };
    auto tightest = [&](auto eligible, auto windowOf, std::vector<uint32_t>& out) {
        int best = INT_MAX;
        for (size_t i = 0; i < d.clicks.size(); i++) if (eligible(d.clicks[i])) best = std::min(best, windowOf(d.clicks[i]));
        for (size_t i = 0; i < d.clicks.size(); i++) if (eligible(d.clicks[i]) && windowOf(d.clicks[i]) == best) out.push_back(uint32_t(i));
    };
    tightest([&](const ClickResult& c) { return reliable(c) && c.in.analysed && !c.in.clamped(); },
             [](const ClickResult& c) { return c.in.window(); }, s.tightestInput);
    tightest([&](const ClickResult& c) { return reliable(c) && c.out.analysed && !c.out.na && !c.out.releaseInsensitive && !c.out.clamped(); },
             [](const ClickResult& c) { return c.out.window(); }, s.tightestOutput);
    {   // cluster ranking is separate and never replaces a clamped click's window (section 7 / 9)
        int best = INT_MAX;
        for (const auto& c : d.clusters) if (c.win.analysed && !c.win.clamped()) best = std::min(best, c.win.window());
        for (size_t i = 0; i < d.clusters.size(); i++)
            if (d.clusters[i].win.analysed && !d.clusters[i].win.clamped() && d.clusters[i].win.window() == best) s.tightestCluster.push_back(uint32_t(i));
    }
    double sumIn = 0, sumOut = 0;
    int nIn = 0, nOut = 0;
    for (const auto& c : d.clicks) {
        if (!reliable(c)) continue;
        if (c.in.analysed) { sumIn += c.in.window(); nIn++; s.avgInputLower |= c.in.lowerBound(); }
        if (c.out.analysed && !c.out.na && !c.out.releaseInsensitive) { sumOut += c.out.window(); nOut++; s.avgOutputLower |= c.out.lowerBound(); }
        s.reliableClicks++;
    }
    s.avgInput = nIn ? sumIn / nIn : 0;
    s.avgOutput = nOut ? sumOut / nOut : 0;
    d.summary = s;
}

void mergeAnalysis(AnalysisData& t, const AnalysisData& p, int from, int to) {
    if (t.clicks.size() != p.clicks.size()) {   // first part: keep the identity of every click, everything else is "not analysed yet"
        AnalysisData n = p;
        for (auto& c : n.clicks) { c.in = WindowResult{}; c.out = WindowResult{}; c.flags = ClickFlag::NotAnalysed; }
        n.clusters.clear();
        n.incomplete = false;
        t = std::move(n);
    }
    from = std::max(from, 0);
    to = std::min<int>(to, int(p.clicks.size()));
    for (int c = from; c < to; c++) t.clicks[size_t(c)] = p.clicks[size_t(c)];
    for (const auto& cl : p.clusters)
        if (!cl.clicks.empty() && int(cl.clicks.front()) >= from && int(cl.clicks.front()) < to) t.clusters.push_back(cl);
    t.incomplete = t.incomplete || p.incomplete;
    t.counters = countWindows(t);
    computeSummary(t);
}

// ---------------------------------------------------------------------------------------
AnalysisEngine::AnalysisEngine(AnalysisInput in, AnalysisParams par, ISimulator& sim)
    : in_(std::move(in)), par_(std::move(par)), sim_(sim) {
    par_.cap = std::clamp(par_.cap, 1, kOffsetCap);  // the pass map and the overlay have 12 cells per side
    par_.reach = std::max(par_.reach, par_.cap);
    par_.subTickSteps = std::clamp(par_.subTickSteps, 1, 8);
    data_.cap = par_.cap;
    data_.reach = par_.reach;
    data_.wholeClick = par_.wholeClick;
    data_.subTickMode = par_.subTick && sim.supportsSubTick();
    data_.fullScanTop = par_.fullScanTop;
    data_.epsilon = par_.epsilon;
    for (const auto& a : in_.actions) base_.push_back({a.tick, a.player, a.press, a.seq});
    buildStreams();
    nextStage();
}

void AnalysisEngine::buildStreams() {
    stream_.assign(2, {});
    for (size_t i = 0; i < in_.actions.size(); i++) stream_[in_.actions[i].player & 1].push_back(int(i));
    for (int p = 0; p < 2; p++) {
        auto& s = stream_[size_t(p)];
        if (s.empty() || !in_.actions[size_t(s[0])].press) continue;  // invalid streams are refused by the gate
        for (size_t k = 0; k < s.size(); k += 2) {
            int rel = k + 1 < s.size() ? s[k + 1] : -1;
            // a synthetic LAST release (import or pause fix) means "the run ends while holding" (Appendix A, item 9)
            if (rel >= 0 && k + 2 == s.size() && in_.actions[size_t(rel)].synthetic) rel = -1;
            ClickRef r{p, int(k / 2), s[k], rel};
            refs_.push_back(r);
            ClickResult c;
            c.index = uint32_t(r.idx);
            c.player = uint8_t(p);
            c.press = in_.actions[size_t(r.press)].tick;
            c.release = r.release >= 0 ? in_.actions[size_t(r.release)].tick : -1;
            data_.clicks.push_back(c);
        }
    }
    unreliable_.assign(refs_.size(), false);
    convT_.assign(refs_.size(), 0);
    convH_.assign(refs_.size(), 0);
}

int AnalysisEngine::prevRelease(int c) const {
    const auto& r = refs_[size_t(c)];
    if (r.idx == 0) return -1;
    return in_.actions[size_t(stream_[size_t(r.player)][size_t(2 * r.idx - 1)])].tick;
}
int AnalysisEngine::nextPress(int c) const {
    const auto& r = refs_[size_t(c)];
    const auto& s = stream_[size_t(r.player)];
    size_t k = size_t(2 * (r.idx + 1));
    return k < s.size() ? in_.actions[size_t(s[k])].tick : -1;
}
int AnalysisEngine::horizonFor(int c) const {
    const auto& r = refs_[size_t(c)];
    const auto& s = stream_[size_t(r.player)];
    size_t k = size_t(2 * (r.idx + 1) + 1);  // release of the next real click
    return k < s.size() ? in_.actions[size_t(s[k])].tick : in_.levelEndTick;
}
std::array<uint8_t, 2> AnalysisEngine::holdAt(int tick) const {
    std::array<uint8_t, 2> h{0, 0};
    for (const auto& a : in_.actions) if (a.tick < tick) h[a.player & 1] = a.press;
    return h;
}

std::vector<SimInput> AnalysisEngine::plan(int startTick, const std::vector<int>& remove,
                                           const std::vector<SimInput>& add) const {
    std::vector<SimInput> v;
    for (size_t a = 0; a < base_.size(); a++) {
        if (base_[a].tick < startTick) continue;
        if (std::find(remove.begin(), remove.end(), int(a)) != remove.end()) continue;
        v.push_back(base_[a]);
    }
    for (const auto& x : add) v.push_back(x);
    std::stable_sort(v.begin(), v.end(), [](const SimInput& a, const SimInput& b) {  // real-time order: tick, then fraction, then sequence
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.sub != b.sub) return a.sub < b.sub;
        return a.order < b.order;
    });
    return v;
}

// ---- staging -------------------------------------------------------------------------------
void AnalysisEngine::enqueueWalks(int c, bool full, bool verify) {
    bool hasRelease = refs_[size_t(c)].release >= 0;
    if (verify) queue_.push_back({TaskKind::Verify, Win::Input, c, 0, false});
    queue_.push_back({TaskKind::Walk, Win::Input, c, -1, full});
    queue_.push_back({TaskKind::Walk, Win::Input, c, +1, full});
    if (hasRelease) {
        if (verify) queue_.push_back({TaskKind::Verify, Win::Output, c, 0, false});
        queue_.push_back({TaskKind::Walk, Win::Output, c, -1, full});
        queue_.push_back({TaskKind::Walk, Win::Output, c, +1, full});
    } else {
        data_.clicks[size_t(c)].out.na = true;  // run ends while holding: output window undefined (Analysis 8)
    }
}

void AnalysisEngine::enqueueClusters() {
    std::vector<int> base(2, 0);
    base[1] = 0;
    for (const auto& r : refs_) if (r.player == 0) base[1]++;
    for (int p = 0; p < 2; p++) {
        const auto& s = stream_[size_t(p)];
        size_t i = 0;
        while (i < s.size()) {
            size_t j = i;
            while (j + 1 < s.size() &&
                   in_.actions[size_t(s[j + 1])].tick - in_.actions[size_t(s[j])].tick <= par_.cap) j++;
            bool inRange = true;
            for (size_t k = i / 2; k <= j / 2; k++) {
                int id = base[size_t(p)] + int(k);
                if (id < par_.clickFrom || id >= par_.clickTo) inRange = false;
            }
            if (inRange && j / 2 > i / 2) {  // covers two or more clicks; a single click is its own input window (4.11)
                Cluster cl{p, int(i), int(j), {}};
                ClusterResult cr;
                cr.player = uint8_t(p);
                cr.firstAction = int(i);
                cr.lastAction = int(j);
                for (size_t k = i; k <= j; k++) cl.acts.push_back(s[k]);
                for (size_t k = i / 2; k <= j / 2; k++) {
                    uint32_t id = uint32_t(base[size_t(p)] + int(k));
                    cr.clicks.push_back(id);
                    data_.clicks[id].flags |= ClickFlag::Cluster;
                }
                clusters_.push_back(std::move(cl));
                data_.clusters.push_back(std::move(cr));
            }
            i = j + 1;
        }
    }
    for (size_t k = 0; k < clusters_.size(); k++) {
        queue_.push_back({TaskKind::Walk, Win::Cluster, int(k), -1, false});
        queue_.push_back({TaskKind::Walk, Win::Cluster, int(k), +1, false});
    }
}

bool AnalysisEngine::nextStage() {
    while (stage_ < 3) {
        stage_++;
        if (stage_ == 1) {  // pass 1: fast mode on every click
            for (int c = std::max(0, par_.clickFrom); c < int(refs_.size()) && c < par_.clickTo; c++) {
                if (horizonFor(c) >= in_.analyseUntilTick) {
                    data_.clicks[size_t(c)].flags |= ClickFlag::NotAnalysed;
                    continue;
                }
                enqueueWalks(c, false, true);
            }
        } else if (stage_ == 2) {  // pass 2: full scan on the tightest clicks and the selected ones
            std::vector<std::pair<int, int>> rank;
            for (int c = 0; c < int(refs_.size()); c++) {
                const auto& k = data_.clicks[size_t(c)];
                if (unreliable_[size_t(c)] || !k.in.analysed || (k.flags & ClickFlag::NotAnalysed)) continue;
                int key = k.in.window();
                if (k.out.analysed && !k.out.na) key = std::min(key, k.out.window());
                rank.push_back({key, c});
            }
            std::sort(rank.begin(), rank.end());
            std::vector<int> pick;
            for (size_t i = 0; i < rank.size() && int(i) < par_.fullScanTop; i++) pick.push_back(rank[i].second);
            for (int s : par_.selectedClicks)
                if (s >= 0 && s < int(refs_.size()) && !unreliable_[size_t(s)] && data_.clicks[size_t(s)].in.analysed) pick.push_back(s);
            std::sort(pick.begin(), pick.end());
            pick.erase(std::unique(pick.begin(), pick.end()), pick.end());
            for (int c : pick) enqueueWalks(c, true, false);
        } else if (stage_ == 3 && par_.clusters) {
            enqueueClusters();
        }
        if (!queue_.empty()) {
            prog_.stage = stage_;
            prog_.total += queue_.size();
            return true;
        }
    }
    return false;
}

bool AnalysisEngine::popTask() {
    if (queue_.empty()) return false;
    cur_ = Walk{};
    cur_.task = queue_.front();
    queue_.pop_front();
    haveCur_ = true;
    return true;
}

// ---- one shifted run -------------------------------------------------------------------------
bool AnalysisEngine::clampAt(const Walk& w, int d) const {
    const Task& t = w.task;
    if (t.win == Win::Cluster) {
        if (t.side < 0) return in_.actions[size_t(clusters_[size_t(t.target)].acts.front())].tick - d < 0;
        return false;  // outside actions are more than `cap` ticks away: they cannot be reached (4.11)
    }
    const ClickRef& r = refs_[size_t(t.target)];
    int P = in_.actions[size_t(r.press)].tick;
    int R = r.release >= 0 ? in_.actions[size_t(r.release)].tick : -1;
    int pr = prevRelease(t.target), np = nextPress(t.target);
    if (t.win == Win::Input) {
        if (t.side < 0) return P - d < 0 || (pr >= 0 && P - d <= pr);          // table 4.8: P_i - d <= R_(i-1)
        if (par_.wholeClick) return R >= 0 && np >= 0 && R + d >= np;          // v1: the release moves too: R_i + d >= P_(i+1)
        if (R >= 0) return P + d >= R;                                         // v2: only the press moves; the hold must stay >= 1 tick
        return in_.levelEndTick > 0 && P + d >= in_.levelEndTick;              // run ends while holding: the press cannot pass the end
    }
    if (t.side < 0) return R - d <= P;                                          // R_i - d <= P_i
    return np >= 0 && R + d >= np;                                              // R_i + d >= P_(i+1)
}

SimRequest AnalysisEngine::buildRequest(const Walk& w, double off, int* startOut) const {
    const Task& t = w.task;
    std::vector<int> remove;
    std::vector<SimInput> add;
    int minTick = 0, horizon = in_.levelEndTick, driftTick = -1, driftPlayer = 0, lastModified = 0;
    const bool whole = off == std::floor(off);
    auto shifted = [&](int actionIdx) {
        remove.push_back(actionIdx);
        SimInput s = base_[size_t(actionIdx)];
        if (whole) {
            s.tick += t.side * int(off);
        } else {  // between two physics steps (sub-tick precision mode)
            double when = double(s.tick) + double(t.side) * off;
            double fl = std::floor(when);
            s.tick = int(fl);
            s.sub = std::clamp(when - fl, 0.0, 0.999999);
        }
        lastModified = std::max(lastModified, s.tick);
        add.push_back(s);
    };
    if (t.win == Win::Cluster) {
        const Cluster& cl = clusters_[size_t(t.target)];
        for (int a : cl.acts) shifted(a);
        minTick = base_[size_t(cl.acts.front())].tick;
        // horizon: release of the first real click after the cluster (4.11)
        const auto& s = stream_[size_t(cl.player)];
        size_t j = size_t(cl.last) + 1;
        if (j < s.size()) {
            size_t rel = in_.actions[size_t(s[j])].press ? j + 1 : j + 2;
            horizon = rel < s.size() ? in_.actions[size_t(s[rel])].tick : in_.levelEndTick;
        }
    } else {
        const ClickRef& r = refs_[size_t(t.target)];
        if (t.win == Win::Input) {
            shifted(r.press);
            if (par_.wholeClick && r.release >= 0) shifted(r.release);   // v1: fixed Hold. v2: the release stays where it was
            minTick = base_[size_t(r.press)].tick;
        } else {
            shifted(r.release);
            minTick = base_[size_t(r.release)].tick;
        }
        horizon = horizonFor(t.target);
        int np = nextPress(t.target);
        if (np >= 0) { driftTick = np; driftPlayer = r.player; }
    }
    // Offsets up to cap start from the checkpoint at edge - cap; larger offsets (long search) from edge - reach.
    const int back = off <= double(par_.cap) ? par_.cap : par_.reach;
    int start = std::max(0, minTick - back);
    if (startOut) *startOut = start;
    SimRequest rq;
    rq.startTick = start;
    rq.holdAtStart = holdAt(start);
    rq.inputs = plan(start, remove, add);
    rq.horizonTick = std::max(horizon, lastModified);
    rq.driftTick = driftTick;
    rq.driftPlayer = driftPlayer;
    rq.convergeAfter = par_.detectConvergence ? lastModified + 1 : -1;
    return rq;
}

SimRequest AnalysisEngine::verifyRequest(int c, Win w, int* startOut) const {
    const ClickRef& r = refs_[size_t(c)];
    int edge = in_.actions[size_t(w == Win::Input ? r.press : r.release)].tick;
    SimRequest rq;
    rq.startTick = std::max(0, edge - par_.cap);
    if (startOut) *startOut = rq.startTick;
    rq.holdAtStart = holdAt(rq.startTick);
    rq.inputs = plan(rq.startTick, {}, {});
    rq.horizonTick = horizonFor(c);
    rq.compareBaseline = true;  // 4.10: must stay alive and match the baseline
    return rq;
}

// One shifted run at offset `off` (ticks, may be fractional in sub-tick mode). true = the player survives to the horizon.
bool AnalysisEngine::runOffset(Walk& w, double off, SimResult& r) {
    r = sim_.run(buildRequest(w, off, nullptr));
    if (!r.loaded && w.task.win != Win::Cluster) {  // the saved state is missing: this click cannot be measured
        unreliable_[size_t(w.task.target)] = true;
        data_.clicks[size_t(w.task.target)].flags |= ClickFlag::Unreliable;
    }
    return r.loaded && !r.died;
}

// All of 1..cap passed (or the scan went on to cap in full-scan mode).
void AnalysisEngine::endScan() {
    Walk& w = cur_;
    if (w.failed) { afterEdge(); return; }
    if (par_.reach > par_.cap && w.task.win != Win::Cluster) {  // long search: coarse steps, then bisection (monotone assumption)
        w.phase = Phase::Coarse;
        w.lo = par_.cap;
        w.coarseX = par_.cap + 4;
        w.extended = true;
        return;
    }
    finishWalk();
}

// The whole-tick edge is known: lies in [passes, passes+1). Narrow it below one tick when the simulator can.
void AnalysisEngine::afterEdge() {
    Walk& w = cur_;
    if (par_.subTick && sim_.supportsSubTick() && w.stop == StopReason::Death && w.task.win != Win::Cluster) {
        w.phase = Phase::Sub;
        w.slo = double(w.passes);
        w.shi = double(w.passes + 1);
        w.subIter = 0;
        return;
    }
    finishWalk();
}

bool AnalysisEngine::advance() {
    Walk& w = cur_;
    const Task& t = w.task;
    if (t.kind == TaskKind::Verify) {
        SimResult r = sim_.run(verifyRequest(t.target, t.win, nullptr));
        prog_.done++;
        haveCur_ = false;
        if (!r.loaded || r.died || r.desync) {  // desync: mark the click UNRELIABLE and skip it (4.10)
            unreliable_[size_t(t.target)] = true;
            data_.clicks[size_t(t.target)].flags |= ClickFlag::Unreliable;
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Task& q) { return q.win != Win::Cluster && q.target == t.target; }), queue_.end());
        }
        return true;
    }
    SimResult r;
    switch (w.phase) {
    case Phase::Scan: {
        if (w.d > par_.cap) { endScan(); return false; }
        if (clampAt(w, w.d)) {
            for (int k = w.d; k <= par_.cap; k++) w.cells[size_t(k - 1)] = '-';  // not tested, never a failure
            if (!w.failed) { w.stop = StopReason::Clamp; finishWalk(); }
            else afterEdge();
            return false;
        }
        bool pass = runOffset(w, w.d, r);
        w.convTests++;
        if (pass && r.converged) w.convHits++;
        size_t idx = size_t(w.d - 1);
        if (pass) {
            w.cells[idx] = 'P';
            if (!w.failed) {  // only the contiguous run counts
                w.passes++;
                if (r.driftValid && t.win != Win::Cluster) {
                    const auto& smp = in_.baselineSamples;
                    const auto& rq = refs_[size_t(t.target)];
                    int np = nextPress(t.target);
                    if (np >= 0 && size_t(np) < smp.size()) {
                        double by = rq.player == 0 ? smp[size_t(np)].y1 : smp[size_t(np)].y2;
                        double bv = rq.player == 0 ? smp[size_t(np)].vy1 : smp[size_t(np)].vy2;
                        w.maxDy = std::max(w.maxDy, std::fabs(r.y - by));
                        w.maxDvy = std::max(w.maxDvy, std::fabs(r.vy - bv));
                        w.drift = true;
                    }
                }
            }
        } else {
            w.cells[idx] = 'X';
            if (!w.failed) {
                w.failed = true;
                w.stop = StopReason::Death;
                w.limiting = r.diedMask;
            }
            if (!t.full) {  // fast mode: later cells were not tested
                for (int k = w.d + 1; k <= par_.cap; k++) w.cells[size_t(k - 1)] = '.';
                afterEdge();
                return true;
            }
        }
        w.d++;
        if (w.d > par_.cap) endScan();
        return true;
    }
    case Phase::Coarse: {
        const int x = std::min(w.coarseX, par_.reach);
        if (clampAt(w, x)) {  // a neighbouring action is in the way: find the first offset that cannot be tested
            int c0 = x;
            for (int k = w.lo + 1; k <= x; k++) if (clampAt(w, k)) { c0 = k; break; }
            w.hi = c0;
            w.hiClamp = true;
            w.phase = Phase::Bisect;
            return false;
        }
        if (runOffset(w, x, r)) {
            w.lo = x;
            if (x >= par_.reach) { w.passes = x; w.stop = StopReason::Cap; finishWalk(); return true; }
            w.coarseX = x + 4;
        } else {
            w.hi = x;
            w.hiClamp = false;
            w.limiting = r.diedMask;
            w.phase = Phase::Bisect;
        }
        return true;
    }
    case Phase::Bisect: {
        if (w.hi - w.lo <= 1) {
            w.passes = w.lo;
            if (w.hiClamp) { w.stop = StopReason::Clamp; finishWalk(); }
            else { w.failed = true; w.stop = StopReason::Death; afterEdge(); }
            return false;
        }
        const int mid = (w.lo + w.hi) / 2;
        if (runOffset(w, mid, r)) w.lo = mid;
        else { w.hi = mid; w.hiClamp = false; w.limiting = r.diedMask; }
        return true;
    }
    case Phase::Sub: {
        if (w.subIter >= par_.subTickSteps) { w.sub = true; finishWalk(); return false; }
        const double mid = 0.5 * (w.slo + w.shi);
        if (runOffset(w, mid, r)) w.slo = mid; else w.shi = mid;
        w.subIter++;
        return true;
    }
    }
    return false;
}

void AnalysisEngine::finishWalk() {
    Walk& w = cur_;
    const Task& t = w.task;
    char fill = (w.stop == StopReason::Clamp) ? '-' : '.';
    for (auto& ch : w.cells) if (ch == '?') ch = fill;
    if (w.stop == StopReason::None) w.stop = StopReason::Cap;  // all offsets passed: true value is cap or more
    WindowResult& wr = (t.win == Win::Cluster) ? data_.clusters[size_t(t.target)].win : win(t.target, t.win);
    const double eLo = w.sub ? w.slo : double(w.passes);
    const double eHi = w.sub ? w.shi : double(w.passes + 1);
    if (t.side < 0) {
        wr.early = w.passes;
        wr.stopEarly = w.stop;
        wr.mapEarly = reversed(w.cells);
        wr.edgeEarlyLo = eLo; wr.edgeEarlyHi = eHi;
    } else {
        wr.late = w.passes;
        wr.stopLate = w.stop;
        wr.mapLate = w.cells;
        wr.edgeLateLo = eLo; wr.edgeLateHi = eHi;
    }
    wr.subTick |= w.sub;
    wr.extended |= w.extended;
    wr.limitingPlayer |= w.limiting;
    if (w.drift) {
        wr.driftValid = true;
        wr.driftDy = std::max(wr.driftDy, w.maxDy);
        wr.driftDvy = std::max(wr.driftDvy, w.maxDvy);
    }
    if (t.win == Win::Output) {
        convT_[size_t(t.target)] += w.convTests;
        convH_[size_t(t.target)] += w.convHits;
    }
    if (t.side > 0) {  // both sides done
        wr.analysed = true;
        wr.fullScan = t.full;
        if (t.full) {
            wr.holes = wr.traps = 0;
            holesAndTraps(reversed(wr.mapEarly), wr.holes, wr.traps);
            holesAndTraps(wr.mapLate, wr.holes, wr.traps);
        }
        if (t.win == Win::Output) {
            int c = t.target;
            wr.releaseInsensitive = convT_[size_t(c)] > 0 && convH_[size_t(c)] == convT_[size_t(c)];
            convT_[size_t(c)] = convH_[size_t(c)] = 0;
        }
        if (t.win != Win::Cluster) data_.counters = countWindows(data_);
    }
    prog_.done++;
    haveCur_ = false;
}

void AnalysisEngine::finalize() {
    auto fp = [](const WindowResult& w) {
        return w.analysed && !w.na && w.early == 0 && w.late == 0 && w.stopEarly == StopReason::Death && w.stopLate == StopReason::Death;
    };
    for (size_t c = 0; c < data_.clicks.size(); c++) {
        auto& k = data_.clicks[c];
        if (k.in.clamped() || k.out.clamped()) k.flags |= ClickFlag::Clamp;
        if (k.in.driftValid || k.out.driftValid) k.flags |= ClickFlag::Drift;
        if (fp(k.in) || fp(k.out)) k.flags |= ClickFlag::FramePerfect;
        if (k.out.releaseInsensitive) k.flags |= ClickFlag::ReleaseInsensitive;
        k.flags |= in_.globalFlags;
        if (in_.regionFlags)
            k.flags |= in_.regionFlags(std::max(0, k.press - par_.cap), horizonFor(int(c)), k.player);
    }
    for (auto& cl : data_.clusters) {
        if (cl.win.clamped()) cl.flags |= ClickFlag::Clamp;
        cl.flags |= in_.globalFlags;
    }
    data_.counters = countWindows(data_);

    computeSummary(data_);   // run summary (Analysis 9)
    data_.incomplete = cancelled_ || data_.incomplete;
    finished_ = true;
}

void AnalysisEngine::cancel() {
    if (finished_) return;
    cancelled_ = true;
    queue_.clear();
    haveCur_ = false;
    finalize();
}

bool AnalysisEngine::step() {
    while (!finished_) {
        if (!haveCur_) {
            if (!popTask()) {
                if (!nextStage()) { finalize(); return false; }
                continue;
            }
        }
        if (advance()) return true;
    }
    return false;
}

LiveView AnalysisEngine::live() const {
    LiveView v;
    v.counters = data_.counters;
    if (haveCur_ && cur_.task.win != Win::Cluster) {
        int c = cur_.task.target;
        v.action = 2 * c + (cur_.task.win == Win::Input ? 1 : 2);
        const WindowResult& wr = cur_.task.win == Win::Input ? data_.clicks[size_t(c)].in : data_.clicks[size_t(c)].out;
        v.mapEarly = wr.mapEarly;
        v.mapLate = wr.mapLate;
        if (cur_.task.kind == TaskKind::Walk) {
            if (cur_.task.side < 0) v.mapEarly = reversed(cur_.cells);
            else v.mapLate = cur_.cells;
        }
    }
    return v;
}

}  // namespace gdfwr
