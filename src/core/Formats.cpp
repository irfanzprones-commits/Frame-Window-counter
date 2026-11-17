#include "Formats.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>

#include "Session.hpp"

namespace gdfwr {
namespace {

// ---- LEB128 ---------------------------------------------------------------------------------
struct Cur {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    uint8_t byte() { if (i >= n) { ok = false; return 0; } return p[i++]; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int s = 0; s < 70; s += 7) {
            uint8_t b = byte();
            if (!ok) return 0;
            v |= uint64_t(b & 0x7F) << s;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    std::string cstr() {
        std::string s;
        while (true) {
            uint8_t b = byte();
            if (!ok) return {};
            if (b == 0) return s;
            s.push_back(char(b));
            if (s.size() > (1u << 20)) { ok = false; return {}; }
        }
    }
    void skip(uint64_t k) { if (k > n - i) ok = false; else i += size_t(k); }
};
void putVarint(std::vector<uint8_t>& o, uint64_t v) {
    do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; o.push_back(b); } while (v);
}
void putCstr(std::vector<uint8_t>& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); o.push_back(0); }

// Packed input (GDR2 readme): first byte = [bit0: more bytes follow][bit1: down][bits 2-7: delta, low 6 bits];
// platformer: [bit0][bit1][bits 2-3: button id][bits 4-7: delta low 4 bits]. Remaining bits continue like a normal varint.
void putPacked(std::vector<uint8_t>& o, uint64_t delta, bool down) {
    uint8_t first = uint8_t(((delta & 0x3F) << 2) | (down ? 2 : 0));
    uint64_t rest = delta >> 6;
    if (rest) { o.push_back(first | 1); putVarint(o, rest); }
    else o.push_back(first);
}

// ---- GDR 2 reader --------------------------------------------------------------------------
class Gdr2Reader final : public IFormatReader {
public:
    bool detect(const uint8_t* d, size_t n) const override { return n >= 4 && d[0] == 'G' && d[1] == 'D' && d[2] == 'R'; }
    FormatInfo info() const override {
        return {"GDR2", {".gdr2", ".gdr"}, "Layout from the GDReplayFormat readme (gdr2 branch). Platformer macros are refused.", true, true};
    }
    ParseResult parse(const uint8_t* d, size_t n) const override {
        ParseResult r;
        auto bad = [&](const char* why) {
            r.ok = false; r.errorCode = std::string(code::ImportFileDamaged); r.errorText = std::string("GDR file cannot be parsed: ") + why;
            return r;
        };
        if (!detect(d, n)) return bad("bad magic");
        Cur c{d + 3, n - 3};
        uint64_t version = c.varint();
        std::string tag = c.cstr();
        c.cstr(); c.cstr();                        // author, description
        c.varint();                                // duration (frames)
        uint64_t gameVersion = c.varint();
        uint64_t fps = c.varint();
        uint64_t seed = c.varint();
        c.varint();                                // coins
        c.byte();                                  // ldm
        bool platformer = c.byte() != 0;
        r.macro.botName = c.cstr();
        r.macro.botVersion = int(c.varint());
        uint64_t levelId = c.varint();
        r.macro.levelName = c.cstr();
        c.skip(c.varint());                        // extension block
        if (!c.ok) return bad("metadata");
        uint64_t deaths = c.varint();
        for (uint64_t i = 0; i < deaths && c.ok; i++) c.varint();
        uint64_t total = c.varint(), p1 = c.varint();
        if (!c.ok || p1 > total || total > (n - c.i) + 0) return bad("input counts");
        r.macro.formatName = "GDR2";
        r.macro.formatVersion = std::to_string(version);
        r.macro.fps = double(fps);
        r.macro.gameVersion = int(gameVersion);
        r.macro.platformer = platformer;
        r.macro.seed = int64_t(seed);
        if (levelId) r.macro.levelId = int64_t(levelId);
        r.macro.inputs.reserve(size_t(total));
        uint64_t frame = 0;
        for (uint64_t i = 0; i < total; i++) {
            if (i == p1) frame = 0;  // P2 stream starts again from frame 0 (delta encoded per stream)
            uint8_t first = c.byte();
            if (!c.ok) return bad("inputs");
            bool down = first & 2;
            uint8_t button = kJump;
            uint64_t delta;
            if (platformer) { button = (first >> 2) & 3; delta = first >> 4; }
            else delta = first >> 2;
            if (first & 1) {
                uint64_t rest = c.varint();
                if (!c.ok) return bad("inputs");
                delta |= rest << (platformer ? 4 : 6);
            }
            if (!tag.empty()) c.skip(c.varint());  // per-input extension of a custom input type
            if (!c.ok) return bad("input extension");
            frame += delta;
            r.macro.inputs.push_back({frame, button, i >= p1, down});
        }
        r.ok = true;
        return r;
    }
};

// Formats whose layout is NOT verified yet. The spec says: do not guess (Part 6, 6.2).
class XdBotReader final : public IFormatReader {
public:
    bool detect(const uint8_t*, size_t) const override { return false; }
    ParseResult parse(const uint8_t*, size_t) const override {
        ParseResult r;
        r.errorCode = std::string(code::ImportUnknownFormat);
        r.errorText = "xdBot (.xd) import is not enabled: its layout is not verified yet (spec 6.2 [VERIFY]).";
        return r;
    }
    FormatInfo info() const override { return {"xdBot", {".xd"}, "Layout not verified - reader is a stub.", false, false}; }
};

}  // namespace

const std::vector<const IFormatReader*>& allReaders() {
    static const Gdr2Reader gdr2;
    static const XdBotReader xd;
    static const std::vector<const IFormatReader*> v{&gdr2, &xd};
    return v;
}

const IFormatReader* detectReader(const uint8_t* d, size_t n, const std::string& ext) {
    for (const auto* r : allReaders()) if (r->info().enabled && r->detect(d, n)) return r;
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    for (const auto* r : allReaders()) {
        auto inf = r->info();
        if (!inf.enabled) continue;
        for (const auto& x : inf.extensions) if (x == e) return r;  // extension match: parse() will still validate the content
    }
    return nullptr;
}

// ---- import --------------------------------------------------------------------------------
ImportResult importMacro(const SourceMacro& s, const ImportOptions& o) {
    ImportResult res;
    auto fail = [&](std::string_view c, std::string t) { res.ok = false; res.errorCode = std::string(c); res.errorText = std::move(t); return res; };
    if (!o.haveLoadedLevel) return fail("IMPORT_NO_LEVEL", "Open the level first: Import needs the level to attach the macro to.");

    // 8.6.4 frame -> tick
    double kf = s.fps > 0 ? 240.0 / s.fps : 0;
    int K = int(std::lround(kf));
    if (s.fps <= 0 || std::fabs(kf - K) > 1e-9 || K < 1)
        return fail(code::ImportFpsNotDivisor, "240 / " + std::to_string(int(s.fps)) + " is not a whole number. The import is refused, not rounded.");
    if (s.platformer || std::any_of(s.inputs.begin(), s.inputs.end(), [](const SourceInput& i) { return i.button != kJump; }))
        return fail(code::ImportPlatformer, "The macro uses platformer buttons.");
    if (s.startsFromCheckpoint) return fail(code::ImportNotFromStart, "The macro does not start at tick 0 of the level.");
    if (s.inputs.empty()) return fail(code::ImportFileDamaged, "The macro has no inputs.");

    struct E { int64_t tick; int player; bool down; size_t order; };
    std::vector<E> ev;
    for (size_t i = 0; i < s.inputs.size(); i++) {
        int64_t tick = (int64_t(s.inputs[i].frame) + o.frameOffset) * K;
        if (tick < 0 || tick > INT32_MAX) return fail(code::ImportBadSequence, "A tick is below 0 or too large.");
        ev.push_back({tick, s.inputs[i].player2 ? 1 : 0, s.inputs[i].down, i});
    }
    std::stable_sort(ev.begin(), ev.end(), [](const E& a, const E& b) { return a.tick < b.tick; });

    // 8.6.6 player / level questions
    bool hasP2 = std::any_of(ev.begin(), ev.end(), [](const E& e) { return e.player == 1; });
    if (hasP2 && !o.levelTwoPlayer) return fail(code::ImportPlayerMismatch, "The macro has player 2 inputs but the level is not 2-player.");
    bool levelDiffers = (s.levelId && *s.levelId != o.levelId);
    if (levelDiffers && !o.acceptLevelMismatch) { res.askLevelMismatch = true; return fail(code::LevelMismatch, "The macro was recorded on a different level. Continue anyway?"); }

    // 8.6.5 cleaning: alternate press/release per player, nothing is changed silently
    GdfwrFile f;
    auto warn = [&](std::string_view c, std::string d) { f.header.warnings.push_back({std::string(c), std::move(d)}); };
    if (levelDiffers) warn(code::LevelMismatch, "source level " + std::to_string(*s.levelId) + " differs from the loaded level");
    if (!s.levelId) warn(code::ImportLevelUnchecked, "the level was chosen by the user only");
    std::vector<E> clean;
    std::array<int, 2> held{0, 0};
    std::array<bool, 2> seen{false, false};
    for (const auto& e : ev) {
        bool had = seen[size_t(e.player)];
        seen[size_t(e.player)] = true;
        if (e.down == bool(held[size_t(e.player)])) {
            if (!e.down && !had) warn(code::ImportOrphanRelease, "first event is a release at tick " + std::to_string(e.tick));
            else warn(code::ImportDuplicateDropped, std::string(e.down ? "press" : "release") + " dropped at tick " + std::to_string(e.tick));
            continue;
        }
        held[size_t(e.player)] = e.down;
        clean.push_back(e);
    }
    int64_t lastTick = clean.empty() ? 0 : clean.back().tick;
    for (int p = 0; p < 2; p++) {
        if (held[size_t(p)]) {  // last event is a press: add a synthetic release after the last tick of the run
            clean.push_back({lastTick + 1, p, false, SIZE_MAX});
            warn(code::ImportSyntheticRelease, "synthetic release added for player " + std::to_string(p + 1));
        }
    }
    if (clean.empty()) return fail(code::ImportBadSequence, "No usable inputs after cleaning.");

    // 8.6.8 the new file
    f.header.modVersion = o.modVersion;
    f.header.gameVersion = o.gameVersion;
    f.header.createdUtc = o.nowUtc;
    f.header.rec = RecStatus::Partial;  // D2: COMPLETED only after verification reaches the end of the level
    f.header.verify = VerifyStatus::NotVerified;
    f.header.edit = EditStatus::Original;
    f.header.origin = Origin::Imported;
    f.header.importFormat = s.formatName;
    f.header.importVersion = s.formatVersion;
    f.header.importFile = o.sourceFileName;
    f.level.levelId = o.levelId;
    f.level.name = o.levelName;
    f.level.version = o.levelVersion;
    f.level.dataHash = o.levelHash;
    f.level.gameVersion = o.gameVersion;
    f.level.twoPlayer = o.levelTwoPlayer;
    f.level.dual = o.levelDual;
    f.level.hasSeed = s.seed.has_value();
    f.level.seed = s.seed.value_or(0);
    f.level.endTick = int32_t(clean.back().tick);
    f.timing.tps = kTps;
    f.timing.clickResolution = K;
    if (K > 1) warn(code::CoarseInput, "imported at " + std::to_string(int(s.fps)) + " FPS: ticks are exact to " + std::to_string(K) + " ticks");

    Segment seg;
    seg.id = 0; seg.startTick = 0; seg.endTick = int32_t(clean.back().tick); seg.endReason = EndReason::Stopped;
    f.macro.segments.push_back(seg);
    std::array<int32_t, 2> open{-1, -1};
    for (const auto& e : clean) {
        Event ev2;
        ev2.seq = uint32_t(f.macro.events.size());
        ev2.segment = 0;
        ev2.tick = ev2.editedTick = int32_t(e.tick);
        ev2.player = uint8_t(e.player);
        ev2.type = e.down ? EventType::Press : EventType::Release;
        ev2.source = "IMPORT:" + s.formatName;
        if (e.order == SIZE_MAX) { ev2.flags |= EventFlag::Synthetic; ev2.source = "synthetic"; }
        else if (K > 1) ev2.flags |= EventFlag::UncertainTick;
        if (e.down) open[size_t(e.player)] = int32_t(ev2.seq);
        else if (open[size_t(e.player)] >= 0) {
            ev2.partner = open[size_t(e.player)];
            f.macro.events[size_t(ev2.partner)].partner = int32_t(ev2.seq);
            open[size_t(e.player)] = -1;
        }
        f.macro.events.push_back(std::move(ev2));
    }
    f.macro.finalRun.exists = true;
    f.macro.finalRun.spans.push_back({0, 0, seg.endTick});
    f.macro.macroHash = computeMacroHash(f.macro);
    res.file = std::move(f);
    res.ok = true;
    return res;
}

// ---- export --------------------------------------------------------------------------------
std::vector<uint8_t> exportGdr2(const GdfwrFile& f, int offset) {
    auto ev = finalRunEvents(f.macro);
    std::array<std::vector<std::pair<uint64_t, bool>>, 2> streams;  // (frame, down)
    for (const auto& e : ev) {
        int64_t frame = int64_t(e.editedTick) - offset;  // 8.7.2: frame = tick - offset
        if (frame < 0) frame = 0;
        streams[e.player & 1].push_back({uint64_t(frame), e.type == EventType::Press});
    }
    std::vector<uint8_t> o{'G', 'D', 'R'};
    putVarint(o, kGdr2WriterVersion);
    putCstr(o, "");                          // input tag: default input type
    putCstr(o, "");                          // author
    putCstr(o, "exported by GDFWR");         // description
    putVarint(o, uint64_t(std::max(0, f.level.endTick)));
    putVarint(o, 2081);                      // game version (2.2081)
    putVarint(o, uint64_t(kTps));
    putVarint(o, f.level.hasSeed ? uint64_t(f.level.seed) : 0);
    putVarint(o, 0);                         // coins
    o.push_back(0);                          // ldm
    o.push_back(0);                          // platformer
    putCstr(o, "GDFWR");
    putVarint(o, 1);
    putVarint(o, uint64_t(f.level.levelId));
    putCstr(o, f.level.name);
    putVarint(o, 0);                         // no extension data
    putVarint(o, 0);                         // death frames
    putVarint(o, streams[0].size() + streams[1].size());
    putVarint(o, streams[0].size());
    for (auto& s : streams) {
        uint64_t prev = 0;
        for (auto [frame, down] : s) { putPacked(o, frame - prev, down); prev = frame; }
    }
    return o;
}

std::vector<std::string> exportLossList(const GdfwrFile& f) {
    std::vector<std::string> l;
    size_t dead = 0;
    auto fin = finalRunEvents(f.macro);
    if (f.macro.events.size() > fin.size()) dead = f.macro.events.size() - fin.size();
    if (f.macro.segments.size() > f.macro.finalRun.spans.size() || dead)
        l.push_back("Segments that are not in the final run and all dead branches (" + std::to_string(dead) + " events)");
    l.push_back("Bookmarks and markers");
    l.push_back("Baseline and Analysis results");
    l.push_back("Edit history (only the edited ticks are written)");
    l.push_back("Flags: SYNTHETIC, EDITED, UNCERTAIN_TICK");
    l.push_back("Random-branch log");
    return l;
}

std::vector<Warning> exportWarnings(const GdfwrFile& f, bool levelMismatch) {
    std::vector<Warning> w;
    if (f.header.verify != VerifyStatus::Verified || f.header.rec == RecStatus::Partial)
        w.push_back({"EXPORT_NOT_VERIFIED", "the file is not VERIFIED or is PARTIAL"});
    bool unc = std::any_of(f.macro.events.begin(), f.macro.events.end(), [](const Event& e) { return e.flags & EventFlag::UncertainTick; });
    if (unc) w.push_back({std::string(code::ExportUncertain), "ticks are only exact to " + std::to_string(f.timing.clickResolution) + " ticks"});
    if (levelMismatch) w.push_back({std::string(code::ExportLevel), "the file belongs to a different level"});
    return w;
}

std::string exportCheck(const GdfwrFile& f, const std::vector<uint8_t>& written) {
    const IFormatReader* r = detectReader(written.data(), written.size(), ".gdr2");
    if (!r) return std::string(code::ExportCheckFailed);
    auto p = r->parse(written.data(), written.size());
    if (!p.ok || p.macro.inputs.size() != finalRunEvents(f.macro).size()) return std::string(code::ExportCheckFailed);
    return {};
}

}  // namespace gdfwr
