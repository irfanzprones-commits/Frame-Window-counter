// Standalone tests for the Geode-free core. Build: see tests/run.sh (no Geode needed).
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>

#include "core/Analysis.hpp"
#include "core/FileIO.hpp"
#include "core/Formats.hpp"
#include "core/Metrics.hpp"
#include "core/Repair.hpp"
#include "core/Session.hpp"
#include "core/Util.hpp"

using namespace gdfwr;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; auto _a = (a); auto _b = (b); if (!(_a == _b)) { g_fail++; std::printf("  FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define TEST(name) static void name(); struct R_##name { R_##name() { tests().push_back({#name, &name}); } } r_##name; static void name()
static std::vector<std::pair<const char*, void (*)()>>& tests() { static std::vector<std::pair<const char*, void (*)()>> t; return t; }

// ---- helpers -------------------------------------------------------------------------------
static GdfwrFile fileFromActions(const std::vector<std::array<int, 3>>& acts, int endTick) {  // {tick, player, press}
    GdfwrFile f;
    f.header.rec = RecStatus::Completed;
    f.header.verify = VerifyStatus::Verified;
    f.header.modVersion = "test";
    f.level.levelId = 4242; f.level.name = "Test Level"; f.level.dataHash = 777; f.level.endTick = endTick;
    Segment s; s.id = 0; s.endTick = endTick; s.endReason = EndReason::Completion;
    f.macro.segments.push_back(s);
    for (auto a : acts) {
        Event e; e.seq = uint32_t(f.macro.events.size()); e.tick = e.editedTick = a[0]; e.player = uint8_t(a[1]);
        e.type = a[2] ? EventType::Press : EventType::Release; e.source = "keyboard";
        f.macro.events.push_back(e);
    }
    f.macro.finalRun.exists = true;
    f.macro.finalRun.spans.push_back({0, 0, endTick});
    f.macro.macroHash = computeMacroHash(f.macro);
    return f;
}

// A mock game: a click survives iff alive(player, click, P, R). Events not in the request keep their original tick.
struct MockSim : ISimulator {
    std::vector<std::array<int, 4>> clicks[2];  // per player: {P, R, pressSeq, releaseSeq}
    std::function<bool(int, int, int, int)> alive = [](int, int, int, int) { return true; };
    std::set<int> desyncStarts;
    bool convergeAll = false;
    int runs = 0;
    SimResult run(const SimRequest& rq) override {
        runs++;
        SimResult r;
        if (rq.compareBaseline && desyncStarts.count(rq.startTick)) { r.desync = true; r.desyncTick = rq.startTick + 1; return r; }
        int drift0P = 0, drift0R = 0;
        for (int p = 0; p < 2; p++) {
            for (size_t c = 0; c < clicks[p].size(); c++) {
                int P = clicks[p][c][0], R = clicks[p][c][1];
                for (const auto& in : rq.inputs) {
                    if (int(in.order) == clicks[p][c][2]) P = in.tick;
                    if (int(in.order) == clicks[p][c][3]) R = in.tick;
                }
                if (p == 0 && c == 0) { drift0P = P; drift0R = R; }
                if (P > rq.horizonTick) continue;
                int Reff = R <= rq.horizonTick ? R : 1 << 28;
                if (!alive(p, int(c), P, Reff) && !r.died) { r.died = true; r.deathTick = std::max(rq.startTick, std::min(P, R)); r.diedMask = uint8_t(1 << p); }
            }
        }
        if (rq.driftTick >= 0) { r.driftValid = true; r.y = 100 + 0.5 * (drift0P - 532); r.vy = 0.1 * (drift0R - 673); }
        if (convergeAll && !rq.compareBaseline) { r.converged = true; r.convergedTick = rq.startTick + 20; }
        return r;
    }
};
static MockSim workedExample() {  // Analysis worked example: P1 532, R1 673, P2 783, R2 905
    MockSim m;
    m.clicks[0] = {{532, 673, 0, 1}, {783, 905, 2, 3}};
    m.alive = [](int, int c, int P, int R) {
        if (c == 0) return P >= 527 && P <= 540 && R - P >= 138;
        return P >= 770 && P <= 800 && R - P >= 100;
    };
    return m;
}
static AnalysisInput inputFor(const GdfwrFile& f, int end) {
    AnalysisInput in;
    in.actions = makeActions(finalRunEvents(f.macro));
    in.levelEndTick = end;
    in.baselineSamples.assign(size_t(end + 2), Sample{});
    for (auto& s : in.baselineSamples) s.y1 = 100;
    return in;
}
// The old tests describe the v1 rules (the press window moves press AND release together), so they run in wholeClick mode.
// The v2 default (press-only / release-only windows) has its own tests further down.
static AnalysisParams legacy() { AnalysisParams p; p.wholeClick = true; return p; }
static AnalysisData analyse(AnalysisInput in, MockSim& sim, AnalysisParams p = legacy()) {
    AnalysisEngine e(std::move(in), p, sim);
    e.runAll();
    return e.takeResult();
}

// ---- util ----------------------------------------------------------------------------------
TEST(crc_and_io) {
    const char* s = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
    ByteWriter w; w.u16(0xBEEF); w.i32(-5); w.str("hi"); w.f64(1.5);
    ByteReader r(w.buf.data(), w.buf.size());
    CHECK_EQ(r.u16(), 0xBEEF); CHECK_EQ(r.i32(), -5); CHECK_EQ(r.str(), "hi"); CHECK_EQ(r.f64(), 1.5); CHECK(r.ok());
    r.u32(); CHECK(!r.ok());  // reading past the end fails safely
    ByteReader bad(w.buf.data(), 3); CHECK_EQ(bad.count(1), 0u); CHECK(!bad.ok());
}

// ---- Part 1: recording rules ------------------------------------------------------------------
static SessionConfig cfg() { SessionConfig c; c.level.levelId = 99; c.level.name = "L"; c.modVersion = "t"; c.createdUtc = 1; return c; }

TEST(final_run_rule) {  // Part 1, 2.2: segment 1 dies at 500 after a checkpoint at 300; segment 2 loads it and completes
    SessionModel s; s.begin(cfg(), 0, {0, 0});
    s.onButton(0, true, 100, "keyboard"); s.onButton(0, false, 150, "keyboard");
    s.onButton(0, true, 250, "keyboard"); s.onButton(0, false, 280, "keyboard");
    s.onCheckpointCreated(1, 300, {0, 0}, 0, 0);
    s.onButton(0, true, 320, "keyboard"); s.onButton(0, false, 350, "keyboard");   // dead branch
    s.onDeath(500);
    s.onReset(500, 300, true, {0, 0}, 1);
    s.onButton(0, true, 310, "keyboard"); s.onButton(0, false, 400, "keyboard");
    s.onButton(0, true, 500, "keyboard"); s.onButton(0, false, 520, "keyboard");
    CHECK_EQ(s.liveActionCount(), 8);
    s.complete(750);
    GdfwrFile f = s.finish("2.2081");
    CHECK(f.header.rec == RecStatus::Completed);
    CHECK_EQ(f.macro.segments.size(), size_t(2));
    CHECK(f.macro.segments[0].endReason == EndReason::Death);
    CHECK(f.macro.segments[1].startKind == StartKind::CheckpointLoad);
    CHECK_EQ(f.macro.segments[1].parent, 0);
    auto ev = finalRunEvents(f.macro);
    std::vector<int> ticks; for (auto& e : ev) ticks.push_back(e.editedTick);
    CHECK((ticks == std::vector<int>{100, 150, 250, 280, 310, 400, 500, 520}));
    CHECK_EQ(f.macro.events.size(), size_t(10));
    CHECK(validateEvents(ev).ok);
    CHECK_EQ(f.macro.finalRun.spans.size(), size_t(2));
    CHECK_EQ(f.macro.finalRun.spans[0].toTick, 300);
}

TEST(junction_hold_rule) {  // Part 1, 2.2: held before the bookmark, released after -> synthetic release at the junction
    SessionModel s; s.begin(cfg(), 0, {0, 0});
    s.onButton(0, true, 280, "keyboard");
    s.onCheckpointCreated(1, 300, {1, 0}, 0, 0);
    s.onDeath(400);
    s.onReset(400, 300, true, {0, 0}, 1);
    s.onButton(0, true, 320, "keyboard"); s.onButton(0, false, 340, "keyboard");
    s.complete(500);
    GdfwrFile f = s.finish("2.2081");
    auto ev = finalRunEvents(f.macro);
    CHECK_EQ(ev.size(), size_t(4));
    CHECK_EQ(ev[1].editedTick, 300); CHECK(ev[1].type == EventType::Release); CHECK(ev[1].flags & EventFlag::Synthetic);
    CHECK(validateEvents(ev).ok);
    CHECK(hasWarning(f, code::SyntheticEvents));
    CHECK_EQ(f.macro.finalRun.syntheticSeqs.size(), size_t(1));
    // opposite case: released before, held after -> synthetic press
    SessionModel s2; s2.begin(cfg(), 0, {0, 0});
    s2.onCheckpointCreated(1, 300, {0, 0}, 0, 0);
    s2.onDeath(350);
    s2.onReset(350, 300, true, {1, 0}, 1);
    s2.onButton(0, false, 310, "keyboard");
    s2.complete(400);
    auto e2 = finalRunEvents(s2.finish("x").macro);
    CHECK_EQ(e2.size(), size_t(2)); CHECK(e2[0].type == EventType::Press); CHECK_EQ(e2[0].editedTick, 300);
}

TEST(session_states_and_flags) {
    SessionModel s; s.begin(cfg(), 0, {0, 0});
    CHECK(!s.onButton(0, false, 5, "keyboard"));               // not an effective change
    CHECK(s.onButton(0, true, 10, "keyboard"));
    CHECK(!s.onButton(0, true, 11, "keyboard"));               // already held
    s.checkHold({0, 0});                                       // the game does not hold: a missed event
    CHECK(s.macro().segments[0].flags & SegFlag::UnreliableInput);
    s.suspend(50); CHECK(s.state() == SessionModel::State::Suspended);
    CHECK(!s.onButton(0, false, 60, "keyboard"));              // nothing captured while suspended
    s.arm(); CHECK(s.state() == SessionModel::State::Armed);
    s.onReset(60, 0, true, {0, 0}, 0);                         // the next restart starts a new segment
    CHECK(s.state() == SessionModel::State::Active);
    CHECK_EQ(s.macro().segments.size(), size_t(2));
    s.stop(100);
    GdfwrFile f = s.finish("v");
    CHECK(f.header.rec == RecStatus::Partial);
    CHECK(hasWarning(f, code::UnreliableInput));
    // a checkpoint made while the recorder is suspended hides inputs
    { SessionModel q; q.begin(cfg(), 0, {0, 0}); q.suspend(10); q.onCheckpointCreated(1, 50, {0, 0}, 0, 0); q.arm(); q.onReset(60, 50, true, {0, 0}, 1);
      CHECK(q.macro().segments.back().flags & SegFlag::UnreliableInput); }
    // pause fix (2.5): the game released the button during a pause -> synthetic release at the resume tick
    SessionModel p; p.begin(cfg(), 0, {0, 0});
    p.onButton(0, true, 10, "mouse"); p.onGamePause(20); p.onGameResume(20, {0, 0});
    CHECK_EQ(p.macro().events.size(), size_t(2)); CHECK(p.macro().events[1].flags & EventFlag::Synthetic);
}

TEST(edit_rules) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {150, 0, 0}, {200, 0, 1}, {200, 0, 0}, {300, 0, 1}, {320, 0, 0}}, 400);
    uint64_t h0 = f.macro.macroHash;
    CHECK(moveEvent(f.macro, 0, 105).ok);
    CHECK(!moveEvent(f.macro, 0, 150).ok);                 // must stay 1 tick before the next event
    CHECK(!moveEvent(f.macro, 1, 105).ok);                 // 1 tick after the previous event
    CHECK(!moveEvent(f.macro, 0, -1).ok);
    CHECK(moveEvent(f.macro, 2, 210).ok);                  // same-tick pair (200/200) moves together
    CHECK_EQ(findEvent(f.macro, 3)->editedTick, 210);
    CHECK(validateEvents(finalRunEvents(f.macro)).ok);
    CHECK(f.macro.macroHash != h0);
    f.baseline = Baseline{}; f.baseline->macroHash = h0; f.analysis = AnalysisData{};
    applyEditSideEffects(f);
    CHECK(f.header.edit == EditStatus::Edited); CHECK(f.header.verify == VerifyStatus::NotVerified);
    CHECK(!f.baseline.has_value()); CHECK(f.analysis->stale);
    CHECK(restoreEvent(f.macro, 0).ok);
    restoreAll(f.macro); applyEditSideEffects(f);
    CHECK(f.header.edit == EditStatus::Original); CHECK_EQ(f.macro.macroHash, h0);
    auto bad = finalRunEvents(fileFromActions({{10, 0, 1}, {20, 0, 1}}, 30).macro);
    CHECK(!validateEvents(bad).ok);                         // INVALID_SEQUENCE
    CHECK(validateEvents(finalRunEvents(fileFromActions({{10, 0, 1}}, 30).macro)).ok);  // run ends while holding
}


// Regression: crash after finishing a recording (log 2026-11-12). abandon() emptied the segments but kept the "current
// segment" index, so the next level reset (noteFps) wrote into an empty vector at 0x40.
TEST(abandon_is_safe) {
    SessionModel s; s.begin(cfg(), 0, {0, 0});
    s.onButton(0, true, 10, "keyboard"); s.onButton(0, false, 20, "keyboard");
    s.onCheckpointCreated(1, 5, {0, 0}, 0, 0); s.onDeath(30); s.onReset(30, 5, true, {0, 0}, 1);   // two segments: cur_ == 1
    s.complete(100); (void)s.finish("v"); s.abandon();
    s.noteFps(90.f, 60.f); s.flagSegment(1); s.onTpsChange(5, 240.f, 120.f); s.onRandomBranch(1, 2, 3);
    s.onCheckpointCreated(2, 1, {0, 0}, 0, 0); s.onDeath(1); s.onGamePause(1); s.onGameResume(1, {0, 0});
    s.checkHold({1, 1}); s.suspend(1); s.arm(); s.stop(2); s.complete(3); s.onReset(1, 0, true, {0, 0}, 0);
    CHECK(s.state() == SessionModel::State::Idle); CHECK_EQ(s.currentSegment(), -1); CHECK_EQ(s.liveActionCount(), 0);
    s.begin(cfg(), 0, {0, 0});                                   // a second recording right after works normally
    CHECK(s.onButton(0, true, 5, "keyboard")); CHECK_EQ(s.liveActionCount(), 1);
}

// ---- Part 2: file format ----------------------------------------------------------------------
static GdfwrFile richFile() {
    GdfwrFile f = fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000);
    f.baseline = Baseline{}; f.baseline->macroHash = f.macro.macroHash; f.baseline->hashes = {1, 2, 3}; f.baseline->samples.assign(3, Sample{});
    MockSim m = workedExample();
    f.analysis = analyse(inputFor(f, 1000), m);
    f.analysis->macroHash = f.macro.macroHash;
    f.header.warnings.push_back({"X", "detail"});
    return f;
}
TEST(file_roundtrip_and_damage) {
    GdfwrFile f = richFile();
    auto bytes = serialize(f);
    auto r = parse(bytes.data(), bytes.size());
    CHECK(r.ok); CHECK(r.loadWarnings.empty());
    CHECK(r.file.analysis.has_value()); CHECK(r.file.baseline.has_value());
    CHECK_EQ(serialize(r.file), bytes);                       // exact roundtrip
    CHECK(!r.file.analysis->stale);
    // find a section and flip one payload byte
    auto sectionAt = [&](const char* id) { for (size_t i = 0; i + 4 < bytes.size(); i++) if (std::equal(id, id + 4, bytes.begin() + long(i))) return i; return size_t(0); };
    { auto b = bytes; b[sectionAt("MACR") + 14] ^= 0x55; auto x = parse(b.data(), b.size());   // S-2
      CHECK(!x.ok); CHECK_EQ(x.errorCode, std::string(code::LoadFileDamaged)); }
    { auto b = bytes; b[sectionAt("ANAL") + 14] ^= 0x55; auto x = parse(b.data(), b.size());   // S-3
      CHECK(x.ok); CHECK(!x.file.analysis.has_value()); CHECK_EQ(x.loadWarnings.size(), size_t(1)); CHECK_EQ(x.loadWarnings[0].code, std::string(code::DerivedDropped)); }
    { auto b = bytes; b[10] = 9; auto x = parse(b.data(), b.size());                            // minReader = 9
      CHECK(!x.ok); CHECK_EQ(x.errorCode, std::string(code::LoadNeedsNewerMod)); }
    { auto b = bytes; b.resize(b.size() / 2); CHECK(!parse(b.data(), b.size()).ok); }          // truncated
    { std::vector<uint8_t> junk(64, 0x41); CHECK(!parse(junk.data(), junk.size()).ok); }
    // S-4: macro edited, derived data kept -> STALE (stale rule 2.9)
    GdfwrFile g = f; g.macro.events[0].editedTick += 1;
    auto gb = serialize(g); auto gr = parse(gb.data(), gb.size());
    CHECK(gr.ok); CHECK(gr.file.analysis->stale); CHECK(gr.file.baseline->stale);
}
TEST(level_compare_and_gate) {
    GdfwrFile f = richFile();
    CHECK(compareLevel(f.level, true, 4242, 777) == LevelMatch::Same);
    CHECK(compareLevel(f.level, true, 4242, 778) == LevelMatch::Changed);
    CHECK(compareLevel(f.level, true, 1, 777) == LevelMatch::Mismatch);
    CHECK(compareLevel(f.level, false, 0, 0) == LevelMatch::NoLevelLoaded);
    std::vector<Warning> w; applyLevelMatch(f, LevelMatch::Changed, w);
    CHECK(f.analysis->stale && f.baseline->stale); CHECK_EQ(w[0].code, std::string(code::LevelChanged));
    CHECK(analysisGate(richFile(), true).empty());
    CHECK(!analysisGate(richFile(), false).empty());
    GdfwrFile p = richFile(); p.header.rec = RecStatus::Partial; CHECK(!analysisGate(p, true).empty());   // recorded, never finished: refused
    GdfwrFile tw = richFile(); tw.header.warnings.push_back({std::string(code::TpsChanged), ""});
    CHECK(analysisGate(tw, true).empty());                                                               // a TPS warning no longer blocks Analyze: the check replays at 240 TPS and fails by itself if the recording was not
    GdfwrFile t2 = richFile(); t2.timing.tps = 360; CHECK(!analysisGate(t2, true).empty());                // but a file that says it was recorded at another TPS is still refused
    // There is no Verify button: the verification is step 1 of Analyze. So an IMPORTED file (PARTIAL until a verification reaches the
    // end of the level, D2) must pass the gate, otherwise nothing could ever verify it.
    GdfwrFile ip = richFile(); ip.header.rec = RecStatus::Partial; ip.header.origin = Origin::Imported;
    ip.header.verify = VerifyStatus::NotVerified; CHECK(analysisGate(ip, true).empty());
    ip.header.verify = VerifyStatus::Failed; CHECK(analysisGate(ip, true).empty());                        // a failed import can be tried again
    CHECK(!analysisGate(ip, false).empty());                                                               // ... but not on another level
    ip.level.platformer = true; CHECK(!analysisGate(ip, true).empty());                                    // ... and the other gate rules stay
    GdfwrFile pl = richFile(); pl.level.platformer = true; CHECK(!analysisGate(pl, true).empty());
    CHECK_EQ(std::string(analysisStateName(f)), "STALE");
}
TEST(atomic_save_and_names) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "gdfwr_test";
    fs::remove_all(dir);
    fs::path p = dir / "a.gdfwr";
    GdfwrFile f = richFile();
    CHECK(saveAtomic(p, f).ok); CHECK(fs::exists(p)); CHECK(!fs::exists(p.string() + ".tmp"));
    CHECK(saveAtomic(p, f).ok); CHECK(fs::exists(p.string() + ".bak"));               // previous version kept
    std::vector<uint8_t> back; CHECK(readWholeFile(p, back)); CHECK(parse(back.data(), back.size()).ok);
    CHECK(!saveAtomic("/proc/nonexistent_dir/x.gdfwr", f).ok);                          // SAVE_WRITE_FAILED, nothing breaks
    std::tm t{}; t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 10; t.tm_hour = 7; t.tm_min = 5;
    CHECK_EQ(defaultFileName("Bad:Name/<x>?", t, true, ".gdfwr"), std::string("Bad_Name__x___2026-11-10_0705.gdfwr"));
    CHECK_EQ(sanitizeFileName("..."), std::string("level"));
    fs::remove_all(dir);
}

// ---- Part 8: import / export --------------------------------------------------------------------
static ImportOptions impOpt() { ImportOptions o; o.haveLoadedLevel = true; o.levelId = 4242; o.levelName = "Test"; o.levelHash = 5; o.levelTwoPlayer = true; o.gameVersion = "2.2081"; o.modVersion = "t"; return o; }
static SourceMacro src(double fps, std::vector<SourceInput> in) { SourceMacro s; s.formatName = "GDR2"; s.fps = fps; s.inputs = std::move(in); return s; }

TEST(import_rules) {
    auto I = [](uint64_t f, bool down, bool p2 = false) { return SourceInput{f, kJump, p2, down}; };
    { auto r = importMacro(src(240, {I(10, true), I(20, false), I(30, true), I(40, false)}), impOpt());   // clean import
      CHECK(r.ok); CHECK(r.file.header.origin == Origin::Imported); CHECK(r.file.header.verify == VerifyStatus::NotVerified);
      CHECK(r.file.header.rec == RecStatus::Partial); CHECK_EQ(r.file.macro.events.size(), size_t(4));
      CHECK(validateEvents(finalRunEvents(r.file.macro)).ok); CHECK(!hasWarning(r.file, code::CoarseInput)); }
    { auto r = importMacro(src(60, {I(10, true), I(12, false)}), impOpt());                                // I-2: 60 FPS -> K = 4
      CHECK(r.ok); CHECK_EQ(r.file.macro.events[0].editedTick, 40); CHECK_EQ(r.file.macro.events[1].editedTick, 48);
      CHECK(hasWarning(r.file, code::CoarseInput)); CHECK(r.file.macro.events[0].flags & EventFlag::UncertainTick); CHECK_EQ(r.file.timing.clickResolution, 4); }
    { auto r = importMacro(src(144, {I(1, true), I(2, false)}), impOpt());                                 // I-3: refused, not rounded
      CHECK(!r.ok); CHECK_EQ(r.errorCode, std::string(code::ImportFpsNotDivisor)); }
    { auto r = importMacro(src(480, {I(1, true), I(2, false)}), impOpt()); CHECK(!r.ok); }
    { auto r = importMacro(src(240, {I(10, true), I(12, true), I(20, false), I(25, false)}), impOpt());    // I-4: duplicates dropped, reported
      CHECK(r.ok); CHECK_EQ(r.file.macro.events.size(), size_t(2)); CHECK(hasWarning(r.file, code::ImportDuplicateDropped)); }
    { auto r = importMacro(src(240, {I(5, false), I(10, true), I(20, false)}), impOpt()); CHECK(r.ok); CHECK(hasWarning(r.file, code::ImportOrphanRelease)); }
    { auto r = importMacro(src(240, {I(10, true), I(20, false), I(30, true)}), impOpt());                  // trailing press -> synthetic release
      CHECK(r.ok); CHECK_EQ(r.file.macro.events.size(), size_t(4)); CHECK(r.file.macro.events[3].flags & EventFlag::Synthetic);
      CHECK(hasWarning(r.file, code::ImportSyntheticRelease)); }
    { SourceMacro m = src(240, {I(10, true), I(20, false)}); m.platformer = true; CHECK_EQ(importMacro(m, impOpt()).errorCode, std::string(code::ImportPlatformer)); }
    { SourceMacro m = src(240, {I(10, true), I(20, false)}); m.startsFromCheckpoint = true; CHECK_EQ(importMacro(m, impOpt()).errorCode, std::string(code::ImportNotFromStart)); }
    { ImportOptions o = impOpt(); o.levelTwoPlayer = false; CHECK_EQ(importMacro(src(240, {I(10, true, true), I(20, false, true)}), o).errorCode, std::string(code::ImportPlayerMismatch)); }
    { SourceMacro m = src(240, {I(10, true), I(20, false)}); m.levelId = 7;                                // other level: ask first, then flag it
      auto r = importMacro(m, impOpt()); CHECK(!r.ok); CHECK(r.askLevelMismatch);
      ImportOptions o = impOpt(); o.acceptLevelMismatch = true; auto r2 = importMacro(m, o); CHECK(r2.ok); CHECK(hasWarning(r2.file, code::LevelMismatch)); }
    { ImportOptions o = impOpt(); o.haveLoadedLevel = false; CHECK(!importMacro(src(240, {I(10, true), I(20, false)}), o).ok); }
    { ImportOptions o = impOpt(); o.frameOffset = 3; auto r = importMacro(src(240, {I(10, true), I(20, false)}), o); CHECK_EQ(r.file.macro.events[0].editedTick, 13); }
}
TEST(gdr2_roundtrip_and_export) {
    auto I = [](uint64_t f, bool down, bool p2 = false) { return SourceInput{f, kJump, p2, down}; };
    // a long delta (> 63 frames) exercises the continuation bytes of the packed varint
    auto r = importMacro(src(240, {I(10, true), I(300, false), I(1000, true), I(1001, false), I(50, true, true), I(80, false, true)}), impOpt());
    CHECK(r.ok);
    auto bytes = exportGdr2(r.file, 0);
    const IFormatReader* rd = detectReader(bytes.data(), bytes.size(), ".gdr2");
    CHECK(rd != nullptr); CHECK_EQ(rd->info().name, std::string("GDR2"));
    auto p = rd->parse(bytes.data(), bytes.size());
    CHECK(p.ok); CHECK_EQ(p.macro.inputs.size(), size_t(6)); CHECK_EQ(p.macro.fps, 240.0);
    auto again = importMacro(p.macro, impOpt());
    CHECK(again.ok); CHECK_EQ(again.file.macro.macroHash, r.file.macro.macroHash);        // import -> export -> import is lossless
    CHECK(exportCheck(r.file, bytes).empty());
    CHECK(!exportCheck(r.file, std::vector<uint8_t>{'G', 'D', 'R', 9}).empty());
    // offset symmetry (8.6.4 / 8.7.2)
    ImportOptions o = impOpt(); o.frameOffset = 2;
    auto off = importMacro(p.macro, o); CHECK(off.ok);
    auto b2 = exportGdr2(off.file, 2); CHECK_EQ(rd->parse(b2.data(), b2.size()).macro.inputs[0].frame, p.macro.inputs[0].frame);
    CHECK(!exportLossList(r.file).empty());
    CHECK(!exportWarnings(r.file, true).empty());
    // damaged data never crashes the reader
    for (size_t cut = 0; cut < bytes.size(); cut++) rd->parse(bytes.data(), cut);
    auto h = bytes; h[0] = 'X'; CHECK(detectReader(h.data(), h.size(), ".bin") == nullptr);
    CHECK(detectReader(h.data(), h.size(), ".GDR2") != nullptr);                            // extension is the second choice
    CHECK(!allReaders()[1]->info().enabled);                                                // xdBot stays disabled until its layout is verified
}

// ---- Part 4: analysis ------------------------------------------------------------------------------
TEST(worked_example) {
    GdfwrFile f = fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000);
    MockSim m = workedExample();
    AnalysisParams p = legacy(); p.fullScanTop = 0;                         // fast mode only
    AnalysisEngine e(inputFor(f, 1000), p, m);
    e.runAll();
    const auto& d = e.result();
    CHECK_EQ(d.clicks.size(), size_t(2));
    const auto& c0 = d.clicks[0];
    CHECK_EQ(c0.in.early, 5); CHECK_EQ(c0.in.late, 8); CHECK_EQ(c0.in.window(), 14);
    CHECK(c0.in.stopEarly == StopReason::Death); CHECK(c0.in.stopLate == StopReason::Death);
    CHECK_EQ(c0.out.early, 3); CHECK_EQ(c0.out.late, 12); CHECK_EQ(c0.out.window(), 16);
    CHECK(c0.out.stopLate == StopReason::Cap); CHECK(c0.out.lowerBound());
    CHECK_EQ(c0.in.mapEarly, std::string("......XPPPPP"));         // fast mode: '.' = not tested after the first failure
    CHECK_EQ(c0.in.mapLate, std::string("PPPPPPPPX..."));
    CHECK(!c0.in.fullScan);
    CHECK_EQ(d.clicks[1].in.window(), 25);
    CHECK_EQ(d.summary.tightestInput.size(), size_t(1)); CHECK_EQ(d.summary.tightestInput[0], 0u);
    CHECK(d.summary.avgInputLower); CHECK_EQ(d.summary.avgInput, 19.5);
    CHECK(e.progress().done == e.progress().total);
    int fastRuns = m.runs;
    // second pass: the full scan fills the whole map
    MockSim m2 = workedExample();
    auto full = analyse(inputFor(f, 1000), m2);
    CHECK_EQ(full.clicks[0].in.mapEarly, std::string("XXXXXXXPPPPP"));
    CHECK_EQ(full.clicks[0].in.mapLate, std::string("PPPPPPPPXXXX"));
    CHECK(full.clicks[0].in.fullScan); CHECK_EQ(full.clicks[0].in.holes, 0); CHECK_EQ(full.clicks[0].in.traps, 0);
    CHECK(m2.runs > fastRuns);                                      // fast mode really stops early
    CHECK_EQ(full.clicks[0].in.window(), c0.in.window());           // both modes agree on the window
}
TEST(holes_and_traps) {
    GdfwrFile f = fileFromActions({{532, 0, 1}, {673, 0, 0}}, 700);
    MockSim m; m.clicks[0] = {{532, 673, 0, 1}};
    m.alive = [](int, int, int P, int R) { int s = P - 532; return !(s <= -9 || s == -6 || s >= 9) && R - P >= 138; };
    auto d = analyse(inputFor(f, 700), m);
    CHECK_EQ(d.clicks[0].in.mapEarly, std::string("XXXXPPXPPPPP"));   // the example of Analysis 9
    CHECK_EQ(d.clicks[0].in.early, 5); CHECK_EQ(d.clicks[0].in.late, 8);
    CHECK_EQ(d.clicks[0].in.holes, 2); CHECK_EQ(d.clicks[0].in.traps, 1);
    MockSim t; t.clicks[0] = {{532, 673, 0, 1}};
    t.alive = [](int, int, int P, int) { return P != 535; };           // a single bad tick at +3
    auto dt = analyse(inputFor(f, 700), t);
    CHECK_EQ(dt.clicks[0].in.late, 2); CHECK_EQ(dt.clicks[0].in.traps, 1); CHECK_EQ(dt.clicks[0].in.holes, 9);
}
TEST(clamps_and_clusters) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {150, 0, 0}, {158, 0, 1}, {200, 0, 0}, {400, 0, 1}, {405, 0, 0}}, 500);
    MockSim m; m.clicks[0] = {{100, 150, 0, 1}, {158, 200, 2, 3}, {400, 405, 4, 5}};
    auto d = analyse(inputFor(f, 500), m);
    CHECK_EQ(d.clicks[0].in.late, 7); CHECK(d.clicks[0].in.stopLate == StopReason::Clamp);   // R1 + 8 >= P2
    CHECK_EQ(d.clicks[0].in.mapLate, std::string("PPPPPPP-----"));
    CHECK_EQ(d.clicks[1].in.early, 7); CHECK(d.clicks[1].in.stopEarly == StopReason::Clamp);
    CHECK_EQ(d.clicks[1].in.mapEarly, std::string("-----PPPPPPP"));
    CHECK(d.clicks[0].flags & ClickFlag::Clamp); CHECK(d.clicks[1].flags & ClickFlag::Clamp);
    CHECK(d.clicks[2].out.early == 4 && d.clicks[2].out.stopEarly == StopReason::Clamp);      // Hold = 5: R - d <= P at d = 5
    CHECK_EQ(d.counters.clamp, 4);                                                            // 0.in, 0.out(late), 1.in(early), 2.out(early)
    CHECK_EQ(d.clusters.size(), size_t(1));                                                   // release 1 + press 2 are 8 ticks apart
    CHECK((d.clusters[0].clicks == std::vector<uint32_t>{0, 1}));
    CHECK(d.clicks[0].flags & ClickFlag::Cluster); CHECK(!(d.clicks[2].flags & ClickFlag::Cluster));
    CHECK_EQ(d.clusters[0].win.window(), 25);                                                 // no clamp inside a cluster except tick 0
    CHECK(d.summary.tightestInput.size() == 1 && d.summary.tightestInput[0] == 2);            // clicks with a clamped side are not ranked
    // a cluster shift near tick 0 clamps at the level start
    GdfwrFile g = fileFromActions({{5, 0, 1}, {9, 0, 0}, {14, 0, 1}, {18, 0, 0}}, 100);
    MockSim m3; m3.clicks[0] = {{5, 9, 0, 1}, {14, 18, 2, 3}};
    auto d3 = analyse(inputFor(g, 100), m3);
    CHECK_EQ(d3.clusters.size(), size_t(1)); CHECK_EQ(d3.clusters[0].win.early, 5); CHECK(d3.clusters[0].win.stopEarly == StopReason::Clamp);
}
TEST(states_and_flags) {
    GdfwrFile f = fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000);
    { MockSim m = workedExample(); m.desyncStarts = {771};                                      // click 2 desyncs from its checkpoint (4.10)
      auto d = analyse(inputFor(f, 1000), m);
      CHECK(d.clicks[1].flags & ClickFlag::Unreliable); CHECK(!d.clicks[1].in.analysed);
      CHECK_EQ(d.counters.unreliable, 2); CHECK_EQ(d.summary.reliableClicks, 1); CHECK(d.clicks[0].in.analysed); }
    { MockSim m = workedExample(); auto in = inputFor(f, 1000);
      in.regionFlags = [](int from, int, int) { return from >= 700 ? ClickFlag::Random : 0u; };
      auto d = analyse(std::move(in), m); CHECK(d.clicks[1].flags & ClickFlag::Random); CHECK_EQ(d.counters.random, 2);
      CHECK(d.summary.tightestInput.size() == 1 && d.summary.tightestInput[0] == 0); }
    { MockSim m = workedExample(); auto in = inputFor(f, 1000); in.analyseUntilTick = 950;      // verification failed at 950: "Continue"
      auto d = analyse(std::move(in), m); CHECK(d.clicks[1].flags & ClickFlag::NotAnalysed); CHECK(d.clicks[0].in.analysed); CHECK_EQ(d.counters.notAnalysed, 2); }
    { GdfwrFile g = fileFromActions({{532, 0, 1}, {673, 0, 0}}, 700);                              // 4.5: the release changes nothing
      MockSim m; m.clicks[0] = {{532, 673, 0, 1}}; m.convergeAll = true; AnalysisParams p = legacy(); p.detectConvergence = true;
      auto d = analyse(inputFor(g, 700), m, p);
      CHECK(d.clicks[0].out.releaseInsensitive); CHECK(d.clicks[0].flags & ClickFlag::ReleaseInsensitive);
      CHECK(d.summary.tightestOutput.empty()); CHECK(!d.clicks[0].in.releaseInsensitive); CHECK_EQ(d.counters.nPlus, 1); }
    { MockSim m = workedExample(); m.convergeAll = true; AnalysisParams p = legacy(); p.detectConvergence = true;   // a release that can kill is NOT insensitive
      auto d = analyse(inputFor(fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000), 1000), m, p);
      CHECK(!d.clicks[0].out.releaseInsensitive); }
    { MockSim m; m.clicks[0] = {{532, 673, 0, 1}}; m.alive = [](int, int, int P, int) { return P == 532; };   // frame-perfect
      GdfwrFile g = fileFromActions({{532, 0, 1}, {673, 0, 0}}, 700);
      auto d = analyse(inputFor(g, 700), m); CHECK(d.clicks[0].flags & ClickFlag::FramePerfect); CHECK_EQ(d.clicks[0].in.window(), 1); CHECK_EQ(d.counters.b1, 1); }
    { MockSim m = workedExample(); auto d = analyse(inputFor(f, 1000), m);                       // drift (4.12)
      CHECK(d.clicks[0].in.driftValid); CHECK(std::abs(d.clicks[0].in.driftDy - 4.0) < 1e-9); CHECK(std::abs(d.clicks[0].in.driftDvy - 0.8) < 1e-9);
      CHECK(d.clicks[0].flags & ClickFlag::Drift); CHECK(!d.clicks[1].in.driftValid); }
    { MockSim m = workedExample(); AnalysisEngine e(inputFor(f, 1000), {}, m);                    // cancel keeps partial results (Part 5, step 4)
      for (int i = 0; i < 5; i++) e.step();
      e.cancel(); CHECK(e.finished()); CHECK(e.result().incomplete); CHECK_EQ(e.result().clicks.size(), size_t(2)); CHECK(!e.step()); }
    { MockSim m; auto d = analyse(AnalysisInput{}, m); CHECK(d.clicks.empty()); }
    { GdfwrFile g = fileFromActions({{100, 0, 1}, {200, 0, 0}, {300, 0, 1}}, 400);               // run ends while holding: output N/A
      MockSim m; m.clicks[0] = {{100, 200, 0, 1}, {300, 1 << 20, 2, -1}};
      auto d = analyse(inputFor(g, 400), m); CHECK(d.clicks[1].out.na); CHECK(d.clicks[1].in.analysed); CHECK(!d.clicks[1].out.analysed); }
    { GdfwrFile g = fileFromActions({{100, 0, 1}, {200, 0, 0}, {300, 0, 1}, {301, 0, 0}}, 400);   // a synthetic LAST release counts as "ends while holding" (A.9)
      g.macro.events[3].flags |= EventFlag::Synthetic;
      MockSim m; auto d = analyse(inputFor(g, 400), m); CHECK(d.clicks[1].out.na); }
}
TEST(two_players_and_checkpoints) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {150, 0, 0}, {152, 1, 1}, {160, 1, 0}}, 300);   // P1 release and P2 press are 2 ticks apart
    MockSim m; m.clicks[0] = {{100, 150, 0, 1}}; m.clicks[1] = {{152, 160, 2, 3}};
    m.alive = [](int p, int, int P, int) { return p == 0 ? P >= 95 : P >= 150; };               // P2 dies when it clicks more than 2 ticks early
    auto d = analyse(inputFor(f, 300), m);
    CHECK_EQ(d.clicks.size(), size_t(2));
    CHECK_EQ(d.clicks[0].in.late, 12); CHECK(d.clicks[0].in.stopLate == StopReason::Cap);        // clamps only look at the same player
    CHECK_EQ(d.clicks[0].in.early, 5); CHECK_EQ(d.clicks[1].player, 1);
    CHECK_EQ(d.clicks[1].in.early, 2); CHECK(d.clicks[1].in.limitingPlayer & 2);
    CHECK(d.clusters.empty());
    auto ticks = requiredCheckpointTicks(makeActions(finalRunEvents(f.macro)));
    CHECK((ticks == std::vector<int>{88, 138, 140, 148}));                                      // edge - 12 for every action edge (4.15)
    CHECK_EQ(requiredCheckpointTicks({{5, 0, true, 0}}).front(), 0);                            // never below tick 0
}

// ---- Part 9: replay -----------------------------------------------------------------------------------
TEST(replay_buckets_and_precision) {
    CHECK(replayBucket(1) == ReplayBucket::W1); CHECK(replayBucket(4) == ReplayBucket::W4); CHECK(replayBucket(5) == ReplayBucket::W5_6);
    CHECK(replayBucket(6) == ReplayBucket::W5_6); CHECK(replayBucket(7) == ReplayBucket::W7_8); CHECK(replayBucket(9) == ReplayBucket::W9_12);
    CHECK(replayBucket(12) == ReplayBucket::W9_12); CHECK(replayBucket(13) == ReplayBucket::W13_24); CHECK(replayBucket(24) == ReplayBucket::W13_24);
    CHECK(replayBucket(25) == ReplayBucket::W25P); CHECK(replayBucket(30) == ReplayBucket::W25P);               // v2: one scheme, 1 2 3 4 5-6 7-8 9-12 13-24 25+
    CHECK_EQ(std::string(replayBucketLabel(ReplayBucket::W25P)), "25+"); CHECK_EQ(kReplayBucketCount, 9);
    CHECK_EQ(std::string(replayBucketLabel(ReplayBucket::W1)), "1"); CHECK_EQ(replayBucketColor(ReplayBucket::W3), 0xFFF27Au);
    CHECK_EQ(replayBucketColor(ReplayBucket::W9_12), 0x7B7BFFu);
    GdfwrFile f = richFile();
    auto plan = buildReplayPlan(f);
    CHECK(plan.ok); CHECK_EQ(plan.rings.size(), size_t(4));
    CHECK(plan.rings[0].known && plan.rings[0].press && plan.rings[0].window == 14 && !plan.rings[0].plus);   // v1 file: whole-click press window = 14
    CHECK(plan.rings[0].bucket == ReplayBucket::W13_24); CHECK(std::abs(plan.rings[0].width - 14.0) < 1e-9);
    CHECK(plan.rings[1].known && !plan.rings[1].press && plan.rings[1].plus);                     // output window with a capped side: "N+"
    CHECK(plan.numbers && plan.note.empty());
    f.analysis->stale = true;                                                                     // v0.1.3: a STALE analysis no longer blocks the replay ...
    { auto q = buildReplayPlan(f); CHECK(q.ok); CHECK(!q.numbers); CHECK(!q.note.empty());        // ... it only removes the numbers
      CHECK_EQ(q.rings.size(), size_t(4)); for (auto& r : q.rings) CHECK(!r.known); }
    { GdfwrFile n = richFile(); n.analysis.reset(); n.header.verify = VerifyStatus::NotVerified;  // a macro with no analysis and no verification
      auto q = buildReplayPlan(n); CHECK(q.ok); CHECK(!q.numbers); CHECK_EQ(q.rings.size(), size_t(4)); CHECK(q.rings[0].press && q.rings[0].tick == 532); }
    { GdfwrFile n = richFile(); n.header.verify = VerifyStatus::Failed; CHECK(buildReplayPlan(n).ok); CHECK(!buildReplayPlan(n).numbers); }
    { GdfwrFile n = richFile(); n.macro.finalRun.exists = false; auto q = buildReplayPlan(n); CHECK(!q.ok); CHECK(!q.reason.empty()); }   // no macro: no replay
    { GdfwrFile n = fileFromActions({}, 100); CHECK(!buildReplayPlan(n).ok); }                                                              // empty macro: no replay
    GdfwrFile g = richFile(); g.analysis->clicks[0].flags |= ClickFlag::Unreliable;
    CHECK(!buildReplayPlan(g).rings[0].known);                                                    // grey ring, not counted
    { GdfwrFile n = richFile(); n.analysis->clicks[0].out.releaseInsensitive = true;              // v0.3.0: measured, but the release never mattered
      auto q = buildReplayPlan(n); CHECK(!q.rings[1].known); CHECK(q.rings[1].insensitive); CHECK(!q.rings[0].insensitive); CHECK(q.rings[0].known); }
    // ring life (v0.3.0): full strength at once (no growing), fully visible for `hold`, then a linear fade over `fade`, then gone
    CHECK(ringOpacity(0.f, 1.f, 2.f) == 1.f); CHECK(ringOpacity(1.f, 1.f, 2.f) == 1.f); CHECK(ringOpacity(-1.f, 1.f, 2.f) == 1.f);
    CHECK(std::abs(ringOpacity(2.f, 1.f, 2.f) - 0.5f) < 1e-6f); CHECK(ringOpacity(3.f, 1.f, 2.f) == 0.f); CHECK(ringOpacity(99.f, 1.f, 2.f) == 0.f);
    CHECK(ringOpacity(1.5f, 1.f, 0.f) == 0.f);
    { bool mono = true; for (float t = 0.f; t < 3.5f; t += 0.01f) mono = mono && ringOpacity(t, 1.f, 2.f) >= ringOpacity(t + 0.01f, 1.f, 2.f); CHECK(mono); }
    CHECK(kRingHoldDefault + kRingFadeDefault >= 5.f * 0.6f);                                      // the old ring was gone after 0.6 s
    std::vector<int> tight(100, 1), wide(100, 12);
    double a = rawPrecision24h(tight, 14400), b = rawPrecision24h(wide, 14400);
    CHECK(a > b); CHECK(b > 0); CHECK(rawPrecision24h(std::vector<int>{}, 100) == 0.0);
    CHECK(rawPrecision24h(std::vector<int>(10, 4), 14400) < rawPrecision24h(std::vector<int>(100, 4), 14400));   // more inputs need more precision
    CHECK_EQ(formatPrecision(114.756), std::string("114.76"));
}


// ---- v0.1.3: auto-repair of a failed verification -----------------------------------------------------
TEST(repair_culprit_and_shifts) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}, {500, 0, 1}, {520, 0, 0}}, 700);
    auto c = culpritClicks(f.macro, 320);                      // died while HOLDING click 2: that click first, then the next one (too late?)
    CHECK_EQ(c.size(), size_t(2)); CHECK_EQ(c[0].index, 1); CHECK_EQ(c[0].pressTick, 300); CHECK(c[0].hasRelease); CHECK_EQ(c[0].releaseTick, 340);
    CHECK_EQ(c[1].index, 2); CHECK_EQ(c[1].pressTick, 500);
    c = culpritClicks(f.macro, 345);                           // died while NOT holding: the click that comes next (too late) first, then the last one
    CHECK_EQ(c.size(), size_t(2)); CHECK_EQ(c[0].index, 2); CHECK_EQ(c[1].index, 1);
    c = culpritClicks(f.macro, 500);                           // an edge at the failure tick was already applied: holding click 3, nothing after it
    CHECK_EQ(c.size(), size_t(1)); CHECK_EQ(c[0].index, 2);
    c = culpritClicks(f.macro, 99);                            // died before the first click: only "that click was too late"
    CHECK_EQ(c.size(), size_t(1)); CHECK_EQ(c[0].index, 0);
    CHECK(culpritClicks(fileFromActions({}, 10).macro, 50).empty());
    GdfwrFile h = fileFromActions({{100, 0, 1}, {150, 0, 0}, {160, 0, 1}}, 300);   // the run ends while holding
    c = culpritClicks(h.macro, 250); CHECK_EQ(c.size(), size_t(1)); CHECK(c[0].index == 1 && !c[0].hasRelease);
    GdfwrFile two = fileFromActions({{100, 0, 1}, {120, 1, 1}, {150, 0, 0}, {160, 1, 0}}, 300);   // players are independent
    c = culpritClicks(two.macro, 155); CHECK(!c.empty() && c[0].player == 0 && c[0].index == 0);   // the last edge <= 155 is player 0's release at 150
    c = culpritClicks(two.macro, 165); CHECK(!c.empty() && c[0].player == 1 && c[0].index == 0);
    CHECK((repairShiftOrder(3) == std::vector<int>{1, -1, 2, -2, 3, -3}));                          // v0.3.0: the smallest correction first, later before earlier on a tie
    CHECK(repairShiftOrder(0).empty());
    CHECK_EQ(repairShiftOrder(10).size(), size_t(20)); CHECK_EQ(repairShiftOrder(10)[1], -1); CHECK_EQ(repairShiftOrder(10)[19], -10);
    CHECK(kRepairMaxClicks >= 100);                                                                  // v0.3.0: a coarse recording may need a fit at every click
    CHECK_EQ(repairSpotMargin(10), 32);
}
TEST(repair_shift_click) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {150, 0, 0}, {300, 0, 1}, {340, 0, 0}}, 700);
    auto c = culpritClicks(f.macro, 320)[0];
    Macro m = f.macro;
    CHECK(shiftClick(m, c, 3).ok);                                                  // later: press and release together
    { auto ev = finalRunEvents(m); CHECK_EQ(ev[2].editedTick, 303); CHECK_EQ(ev[3].editedTick, 343); CHECK(ev[2].flags & EventFlag::Edited);
      CHECK_EQ(ev[2].tick, 300); CHECK_EQ(m.edits.size(), size_t(2)); CHECK(m.macroHash != f.macro.macroHash); }
    Macro e = f.macro;
    CHECK(shiftClick(e, c, -10).ok);                                                // earlier
    { auto ev = finalRunEvents(e); CHECK_EQ(ev[2].editedTick, 290); CHECK_EQ(ev[3].editedTick, 330); }
    Macro n = f.macro;
    CHECK(!shiftClick(n, c, -150).ok);                                              // would cross the previous release (tick 150)
    CHECK(finalRunEvents(n)[2].editedTick == 300 && n.edits.empty());               // all or nothing: nothing was changed
    GdfwrFile g = fileFromActions({{100, 0, 1}, {105, 0, 0}, {107, 0, 1}, {120, 0, 0}}, 300);   // tight neighbours
    Macro t = g.macro; auto c2 = culpritClicks(t, 110)[0];
    CHECK(!shiftClick(t, c2, -2).ok);                                               // release at 105 would touch the press at 105 -> refused
    CHECK(shiftClick(t, c2, -1).ok);                                                // 106 is still one tick after the release at 105
    GdfwrFile z = fileFromActions({{3, 0, 1}, {20, 0, 0}}, 100);
    Macro zm = z.macro; auto c3 = culpritClicks(zm, 5)[0];
    CHECK(!shiftClick(zm, c3, -4).ok); CHECK(shiftClick(zm, c3, -3).ok);            // never below tick 0
    Macro same = f.macro; CHECK(shiftClick(same, c, 0).ok && same.edits.empty());
    GdfwrFile hold = fileFromActions({{100, 0, 1}, {150, 0, 0}, {160, 0, 1}}, 300);   // trailing press: only the press moves
    Macro hm = hold.macro; auto c4 = culpritClicks(hm, 250)[0]; CHECK(shiftClick(hm, c4, 2).ok); CHECK_EQ(finalRunEvents(hm)[2].editedTick, 162);
    // a successful repair makes the file EDITED / NOT_VERIFIED through the normal edit rule
    GdfwrFile r = f; CHECK(shiftClick(r.macro, c, 1).ok); applyEditSideEffects(r);
    CHECK(r.header.edit == EditStatus::Edited); CHECK(r.header.verify == VerifyStatus::NotVerified);
    CHECK(describeShift(c, 3).find("3 ticks later") != std::string::npos); CHECK(describeShift(c, -1).find("1 tick earlier") != std::string::npos);
    // restoring all clicks undoes a repair completely
    GdfwrFile u = f; shiftClick(u.macro, c, 4); restoreAll(u.macro); CHECK_EQ(u.macro.macroHash, f.macro.macroHash);
}

// ---- v2: per-action windows, long search, sub-tick width, file v2 --------------------------------------------
// A continuous mock for the sub-tick mode: the click survives iff its press time (tick + fraction) lies in [lo, hi].
struct ContSim : ISimulator {
    double lo = 0, hi = 0;
    int pressSeq = 0, P = 0;
    bool sub = true;
    int runs = 0;
    SimResult run(const SimRequest& rq) override {
        runs++;
        SimResult r;
        double pt = P;
        for (const auto& in : rq.inputs) if (int(in.order) == pressSeq) pt = double(in.tick) + in.sub;
        if (pt < lo || pt > hi) { r.died = true; r.deathTick = rq.startTick; r.diedMask = 1; }
        return r;
    }
    bool supportsSubTick() const override { return sub; }
};

TEST(v2_press_window_moves_only_the_press) {
    GdfwrFile f = fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000);
    MockSim m = workedExample(), m2 = workedExample();
    auto d = analyse(inputFor(f, 1000), m, AnalysisParams{});            // v2 default
    const auto& c0 = d.clicks[0];
    CHECK_EQ(c0.in.early, 5);
    CHECK_EQ(c0.in.late, 3);                                             // the release stays at 673, so a press at 536 leaves a hold of 137 < 138
    CHECK(c0.in.stopLate == StopReason::Death); CHECK_EQ(c0.in.window(), 9);
    CHECK_EQ(c0.out.early, 3); CHECK_EQ(c0.out.late, 12);                // the release window is the same in both modes
    CHECK(!d.wholeClick);
    auto l = analyse(inputFor(f, 1000), m2, legacy());                   // v1 rule: press and release move together
    CHECK_EQ(l.clicks[0].in.late, 8); CHECK_EQ(l.clicks[0].in.window(), 14); CHECK(l.wholeClick);
}
TEST(v2_press_late_side_clamps_at_its_own_release) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {110, 0, 0}}, 300);
    MockSim m; m.clicks[0] = {{100, 110, 0, 1}};
    auto d = analyse(inputFor(f, 300), m, AnalysisParams{});
    CHECK_EQ(d.clicks[0].in.late, 9); CHECK(d.clicks[0].in.stopLate == StopReason::Clamp);   // P + 10 would touch the release at 110
    CHECK_EQ(d.clicks[0].in.mapLate, std::string("PPPPPPPPP---"));
    CHECK_EQ(d.clicks[0].in.early, 12); CHECK(d.clicks[0].in.stopEarly == StopReason::Cap);
    MockSim m2; m2.clicks[0] = {{100, 110, 0, 1}};
    auto l = analyse(inputFor(f, 300), m2, legacy());                    // v1: the release moves too, nothing in the way
    CHECK_EQ(l.clicks[0].in.late, 12); CHECK(l.clicks[0].in.stopLate == StopReason::Cap);
}
TEST(v2_long_search_uses_coarse_steps_and_bisection) {
    CHECK((requiredCheckpointTicks(makeActions(finalRunEvents(fileFromActions({{100, 0, 1}, {150, 0, 0}}, 400).macro)), 12, 60) == std::vector<int>{40, 88, 90, 138}));
    GdfwrFile f = fileFromActions({{200, 0, 1}, {260, 0, 0}}, 600);
    MockSim m; m.clicks[0] = {{200, 260, 0, 1}};
    m.alive = [](int, int, int P, int) { return P >= 170 && P <= 245; };  // early edge 30, late edge 45
    AnalysisParams p; p.fullScanTop = 0; p.clusters = false; p.reach = 60;
    auto d = analyse(inputFor(f, 600), m, p);
    const auto& w = d.clicks[0].in;
    CHECK_EQ(w.early, 30); CHECK(w.stopEarly == StopReason::Death);
    CHECK_EQ(w.late, 45); CHECK(w.stopLate == StopReason::Death);
    CHECK_EQ(w.window(), 76); CHECK(w.extended); CHECK(!w.lowerBound());
    CHECK_EQ(d.clicks[0].out.early, 59); CHECK(d.clicks[0].out.stopEarly == StopReason::Clamp);   // R - 60 would touch the press: the wall is found by bisection
    CHECK_EQ(d.clicks[0].out.late, 60); CHECK(d.clicks[0].out.stopLate == StopReason::Cap);       // nothing failed up to the limit
    CHECK(m.runs < 130);                                                                          // a 1-tick sweep of all four sides would need about 190
    CHECK_EQ(d.reach, 60);
    MockSim m2; m2.clicks[0] = {{200, 400, 0, 1}}; AnalysisParams q; q.fullScanTop = 0; q.clusters = false; q.reach = 60;
    auto e = analyse(inputFor(fileFromActions({{200, 0, 1}, {400, 0, 0}}, 600), 600), m2, q);
    CHECK_EQ(e.clicks[0].in.early, 60); CHECK_EQ(e.clicks[0].in.late, 60); CHECK(e.clicks[0].in.lowerBound());
    MockSim m3 = m2; m3.runs = 0; AnalysisParams off; off.fullScanTop = 0; off.clusters = false;      // reach == cap: no long search
    auto g = analyse(inputFor(fileFromActions({{200, 0, 1}, {400, 0, 0}}, 600), 600), m3, off);
    CHECK_EQ(g.clicks[0].in.early, 12); CHECK(!g.clicks[0].in.extended);
}
TEST(v2_long_search_matches_a_plain_sweep_when_pass_and_fail_are_monotone) {
    // Random edges 0..80 on both sides; with a limit of 60 the engine must find min(edge, 60) and stop with DEATH below the limit, CAP at it.
    uint32_t seed = 12345;
    auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return int((seed >> 8) % uint32_t(n)); };
    int bad = 0;
    for (int it = 0; it < 300; it++) {
        int e = rnd(81), l = rnd(81), reach = 13 + rnd(60);
        GdfwrFile f = fileFromActions({{300, 0, 1}, {900, 0, 0}}, 1200);
        MockSim m; m.clicks[0] = {{300, 900, 0, 1}};
        m.alive = [e, l](int, int, int P, int) { return P >= 300 - e && P <= 300 + l; };
        AnalysisParams p; p.fullScanTop = 0; p.clusters = false; p.reach = reach;
        auto d = analyse(inputFor(f, 1200), m, p);
        const auto& w = d.clicks[0].in;
        int wantE = std::min(e, reach), wantL = std::min(l, reach);
        bool capE = e >= reach, capL = l >= reach;
        if (w.early != wantE || w.late != wantL) bad++;
        else if ((w.stopEarly == StopReason::Cap) != capE || (w.stopLate == StopReason::Cap) != capL) bad++;
        else if (!capE && w.stopEarly != StopReason::Death) bad++;
        else if (!capL && w.stopLate != StopReason::Death) bad++;
    }
    CHECK_EQ(bad, 0);
}
TEST(v2_sub_tick_width) {
    GdfwrFile f = fileFromActions({{100, 0, 1}, {150, 0, 0}}, 400);
    auto run = [&](bool sub, bool simSupports) {
        ContSim c; c.P = 100; c.lo = 97.3; c.hi = 103.6; c.sub = simSupports;
        AnalysisParams p; p.fullScanTop = 0; p.clusters = false; p.subTick = sub;
        AnalysisEngine e(inputFor(f, 400), p, c); e.runAll();
        return e.takeResult();
    };
    auto a = run(false, true), b = run(true, true), n = run(true, false);
    const auto& wa = a.clicks[0].in; const auto& wb = b.clicks[0].in;
    CHECK_EQ(wa.early, 2); CHECK_EQ(wa.late, 3); CHECK_EQ(wa.window(), 6);
    CHECK(!wa.subTick); CHECK(std::abs(wa.widthLo() - 5.0) < 1e-9); CHECK(std::abs(wa.widthHi() - 7.0) < 1e-9);   // whole ticks: width in [n-1, n+1)
    CHECK(wb.subTick); CHECK_EQ(wb.early, 2); CHECK_EQ(wb.late, 3);                                                 // the whole-tick count does not change
    CHECK(wb.widthLo() <= 6.3 && 6.3 <= wb.widthHi());                                                              // the true width is 6.3 ticks
    CHECK(wb.widthHi() - wb.widthLo() <= 0.125 + 1e-9);                                                             // 4 halvings per side
    CHECK(b.subTickMode); CHECK(!n.subTickMode); CHECK(!n.clicks[0].in.subTick);                                    // a simulator without sub-tick support: whole ticks only
}
TEST(v2_file_roundtrip_and_v1_compat) {
    GdfwrFile f = fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000);
    MockSim m = workedExample(); AnalysisParams p; p.reach = 40;
    f.analysis = analyse(inputFor(f, 1000), m, p); f.analysis->macroHash = f.macro.macroHash;
    auto bytes = serialize(f); auto r = parse(bytes.data(), bytes.size());
    CHECK(r.ok); CHECK(r.file.analysis.has_value()); CHECK(!r.file.analysis->stale);
    const auto& a = *r.file.analysis; const auto& o = *f.analysis;
    CHECK_EQ(a.reach, 40); CHECK(!a.wholeClick); CHECK_EQ(a.clicks.size(), o.clicks.size());
    CHECK(a.clicks[0].in.edgeEarlyLo == o.clicks[0].in.edgeEarlyLo && a.clicks[0].in.edgeLateHi == o.clicks[0].in.edgeLateHi);
    CHECK_EQ(a.clicks[0].in.extended, o.clicks[0].in.extended); CHECK_EQ(a.clicks[0].in.window(), o.clicks[0].in.window());
    // the sub-tick fraction of an event survives a save and changes the macro hash only when it is set
    GdfwrFile s = fileFromActions({{100, 0, 1}, {150, 0, 0}}, 300);
    uint64_t plain = s.macro.macroHash; s.macro.events[0].sub = 0x8000; s.macro.macroHash = computeMacroHash(s.macro);
    CHECK(s.macro.macroHash != plain);
    auto sb = serialize(s); auto sr = parse(sb.data(), sb.size());
    CHECK(sr.ok); CHECK_EQ(sr.file.macro.events[0].sub, uint16_t(0x8000)); CHECK_EQ(sr.file.macro.macroHash, s.macro.macroHash);
    // a file written by v0.1.x (MACR v1, ANAL v1) still loads: whole-click windows, edges derived from the counts
    GdfwrFile g = fileFromActions({{532, 0, 1}, {673, 0, 0}, {783, 0, 1}, {905, 0, 0}}, 1000);
    MockSim m2 = workedExample(); g.analysis = analyse(inputFor(g, 1000), m2, legacy()); g.analysis->macroHash = g.macro.macroHash;
    auto old = serialize(g, 1, 1); auto lr = parse(old.data(), old.size());
    CHECK(lr.ok); CHECK(lr.file.analysis.has_value()); CHECK(!lr.file.analysis->stale);
    const auto& la = *lr.file.analysis;
    CHECK(la.wholeClick); CHECK_EQ(la.reach, la.cap);
    CHECK_EQ(la.clicks[0].in.early, 5); CHECK_EQ(la.clicks[0].in.late, 8);
    CHECK(la.clicks[0].in.edgeEarlyLo == 5.0 && la.clicks[0].in.edgeEarlyHi == 6.0 && la.clicks[0].in.edgeLateHi == 9.0);
    CHECK_EQ(lr.file.macro.events[0].sub, uint16_t(0));
}

// ---- v2: streamed analysis (chunks) -------------------------------------------------------------------------
static bool sameWindow(const WindowResult& a, const WindowResult& b) {
    return a.analysed == b.analysed && a.na == b.na && a.early == b.early && a.late == b.late && a.stopEarly == b.stopEarly &&
           a.stopLate == b.stopLate && a.mapEarly == b.mapEarly && a.mapLate == b.mapLate && a.holes == b.holes && a.traps == b.traps &&
           a.fullScan == b.fullScan && a.releaseInsensitive == b.releaseInsensitive && a.edgeEarlyLo == b.edgeEarlyLo &&
           a.edgeLateHi == b.edgeLateHi && a.extended == b.extended;
}
TEST(v2_plan_chunks) {
    std::vector<std::array<int, 3>> acts;
    for (int i = 0; i < 20; i++) { acts.push_back({100 + i * 100, 0, 1}); acts.push_back({140 + i * 100, 0, 0}); }   // 20 clicks, 60 ticks apart
    auto a = makeActions(finalRunEvents(fileFromActions(acts, 3000).macro));
    CHECK_EQ(listClicks(a).size(), size_t(20));
    auto one = planChunks(a, 12, 12, 1000);                       // plenty of room: one chunk, exactly the states the plan of 4.15 asks for
    CHECK_EQ(one.size(), size_t(1)); CHECK_EQ(one[0].clickFrom, 0); CHECK_EQ(one[0].clickTo, 20);
    CHECK((one[0].ticks == requiredCheckpointTicks(a, 12))); CHECK_EQ(one[0].lastTick, one[0].ticks.back());
    auto many = planChunks(a, 12, 12, 8);                         // 8 states = 4 clicks per chunk (2 edges each)
    CHECK_EQ(many.size(), size_t(5));
    int next = 0; size_t most = 0;
    for (const auto& c : many) { CHECK_EQ(c.clickFrom, next); next = c.clickTo; most = std::max(most, c.ticks.size()); CHECK(c.fullScanTop >= 1); }
    CHECK_EQ(next, 20); CHECK(most <= 8);
    auto far = planChunks(a, 12, 60, 1000);                       // long search: a second, farther state per edge
    CHECK(far[0].ticks.size() > one[0].ticks.size());
    // clicks of one cluster are never separated, even when that goes over the limit: 5 clicks 5 ticks apart hold 10 states (limit 8)
    auto cl = makeActions(finalRunEvents(fileFromActions({{100, 0, 1}, {110, 0, 0}, {115, 0, 1}, {125, 0, 0}, {130, 0, 1}, {140, 0, 0},
                                                           {145, 0, 1}, {155, 0, 0}, {160, 0, 1}, {170, 0, 0}, {600, 0, 1}, {610, 0, 0}, {700, 0, 1}, {710, 0, 0}}, 900).macro));
    auto pc = planChunks(cl, 12, 12, 8);
    CHECK_EQ(pc.size(), size_t(2)); CHECK_EQ(pc[0].clickTo, 5); CHECK_EQ(pc[1].clickFrom, 5); CHECK_EQ(pc[0].ticks.size(), size_t(10));
    CHECK(planChunks({}, 12, 12, 8).empty());
}
TEST(v2_chunked_analysis_equals_one_pass) {
    uint32_t seed = 777;
    auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return int((seed >> 8) % uint32_t(n)); };
    int bad = 0, runsWithClusters = 0;
    for (int it = 0; it < 40; it++) {
        const int n = 12 + rnd(30);
        std::vector<std::array<int, 3>> acts;
        MockSim m;
        std::vector<std::array<int, 4>> win;   // per click: early/late of the press and of the release (the mock's survival window)
        int t = 60;
        for (int i = 0; i < n; i++) {
            int hold = 8 + rnd(40), P = t, R = t + hold;
            acts.push_back({P, 0, 1}); acts.push_back({R, 0, 0});
            m.clicks[0].push_back({P, R, 2 * i, 2 * i + 1});
            win.push_back({rnd(15), rnd(15), rnd(15), rnd(15)});
            t = R + (rnd(3) == 0 ? 3 + rnd(10) : 25 + rnd(80));   // every third gap is short: clusters
        }
        auto origP = m.clicks[0];
        m.alive = [win, origP](int, int c, int P, int R) {
            const auto& w = win[size_t(c)];
            int P0 = origP[size_t(c)][0], R0 = origP[size_t(c)][1];
            return P >= P0 - w[0] && P <= P0 + w[1] && (R > (1 << 27) || (R >= R0 - w[2] && R <= R0 + w[3]));
        };
        GdfwrFile f = fileFromActions(acts, t + 200);
        AnalysisParams base; base.fullScanTop = 1000;
        MockSim m1 = m;
        auto one = analyse(inputFor(f, t + 200), m1, base);
        auto actions = makeActions(finalRunEvents(f.macro));
        const size_t limit = size_t(8 + rnd(40));
        auto chunks = planChunks(actions, base.cap, base.reach, limit, 1000);
        AnalysisData merged;
        for (const auto& c : chunks) {
            MockSim mc = m;
            AnalysisParams p = base; p.clickFrom = c.clickFrom; p.clickTo = c.clickTo; p.fullScanTop = c.fullScanTop;
            AnalysisEngine e(inputFor(f, t + 200), p, mc); e.runAll();
            mergeAnalysis(merged, e.result(), c.clickFrom, c.clickTo);
        }
        if (merged.clicks.size() != one.clicks.size()) { bad++; continue; }
        for (size_t c = 0; c < one.clicks.size(); c++)
            if (!sameWindow(one.clicks[c].in, merged.clicks[c].in) || !sameWindow(one.clicks[c].out, merged.clicks[c].out) || one.clicks[c].flags != merged.clicks[c].flags) bad++;
        if (one.clusters.size() != merged.clusters.size()) bad++;
        else for (size_t c = 0; c < one.clusters.size(); c++) if (!sameWindow(one.clusters[c].win, merged.clusters[c].win)) bad++;
        if (!one.clusters.empty()) runsWithClusters++;
        const auto &a = one.counters, &b = merged.counters;
        if (a.b1 != b.b1 || a.b2 != b.b2 || a.b3 != b.b3 || a.b5_6 != b.b5_6 || a.b7_10 != b.b7_10 || a.b11_24 != b.b11_24 || a.nPlus != b.nPlus || a.clamp != b.clamp) bad++;
        if (one.summary.tightestInput != merged.summary.tightestInput || one.summary.tightestOutput != merged.summary.tightestOutput ||
            one.summary.tightestCluster != merged.summary.tightestCluster || one.summary.avgInput != merged.summary.avgInput ||
            one.summary.reliableClicks != merged.summary.reliableClicks) bad++;
    }
    CHECK_EQ(bad, 0); CHECK(runsWithClusters > 10);   // the random macros really contain clusters
    // an unfinished merge (cancelled between chunks) marks the clicks it never reached as NOT_ANALYSED
    GdfwrFile g = fileFromActions({{100, 0, 1}, {140, 0, 0}, {400, 0, 1}, {440, 0, 0}}, 800);
    MockSim mg; mg.clicks[0] = {{100, 140, 0, 1}, {400, 440, 2, 3}};
    AnalysisParams p; p.clickFrom = 0; p.clickTo = 1;
    AnalysisEngine e(inputFor(g, 800), p, mg); e.runAll();
    AnalysisData part; mergeAnalysis(part, e.result(), 0, 1);
    CHECK(part.clicks[0].in.analysed); CHECK(!part.clicks[1].in.analysed); CHECK(part.clicks[1].flags & ClickFlag::NotAnalysed); CHECK_EQ(part.counters.notAnalysed, 2);
}

TEST(v2_tick_step_test) {
    const double st = 1.0 / 240.0;
    CHECK(tickStepLooksNormal(st, 1.0)); CHECK(tickStepLooksNormal(st + 2e-9, 1.0)); CHECK(tickStepLooksNormal(float(st), 1.0));   // float noise
    CHECK(tickStepLooksNormal(st * 0.5, 0.5)); CHECK(tickStepLooksNormal(st * 2.0, 0.5));                                           // time warp scales the step
    CHECK(tickStepLooksNormal(st * 1.5, 1.5)); CHECK(tickStepLooksNormal(st / 1.5, 1.5));
    CHECK(!tickStepLooksNormal(st * 0.5, 1.0)); CHECK(!tickStepLooksNormal(1.0 / 480.0, 1.0)); CHECK(!tickStepLooksNormal(1.0 / 360.0, 1.0));   // a real 480 / 360 TPS
    CHECK(!tickStepLooksNormal(0.0, 1.0)); CHECK(!tickStepLooksNormal(-st, 1.0)); CHECK(!tickStepLooksNormal(st * 3.0, 1.0));
    CHECK(!tickStepLooksNormal(st * 0.5, 0.0));                                                                                      // an invalid time warp is never an excuse
}

int main() {
    for (auto& [name, fn] : tests()) {
        int before = g_fail;
        fn();
        std::printf("[%s] %s\n", g_fail == before ? " ok " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
