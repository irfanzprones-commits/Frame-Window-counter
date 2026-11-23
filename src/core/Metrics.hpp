// Replay buckets, rings and the precision readout (Part 9).
#pragma once
#include "Model.hpp"

namespace gdfwr {

// v2: ONE bucket scheme for the replay HUD and the analysis counters (Appendix A item 1): 1, 2, 3, 4, 5-6, 7-8, 9-12, 13-24, 25+.
enum class ReplayBucket : uint8_t { W25P = 0, W13_24, W9_12, W7_8, W5_6, W4, W3, W2, W1 };  // HUD order, top to bottom (9.4)
inline constexpr int kReplayBucketCount = 9;
ReplayBucket replayBucket(int window);
const char* replayBucketLabel(ReplayBucket b);         // "9-12" ... "1" (the lowest bucket is just "1", 9.8)
uint32_t replayBucketColor(ReplayBucket b);            // 0xRRGGBB from 9.6

// One ring per final-run action, in execution order.
struct ReplayRing {
    int tick = 0;
    uint8_t player = 0;
    bool press = true;
    bool known = false;       // false: UNRELIABLE / RANDOM / NOT_ANALYSED / N/A -> grey ring "?", not counted
    bool insensitive = false; // v0.3.0: a release whose timing never changed anything: grey ring "any" (measured, but nothing to count)
    bool plus = false;        // value is a lower bound (N+ or CLAMP): shown as "N+"
    int window = 0;           // whole-tick count at the recorded alignment, 1..99
    double width = 0;         // continuous width in ticks (middle of the known range); equals `window` with whole-tick runs
    ReplayBucket bucket = ReplayBucket::W9_12;
};
struct ReplayPlan {
    bool ok = false;          // the replay can start: a macro (final run) exists. Analysis and verification are NOT needed (v0.1.3)
    std::string reason;       // why it cannot start (only when !ok)
    bool numbers = false;     // the rings carry window numbers (the analysis is CURRENT and the file is VERIFIED)
    std::string note;         // why there are no numbers (only when ok && !numbers); empty otherwise
    std::vector<ReplayRing> rings;
};
// A replay is just "play the macro and watch": it needs a macro and nothing else, so the user can check by eye whether the
// macro passes before (or without) any analysis. Source of the numbers [DECISION, Appendix A item 3]: the stored ANALYSIS
// section; it must be CURRENT and the file VERIFIED, otherwise every ring is grey and nothing is counted.
// Action 2i-1 of a player stream = press window of click i, action 2i = its release window (Part 5, step 5; v2: each edge on its own).
ReplayPlan buildReplayPlan(const GdfwrFile& f);

// v0.3.0 ring life: a ring appears at full strength at once (no growing, no animation), stays fully visible for `hold` seconds and then
// fades out linearly over `fade` seconds. Returns the opacity 0..1; 0 means "remove the ring".
inline constexpr float kRingHoldDefault = 1.0f, kRingFadeDefault = 2.0f;
float ringOpacity(float age, float hold, float fade);

// "Raw Precision (24h) in sigma/s". PROVISIONAL: the model is my reading from Appendix A item 5 and is
// not confirmed by the owner. windows = N_i of every counted action; levelTicks = length of one attempt.
double rawPrecision24h(const std::vector<int>& windows, int levelTicks);
// Same model with the continuous widths (ticks), which is what NaN GD's counts are. With whole-tick runs the width is the integer count.
double rawPrecision24h(const std::vector<double>& widths, int levelTicks);
std::string formatPrecision(double v);

}  // namespace gdfwr
