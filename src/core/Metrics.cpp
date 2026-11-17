#include "Metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "Session.hpp"

namespace gdfwr {

ReplayBucket replayBucket(int w) {
    if (w <= 1) return ReplayBucket::W1;
    if (w == 2) return ReplayBucket::W2;
    if (w == 3) return ReplayBucket::W3;
    if (w == 4) return ReplayBucket::W4;
    if (w <= 6) return ReplayBucket::W5_6;
    if (w <= 8) return ReplayBucket::W7_8;
    if (w <= 12) return ReplayBucket::W9_12;
    if (w <= 24) return ReplayBucket::W13_24;
    return ReplayBucket::W25P;
}
const char* replayBucketLabel(ReplayBucket b) {
    static const char* n[] = {"25+", "13-24", "9-12", "7-8", "5-6", "4", "3", "2", "1"};
    return n[int(b)];
}
uint32_t replayBucketColor(ReplayBucket b) {
    // 13-24 and 25+ are new in v2 [DECISION: colours]; the others are the ones of 9.6.
    static const uint32_t c[] = {0xE3B5FF, 0xB48CFF, 0x7B7BFF, 0x7DB8FF, 0x7CF08A, 0xFFFFFF, 0xFFF27A, 0xFFA85C, 0xFF5C5C};
    return c[int(b)];
}

ReplayPlan buildReplayPlan(const GdfwrFile& f) {
    ReplayPlan p;
    if (!f.macro.finalRun.exists || finalRunEvents(f.macro).empty()) { p.reason = "There is no macro to replay."; return p; }
    if (!f.analysis) p.note = "No analysis yet: the replay shows the macro only. Run Analyze for the window numbers.";
    else if (f.analysis->stale) p.note = "The analysis is STALE: the replay shows the macro only. Run Analyze again for the window numbers.";
    else if (f.header.verify != VerifyStatus::Verified) p.note = "The file is not VERIFIED: the replay shows the macro only.";
    p.numbers = p.note.empty();
    std::map<std::pair<int, uint32_t>, const ClickResult*> byClick;
    if (p.numbers) for (const auto& c : f.analysis->clicks) byClick[{c.player, c.index}] = &c;
    std::array<int, 2> k{0, 0};  // action counter per player stream
    for (const auto& e : finalRunEvents(f.macro)) {
        ReplayRing r;
        r.tick = e.editedTick;
        r.player = e.player;
        r.press = e.type == EventType::Press;
        int a = k[e.player & 1]++;
        auto it = byClick.find({e.player, uint32_t(a / 2)});
        if (it != byClick.end()) {
            const ClickResult& c = *it->second;
            const WindowResult& w = (a % 2 == 0) ? c.in : c.out;
            bool bad = (c.flags & (ClickFlag::Unreliable | ClickFlag::Random | ClickFlag::NotAnalysed)) || !w.analysed ||
                       w.na || w.releaseInsensitive;
            if (!bad) {
                r.known = true;
                r.plus = w.lowerBound();
                r.window = std::clamp(w.window(), 1, 99);
                r.width = std::max(1.0, w.widthMid());   // whole-tick runs: widthMid() == window()
                r.bucket = replayBucket(r.window);
            }
        }
        p.rings.push_back(r);
    }
    p.ok = true;
    return p;
}

double rawPrecision24h(const std::vector<int>& windows, int levelTicks) {
    return rawPrecision24h(std::vector<double>(windows.begin(), windows.end()), levelTicks);
}
double rawPrecision24h(const std::vector<double>& windows, int levelTicks) {
    if (windows.empty() || levelTicks <= 0) return 0;
    const double attempt = double(levelTicks) / kTps;       // seconds per attempt (assumption: a full run)
    const double target = std::min(1.0, attempt / 86400.0); // P(L*) so that attempt / P = 24 h
    auto logP = [&](double L) {
        double s = 0;
        for (double n : windows) {
            double z = 0.5 * (n / kTps) * L;                 // s_i / sigma with sigma = 1 / L
            double p = std::erf(z / std::sqrt(2.0));
            s += std::log(std::max(p, 1e-300));
        }
        return s;
    };
    double lo = 1e-3, hi = 1e6, goal = std::log(target);
    if (logP(hi) < goal) return hi;
    for (int i = 0; i < 200; i++) {
        double mid = std::sqrt(lo * hi);
        (logP(mid) < goal ? lo : hi) = mid;
    }
    return hi;
}

std::string formatPrecision(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

}  // namespace gdfwr
