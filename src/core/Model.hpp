// GDFWR core data model. Pure C++23, no Geode dependency (unit-testable on any machine).
// Section references like "Part 1, 2.2" point to GDFWR_Full_Specification.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gdfwr {

inline constexpr int kTps = 240;               // v1 supports 240 only (Part 1, 2.4)
inline constexpr int kOffsetCap = 12;          // Analysis section 2
inline constexpr int kJump = 1;                // button 1 = jump (Part 1, 2.3)
inline constexpr uint16_t kFormatVersion = 1;  // Part 2, 2.3
inline constexpr uint16_t kMinReaderVersion = 1;
inline constexpr std::string_view kAnalysisSpec = "v2-per-action";

// ---- codes (Appendix B) ----------------------------------------------------
namespace code {
inline constexpr std::string_view CoarseInput = "COARSE_INPUT", SyntheticEvents = "SYNTHETIC_EVENTS",
    RandomUncaptured = "RANDOM_UNCAPTURED", PassedOnRetry = "PASSED_ON_RETRY", LevelMismatch = "LEVEL_MISMATCH",
    LevelChanged = "LEVEL_CHANGED", DerivedDropped = "DERIVED_DROPPED", TpsChanged = "TPS_CHANGED",
    UnreliableInput = "UNRELIABLE_INPUT", UnreliableTick = "UNRELIABLE_TICK", InvalidSequence = "INVALID_SEQUENCE",
    UnsupportedDevice = "UNSUPPORTED_DEVICE",
    ImportUnknownFormat = "IMPORT_UNKNOWN_FORMAT", ImportFileDamaged = "IMPORT_FILE_DAMAGED",
    ImportFpsNotDivisor = "IMPORT_FPS_NOT_DIVISOR", ImportPlatformer = "IMPORT_PLATFORMER",
    ImportNotFromStart = "IMPORT_NOT_FROM_START", ImportBadSequence = "IMPORT_BAD_SEQUENCE",
    ImportPlayerMismatch = "IMPORT_PLAYER_MISMATCH", ImportDuplicateDropped = "IMPORT_DUPLICATE_DROPPED",
    ImportOrphanRelease = "IMPORT_ORPHAN_RELEASE", ImportSyntheticRelease = "IMPORT_SYNTHETIC_RELEASE",
    ImportLevelUnchecked = "IMPORT_LEVEL_UNCHECKED",
    LoadNeedsNewerMod = "LOAD_NEEDS_NEWER_MOD", LoadFileDamaged = "LOAD_FILE_DAMAGED",
    SaveWriteFailed = "SAVE_WRITE_FAILED",
    ExportUncertain = "EXPORT_UNCERTAIN", ExportLevel = "EXPORT_LEVEL", ExportCheckFailed = "EXPORT_CHECK_FAILED";
}  // namespace code

// ---- statuses (Part 0, 0.6) ------------------------------------------------
enum class RecStatus : uint8_t { Partial = 0, Completed = 1 };
enum class VerifyStatus : uint8_t { NotVerified = 0, Verified = 1, Failed = 2 };
enum class EditStatus : uint8_t { Original = 0, Edited = 1 };
enum class Origin : uint8_t { Recorded = 0, Imported = 1 };
enum class StartKind : uint8_t { LevelStart = 0, CheckpointLoad = 1 };
enum class EndReason : uint8_t { None = 0, Death, Restart, CheckpointLoad, Completion, Stopped, Exit, Suspended };
enum class MarkerKind : uint8_t {
    Pause, Resume, CheckpointCreate, CheckpointDelete, CheckpointLoad, Restart, Death, Suspend, Arm, TpsChange, Completion
};
enum class EventType : uint8_t { Release = 0, Press = 1 };

namespace EventFlag { inline constexpr uint8_t Synthetic = 1, Edited = 2, UncertainTick = 4, Junction = 8; }
namespace SegFlag { inline constexpr uint8_t UnreliableInput = 1, UnreliableTick = 2, TpsChanged = 4; }

struct Warning {
    std::string code;
    std::string detail;
};

// ---- recording structures (Part 1, 2.2 / Part 2, 2.6) -----------------------
struct Event {
    uint32_t seq = 0;
    uint32_t segment = 0;
    int32_t tick = 0;        // captured tick, never changed
    int32_t editedTick = 0;  // equals tick unless edited
    uint16_t sub = 0;        // v2: where inside the tick the input landed, in 1/65536 tick. 0 = on the tick. Reserved for
                             // click-between-steps (precision mode); v1 recording always stores 0.
    uint8_t player = 0;      // 0 = P1, 1 = P2
    EventType type = EventType::Press;
    uint8_t button = kJump;
    std::string source;      // keyboard | mouse | other | synthetic | IMPORT:<format>
    uint8_t flags = 0;
    int32_t partner = -1;    // seq of the matching press/release
};

struct Segment {
    uint32_t id = 0;
    int32_t parent = -1;
    int32_t parentBookmark = -1;
    int32_t startTick = 0;
    int32_t endTick = 0;
    StartKind startKind = StartKind::LevelStart;
    EndReason endReason = EndReason::None;
    std::array<uint8_t, 2> startHold{0, 0};
    uint16_t tps = kTps;
    uint8_t flags = 0;
    float fpsAvg = 0, fpsMin = 0;
};

struct Bookmark {
    uint32_t id = 0;
    uint64_t gameId = 0;  // identity of the game's checkpoint object
    uint32_t segment = 0;
    int32_t tick = 0;
    uint32_t seq = 0;
    std::array<uint8_t, 2> hold{0, 0};
    float x = 0, y = 0;
    bool active = true;
};

struct Marker {
    MarkerKind kind = MarkerKind::Pause;
    uint32_t segment = 0;
    int32_t tick = 0;
    uint32_t seq = 0;
};

struct RandomEntry { int32_t tick = 0, trigger = 0, branch = 0; uint32_t segment = 0; };
struct FinalSpan { uint32_t segment = 0; int32_t fromTick = 0, toTick = 0; };  // toTick exclusive except the last span
struct FinalRun {
    bool exists = false;
    bool partial = false;
    std::vector<FinalSpan> spans;
    std::vector<uint32_t> syntheticSeqs;  // synthetic events added at junctions
};
struct EditRecord { uint32_t seq = 0; int32_t originalTick = 0, newTick = 0; };

struct Macro {
    std::vector<Segment> segments;
    std::vector<Bookmark> bookmarks;
    std::vector<Marker> markers;
    std::vector<Event> events;
    std::vector<RandomEntry> randomLog;
    FinalRun finalRun;
    std::vector<EditRecord> edits;
    uint64_t macroHash = 0;
};

// ---- level / timing (Part 2, 2.4 / 2.5) -------------------------------------
struct LevelInfo {
    int64_t levelId = 0;
    std::string name;
    int32_t version = 0;
    uint64_t dataHash = 0;
    std::string gameVersion;
    bool platformer = false, twoPlayer = false, dual = false;
    int32_t endTick = 0;
    bool hasSeed = false;
    int64_t seed = 0;
};
struct FpsStat { uint32_t segment = 0; float avg = 0, min = 0; };
struct TpsChange { uint32_t segment = 0; int32_t tick = 0; float from = 0, to = 0; };
struct TimingInfo {
    uint16_t tps = kTps;
    int32_t clickResolution = 1;  // ticks; > 1 => COARSE_INPUT
    bool cbf = false;
    std::vector<FpsStat> fps;
    std::vector<TpsChange> tpsChanges;
};

// ---- baseline (Part 2, 2.7) ---------------------------------------------------
struct Sample { float x1 = 0, y1 = 0, vy1 = 0, x2 = 0, y2 = 0, vy2 = 0; };
struct Baseline {
    uint64_t macroHash = 0;
    double epsilon = 0;                  // 0 = exact
    std::vector<uint64_t> hashes;        // state hash at the START of each tick of the final run
    std::vector<Sample> samples;         // position / velocity per tick
    std::vector<RandomEntry> randomLog;  // from the verification replay
    bool stale = false;
};

// ---- analysis results (Part 2, 2.8 / Analysis 9) ------------------------------
enum class StopReason : uint8_t { None = 0, Death, Cap, Clamp };
namespace ClickFlag {
inline constexpr uint32_t Clamp = 1, Cluster = 2, Drift = 4, RandomPinned = 8, Random = 16, Unreliable = 32,
    FramePerfect = 64, Edited = 128, NotAnalysed = 256, ReleaseInsensitive = 512;
}
struct WindowResult {
    bool analysed = false;
    bool na = false;  // undefined (run ends while holding)
    int early = 0, late = 0;
    // v2: where the surviving timings really end, in ticks from the recorded edge. On each side the edge lies in
    // [edge*Lo, edge*Hi). With whole-tick runs only that is [n, n+1); with sub-tick runs it is narrowed by bisection.
    // The continuous width of the window is between widthLo() and widthHi() ticks (NaN GD counts this width, so the
    // whole-tick count window() can read one higher or lower depending on where the player's click fell).
    double edgeEarlyLo = 0, edgeEarlyHi = 1, edgeLateLo = 0, edgeLateHi = 1;
    bool subTick = false;    // at least one side was narrowed below one tick
    bool extended = false;   // at least one side was searched beyond the 12-tick map (coarse steps + bisection)
    StopReason stopEarly = StopReason::None, stopLate = StopReason::None;
    bool fullScan = false;
    std::string mapEarly = std::string(12, '?');  // offsets -12 .. -1
    std::string mapLate = std::string(12, '?');   // offsets +1 .. +12
    int holes = 0, traps = 0;
    bool releaseInsensitive = false;
    bool driftValid = false;
    double driftDy = 0, driftDvy = 0;
    uint8_t limitingPlayer = 0;  // dual: bit0 = icon 1, bit1 = icon 2
    int window() const { return early + 1 + late; }
    double widthLo() const { return edgeEarlyLo + edgeLateLo; }
    double widthHi() const { return edgeEarlyHi + edgeLateHi; }
    double widthMid() const { return 0.5 * (widthLo() + widthHi()); }
    bool clamped() const { return stopEarly == StopReason::Clamp || stopLate == StopReason::Clamp; }
    bool lowerBound() const { return stopEarly != StopReason::Death || stopLate != StopReason::Death; }
};
struct ClickResult {
    uint32_t index = 0;   // 0-based click index inside its player stream
    uint8_t player = 0;
    int32_t press = 0;
    int32_t release = -1;  // -1 = run ends while holding
    WindowResult in, out;
    uint32_t flags = 0;
};
struct ClusterResult {
    uint8_t player = 0;
    int firstAction = 0, lastAction = 0;  // indices into the player's action stream
    std::vector<uint32_t> clicks;
    WindowResult win;
    uint32_t flags = 0;
};
struct Counters {
    int b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5_6 = 0, b7_10 = 0, b11_24 = 0, b25p = 0;
    int nPlus = 0, clamp = 0, unreliable = 0, random = 0, notAnalysed = 0;
};
struct Summary {
    std::vector<uint32_t> tightestInput, tightestOutput;  // click ids (position in AnalysisData::clicks), ties listed
    std::vector<uint32_t> tightestCluster;
    double avgInput = 0, avgOutput = 0;
    bool avgInputLower = false, avgOutputLower = false;
    int reliableClicks = 0;
};
struct AnalysisData {
    std::string specVersion{kAnalysisSpec};
    std::string modVersion;
    int cap = kOffsetCap;
    int fullScanTop = 20;
    double epsilon = 0;          // kept for the file format; the engine compares state exactly (no epsilon) since v2
    int tps = kTps;
    int reach = kOffsetCap;      // v2: search limit in ticks per side (== cap: no long search)
    bool wholeClick = false;     // v1 files: true (the press window moved press and release together)
    bool subTickMode = false;    // v2: widths were narrowed below one tick
    uint64_t macroHash = 0;
    bool stale = false;
    bool incomplete = false;
    std::vector<ClickResult> clicks;
    std::vector<ClusterResult> clusters;
    Counters counters;
    Summary summary;
    std::vector<Warning> warnings;
};

// ---- the whole document (Part 2) -------------------------------------------------
struct Header {
    uint16_t formatVersion = kFormatVersion;
    uint16_t minReader = kMinReaderVersion;
    std::string modVersion, gameVersion;
    int64_t createdUtc = 0;
    RecStatus rec = RecStatus::Partial;
    VerifyStatus verify = VerifyStatus::NotVerified;
    int32_t verifyFailTick = -1;
    EditStatus edit = EditStatus::Original;
    Origin origin = Origin::Recorded;
    std::string importFormat, importVersion, importFile;
    std::vector<Warning> warnings;
};
struct GdfwrFile {
    Header header;
    LevelInfo level;
    TimingInfo timing;
    Macro macro;
    std::optional<Baseline> baseline;
    std::optional<AnalysisData> analysis;
};

}  // namespace gdfwr
