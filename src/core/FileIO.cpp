#include "FileIO.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#include "Session.hpp"
#include "Util.hpp"

namespace gdfwr {
namespace {

constexpr uint8_t kMagic[8] = {'G', 'D', 'F', 'W', 'R', 0x1A, 0x0A, 0x00};
constexpr uint16_t kVerLevel = 1, kVerTime = 1, kVerBase = 1;

// ---- small writers ---------------------------------------------------------------
void putWarnings(ByteWriter& w, const std::vector<Warning>& v) {
    w.u32(uint32_t(v.size()));
    for (const auto& x : v) { w.str(x.code); w.str(x.detail); }
}
void getWarnings(ByteReader& r, std::vector<Warning>& v) {
    uint32_t n = r.count(8);
    for (uint32_t i = 0; i < n && r.ok(); i++) { Warning x; x.code = r.str(); x.detail = r.str(); v.push_back(std::move(x)); }
}
void putRandom(ByteWriter& w, const std::vector<RandomEntry>& v) {
    w.u32(uint32_t(v.size()));
    for (const auto& x : v) { w.i32(x.tick); w.i32(x.trigger); w.i32(x.branch); w.u32(x.segment); }
}
void getRandom(ByteReader& r, std::vector<RandomEntry>& v) {
    uint32_t n = r.count(16);
    for (uint32_t i = 0; i < n && r.ok(); i++) { RandomEntry x; x.tick = r.i32(); x.trigger = r.i32(); x.branch = r.i32(); x.segment = r.u32(); v.push_back(x); }
}
void putWindow(ByteWriter& w, const WindowResult& x, uint16_t ver) {
    w.u8(uint8_t(x.analysed | (x.na << 1) | (x.fullScan << 2) | (x.releaseInsensitive << 3) | (x.driftValid << 4)));
    w.i32(x.early); w.i32(x.late); w.u8(uint8_t(x.stopEarly)); w.u8(uint8_t(x.stopLate));
    w.str(x.mapEarly); w.str(x.mapLate); w.i32(x.holes); w.i32(x.traps);
    w.f64(x.driftDy); w.f64(x.driftDvy); w.u8(x.limitingPlayer);
    if (ver >= 2) {
        w.f64(x.edgeEarlyLo); w.f64(x.edgeEarlyHi); w.f64(x.edgeLateLo); w.f64(x.edgeLateHi);
        w.u8(uint8_t(x.subTick | (x.extended << 1)));
    }
}
void getWindow(ByteReader& r, WindowResult& x, uint16_t ver) {
    uint8_t b = r.u8();
    x.analysed = b & 1; x.na = b & 2; x.fullScan = b & 4; x.releaseInsensitive = b & 8; x.driftValid = b & 16;
    x.early = r.i32(); x.late = r.i32(); x.stopEarly = StopReason(r.u8()); x.stopLate = StopReason(r.u8());
    x.mapEarly = r.str(); x.mapLate = r.str(); x.holes = r.i32(); x.traps = r.i32();
    x.driftDy = r.f64(); x.driftDvy = r.f64(); x.limitingPlayer = r.u8();
    if (ver >= 2) {
        x.edgeEarlyLo = r.f64(); x.edgeEarlyHi = r.f64(); x.edgeLateLo = r.f64(); x.edgeLateHi = r.f64();
        uint8_t f = r.u8();
        x.subTick = f & 1; x.extended = f & 2;
    } else {  // a v1 file only knows whole ticks: the edge lies in [n, n+1) on each side
        x.edgeEarlyLo = x.early; x.edgeEarlyHi = x.early + 1; x.edgeLateLo = x.late; x.edgeLateHi = x.late + 1;
    }
}

// ---- section payloads --------------------------------------------------------------
std::vector<uint8_t> headerPayload(const Header& h) {
    ByteWriter w;
    w.str(h.modVersion); w.str(h.gameVersion); w.i64(h.createdUtc);
    w.u8(uint8_t(h.rec)); w.u8(uint8_t(h.verify)); w.i32(h.verifyFailTick); w.u8(uint8_t(h.edit)); w.u8(uint8_t(h.origin));
    w.str(h.importFormat); w.str(h.importVersion); w.str(h.importFile);
    putWarnings(w, h.warnings);
    return w.buf;
}
std::vector<uint8_t> levelPayload(const LevelInfo& l) {
    ByteWriter w;
    w.i64(l.levelId); w.str(l.name); w.i32(l.version); w.u64(l.dataHash); w.str(l.gameVersion);
    w.u8(uint8_t(l.platformer | (l.twoPlayer << 1) | (l.dual << 2) | (l.hasSeed << 3)));
    w.i32(l.endTick); w.i64(l.seed);
    return w.buf;
}
std::vector<uint8_t> timingPayload(const TimingInfo& t) {
    ByteWriter w;
    w.u16(t.tps); w.i32(t.clickResolution); w.u8(t.cbf);
    w.u32(uint32_t(t.fps.size()));
    for (const auto& f : t.fps) { w.u32(f.segment); w.f32(f.avg); w.f32(f.min); }
    w.u32(uint32_t(t.tpsChanges.size()));
    for (const auto& c : t.tpsChanges) { w.u32(c.segment); w.i32(c.tick); w.f32(c.from); w.f32(c.to); }
    return w.buf;
}
std::vector<uint8_t> macroPayload(const Macro& m, uint16_t ver) {
    ByteWriter w;
    w.u32(uint32_t(m.segments.size()));
    for (const auto& s : m.segments) {
        w.u32(s.id); w.i32(s.parent); w.i32(s.parentBookmark); w.i32(s.startTick); w.i32(s.endTick);
        w.u8(uint8_t(s.startKind)); w.u8(uint8_t(s.endReason)); w.u8(s.startHold[0]); w.u8(s.startHold[1]);
        w.u16(s.tps); w.u8(s.flags); w.f32(s.fpsAvg); w.f32(s.fpsMin);
    }
    w.u32(uint32_t(m.bookmarks.size()));
    for (const auto& b : m.bookmarks) {
        w.u32(b.id); w.u64(b.gameId); w.u32(b.segment); w.i32(b.tick); w.u32(b.seq);
        w.u8(b.hold[0]); w.u8(b.hold[1]); w.f32(b.x); w.f32(b.y); w.u8(b.active);
    }
    w.u32(uint32_t(m.markers.size()));
    for (const auto& k : m.markers) { w.u8(uint8_t(k.kind)); w.u32(k.segment); w.i32(k.tick); w.u32(k.seq); }
    w.u32(uint32_t(m.events.size()));
    for (const auto& e : m.events) {
        w.u32(e.seq); w.u32(e.segment); w.i32(e.tick); w.i32(e.editedTick); w.u8(e.player); w.u8(uint8_t(e.type));
        w.u8(e.button); w.str(e.source); w.u8(e.flags); w.i32(e.partner);
        if (ver >= 2) w.u16(e.sub);
    }
    putRandom(w, m.randomLog);
    w.u8(m.finalRun.exists); w.u8(m.finalRun.partial);
    w.u32(uint32_t(m.finalRun.spans.size()));
    for (const auto& s : m.finalRun.spans) { w.u32(s.segment); w.i32(s.fromTick); w.i32(s.toTick); }
    w.u32(uint32_t(m.finalRun.syntheticSeqs.size()));
    for (auto q : m.finalRun.syntheticSeqs) w.u32(q);
    w.u32(uint32_t(m.edits.size()));
    for (const auto& e : m.edits) { w.u32(e.seq); w.i32(e.originalTick); w.i32(e.newTick); }
    w.u64(m.macroHash);
    return w.buf;
}
std::vector<uint8_t> basePayload(const Baseline& b) {
    ByteWriter w;
    w.u64(b.macroHash); w.f64(b.epsilon); w.u8(b.stale);
    w.u32(uint32_t(b.hashes.size()));
    for (auto h : b.hashes) w.u64(h);
    w.u32(uint32_t(b.samples.size()));
    for (const auto& s : b.samples) { w.f32(s.x1); w.f32(s.y1); w.f32(s.vy1); w.f32(s.x2); w.f32(s.y2); w.f32(s.vy2); }
    putRandom(w, b.randomLog);
    return w.buf;
}
std::vector<uint8_t> analPayload(const AnalysisData& a, uint16_t ver) {
    ByteWriter w;
    w.str(a.specVersion); w.str(a.modVersion); w.i32(a.cap); w.i32(a.fullScanTop); w.f64(a.epsilon); w.i32(a.tps);
    if (ver >= 2) { w.i32(a.reach); w.u8(uint8_t(a.wholeClick | (a.subTickMode << 1))); }
    w.u64(a.macroHash); w.u8(uint8_t(a.stale | (a.incomplete << 1)));
    w.u32(uint32_t(a.clicks.size()));
    for (const auto& c : a.clicks) {
        w.u32(c.index); w.u8(c.player); w.i32(c.press); w.i32(c.release); w.u32(c.flags);
        putWindow(w, c.in, ver); putWindow(w, c.out, ver);
    }
    w.u32(uint32_t(a.clusters.size()));
    for (const auto& c : a.clusters) {
        w.u8(c.player); w.i32(c.firstAction); w.i32(c.lastAction); w.u32(c.flags);
        w.u32(uint32_t(c.clicks.size()));
        for (auto k : c.clicks) w.u32(k);
        putWindow(w, c.win, ver);
    }
    const auto& k = a.counters;
    for (int v : {k.b1, k.b2, k.b3, k.b4, k.b5_6, k.b7_10, k.b11_24, k.b25p, k.nPlus, k.clamp, k.unreliable, k.random, k.notAnalysed}) w.i32(v);
    const auto& s = a.summary;
    for (const auto* v : {&s.tightestInput, &s.tightestOutput, &s.tightestCluster}) {
        w.u32(uint32_t(v->size()));
        for (auto q : *v) w.u32(q);
    }
    w.f64(s.avgInput); w.f64(s.avgOutput); w.u8(uint8_t(s.avgInputLower | (s.avgOutputLower << 1))); w.i32(s.reliableClicks);
    putWarnings(w, a.warnings);
    return w.buf;
}

void addSection(ByteWriter& out, const char id[4], uint16_t ver, const std::vector<uint8_t>& payload) {
    out.bytes(reinterpret_cast<const uint8_t*>(id), 4);
    out.u16(ver);
    out.u32(uint32_t(payload.size()));
    out.bytes(payload.data(), payload.size());
    out.u32(crc32(payload.data(), payload.size()));
}

// ---- section readers (return false when the payload is malformed) -----------------------
bool readHeader(ByteReader& r, Header& h) {
    h.modVersion = r.str(); h.gameVersion = r.str(); h.createdUtc = r.i64();
    h.rec = RecStatus(r.u8()); h.verify = VerifyStatus(r.u8()); h.verifyFailTick = r.i32();
    h.edit = EditStatus(r.u8()); h.origin = Origin(r.u8());
    h.importFormat = r.str(); h.importVersion = r.str(); h.importFile = r.str();
    getWarnings(r, h.warnings);
    return r.ok();
}
bool readLevel(ByteReader& r, LevelInfo& l) {
    l.levelId = r.i64(); l.name = r.str(); l.version = r.i32(); l.dataHash = r.u64(); l.gameVersion = r.str();
    uint8_t f = r.u8();
    l.platformer = f & 1; l.twoPlayer = f & 2; l.dual = f & 4; l.hasSeed = f & 8;
    l.endTick = r.i32(); l.seed = r.i64();
    return r.ok();
}
bool readTiming(ByteReader& r, TimingInfo& t) {
    t.tps = r.u16(); t.clickResolution = r.i32(); t.cbf = r.u8();
    uint32_t n = r.count(12);
    for (uint32_t i = 0; i < n && r.ok(); i++) { FpsStat f; f.segment = r.u32(); f.avg = r.f32(); f.min = r.f32(); t.fps.push_back(f); }
    n = r.count(16);
    for (uint32_t i = 0; i < n && r.ok(); i++) { TpsChange c; c.segment = r.u32(); c.tick = r.i32(); c.from = r.f32(); c.to = r.f32(); t.tpsChanges.push_back(c); }
    return r.ok();
}
bool readMacro(ByteReader& r, Macro& m, uint16_t ver) {
    uint32_t n = r.count(36);
    for (uint32_t i = 0; i < n && r.ok(); i++) {
        Segment s;
        s.id = r.u32(); s.parent = r.i32(); s.parentBookmark = r.i32(); s.startTick = r.i32(); s.endTick = r.i32();
        s.startKind = StartKind(r.u8()); s.endReason = EndReason(r.u8()); s.startHold[0] = r.u8(); s.startHold[1] = r.u8();
        s.tps = r.u16(); s.flags = r.u8(); s.fpsAvg = r.f32(); s.fpsMin = r.f32();
        m.segments.push_back(s);
    }
    n = r.count(36);
    for (uint32_t i = 0; i < n && r.ok(); i++) {
        Bookmark b;
        b.id = r.u32(); b.gameId = r.u64(); b.segment = r.u32(); b.tick = r.i32(); b.seq = r.u32();
        b.hold[0] = r.u8(); b.hold[1] = r.u8(); b.x = r.f32(); b.y = r.f32(); b.active = r.u8();
        m.bookmarks.push_back(b);
    }
    n = r.count(13);
    for (uint32_t i = 0; i < n && r.ok(); i++) { Marker k; k.kind = MarkerKind(r.u8()); k.segment = r.u32(); k.tick = r.i32(); k.seq = r.u32(); m.markers.push_back(k); }
    n = r.count(26);
    for (uint32_t i = 0; i < n && r.ok(); i++) {
        Event e;
        e.seq = r.u32(); e.segment = r.u32(); e.tick = r.i32(); e.editedTick = r.i32(); e.player = r.u8();
        e.type = EventType(r.u8()); e.button = r.u8(); e.source = r.str(); e.flags = r.u8(); e.partner = r.i32();
        if (ver >= 2) e.sub = r.u16();
        m.events.push_back(std::move(e));
    }
    getRandom(r, m.randomLog);
    m.finalRun.exists = r.u8(); m.finalRun.partial = r.u8();
    n = r.count(12);
    for (uint32_t i = 0; i < n && r.ok(); i++) { FinalSpan s; s.segment = r.u32(); s.fromTick = r.i32(); s.toTick = r.i32(); m.finalRun.spans.push_back(s); }
    n = r.count(4);
    for (uint32_t i = 0; i < n && r.ok(); i++) m.finalRun.syntheticSeqs.push_back(r.u32());
    n = r.count(12);
    for (uint32_t i = 0; i < n && r.ok(); i++) { EditRecord e; e.seq = r.u32(); e.originalTick = r.i32(); e.newTick = r.i32(); m.edits.push_back(e); }
    m.macroHash = r.u64();
    if (!r.ok()) return false;
    // consistency: ids/seqs are indices, references are in range (a damaged-but-CRC-valid file must not crash later code)
    for (size_t i = 0; i < m.segments.size(); i++) if (m.segments[i].id != i) return false;
    for (size_t i = 0; i < m.bookmarks.size(); i++) if (m.bookmarks[i].id != i) return false;
    for (const auto& s : m.segments) {
        if (s.parent >= int(m.segments.size()) || s.parentBookmark >= int(m.bookmarks.size())) return false;
    }
    for (const auto& e : m.events) if (e.segment >= m.segments.size() || e.player > 1) return false;
    for (const auto& s : m.finalRun.spans) if (s.segment >= m.segments.size()) return false;
    return true;
}
bool readBase(ByteReader& r, Baseline& b) {
    b.macroHash = r.u64(); b.epsilon = r.f64(); b.stale = r.u8();
    uint32_t n = r.count(8);
    for (uint32_t i = 0; i < n && r.ok(); i++) b.hashes.push_back(r.u64());
    n = r.count(24);
    for (uint32_t i = 0; i < n && r.ok(); i++) { Sample s; s.x1 = r.f32(); s.y1 = r.f32(); s.vy1 = r.f32(); s.x2 = r.f32(); s.y2 = r.f32(); s.vy2 = r.f32(); b.samples.push_back(s); }
    getRandom(r, b.randomLog);
    return r.ok();
}
bool readAnal(ByteReader& r, AnalysisData& a, uint16_t ver) {
    a.specVersion = r.str(); a.modVersion = r.str(); a.cap = r.i32(); a.fullScanTop = r.i32(); a.epsilon = r.f64(); a.tps = r.i32();
    if (ver >= 2) { a.reach = r.i32(); uint8_t m = r.u8(); a.wholeClick = m & 1; a.subTickMode = m & 2; }
    else { a.reach = a.cap; a.wholeClick = true; a.subTickMode = false; }   // v1 windows moved press and release together
    a.macroHash = r.u64();
    uint8_t f = r.u8();
    a.stale = f & 1; a.incomplete = f & 2;
    uint32_t n = r.count(40);
    for (uint32_t i = 0; i < n && r.ok(); i++) {
        ClickResult c;
        c.index = r.u32(); c.player = r.u8(); c.press = r.i32(); c.release = r.i32(); c.flags = r.u32();
        getWindow(r, c.in, ver); getWindow(r, c.out, ver);
        a.clicks.push_back(std::move(c));
    }
    n = r.count(20);
    for (uint32_t i = 0; i < n && r.ok(); i++) {
        ClusterResult c;
        c.player = r.u8(); c.firstAction = r.i32(); c.lastAction = r.i32(); c.flags = r.u32();
        uint32_t k = r.count(4);
        for (uint32_t j = 0; j < k && r.ok(); j++) c.clicks.push_back(r.u32());
        getWindow(r, c.win, ver);
        a.clusters.push_back(std::move(c));
    }
    auto& k = a.counters;
    for (int* v : {&k.b1, &k.b2, &k.b3, &k.b4, &k.b5_6, &k.b7_10, &k.b11_24, &k.b25p, &k.nPlus, &k.clamp, &k.unreliable, &k.random, &k.notAnalysed}) *v = r.i32();
    auto& s = a.summary;
    for (auto* v : {&s.tightestInput, &s.tightestOutput, &s.tightestCluster}) {
        uint32_t c = r.count(4);
        for (uint32_t j = 0; j < c && r.ok(); j++) v->push_back(r.u32());
    }
    s.avgInput = r.f64(); s.avgOutput = r.f64();
    uint8_t lb = r.u8();
    s.avgInputLower = lb & 1; s.avgOutputLower = lb & 2; s.reliableClicks = r.i32();
    getWarnings(r, a.warnings);
    return r.ok();
}

}  // namespace

std::vector<uint8_t> serialize(const GdfwrFile& f, uint16_t macroVersion, uint16_t analysisVersion) {
    ByteWriter out;
    out.bytes(kMagic, 8);
    out.u16(f.header.formatVersion);
    out.u16(f.header.minReader);
    auto hp = headerPayload(f.header);
    out.u32(uint32_t(hp.size()));
    out.bytes(hp.data(), hp.size());
    out.u32(crc32(hp.data(), hp.size()));
    addSection(out, "LEVL", kVerLevel, levelPayload(f.level));
    addSection(out, "TIME", kVerTime, timingPayload(f.timing));
    addSection(out, "MACR", macroVersion, macroPayload(f.macro, macroVersion));
    // STALE derived sections are saved WITH their stale mark, never as CURRENT (Part 8, 8.4.1)
    if (f.baseline) addSection(out, "BASE", kVerBase, basePayload(*f.baseline));
    if (f.analysis) addSection(out, "ANAL", analysisVersion, analPayload(*f.analysis, analysisVersion));
    return std::move(out.buf);
}

LoadResult parse(const uint8_t* data, size_t n) {
    LoadResult res;
    auto damaged = [&](std::string why) {
        res.ok = false;
        res.errorCode = std::string(code::LoadFileDamaged);
        res.errorText = "File is damaged (" + why + ").";
        return res;
    };
    if (n < 16 || !std::equal(kMagic, kMagic + 8, data)) return damaged("not a .gdfwr file");
    ByteReader r(data + 8, n - 8);
    uint16_t fmt = r.u16(), minReader = r.u16();
    if (minReader > kFormatVersion) {
        res.errorCode = std::string(code::LoadNeedsNewerMod);
        res.errorText = "This file needs a newer version of the mod.";
        return res;
    }
    uint32_t hlen = r.u32();
    if (!r.ok() || hlen > r.left()) return damaged("header");
    size_t hpos = 8 + 2 + 2 + 4;
    const uint8_t* hp = data + hpos;
    ByteReader after(data + hpos + hlen, n - hpos - hlen);
    if (after.u32() != crc32(hp, hlen) || !after.ok()) return damaged("header CRC");
    ByteReader hr(hp, hlen);
    GdfwrFile f;
    f.header.formatVersion = fmt;
    f.header.minReader = minReader;
    if (!readHeader(hr, f.header)) return damaged("header");

    bool haveLevel = false, haveTime = false, haveMacro = false;
    size_t pos = hpos + hlen + 4;
    while (pos < n) {
        if (n - pos < 14) return damaged("truncated section header");
        ByteReader sh(data + pos, n - pos);
        char id[5] = {char(sh.u8()), char(sh.u8()), char(sh.u8()), char(sh.u8()), 0};
        uint16_t sver = sh.u16();
        uint32_t len = sh.u32();
        if (size_t(len) + 4 > n - pos - 10) return damaged("truncated section " + std::string(id));
        const uint8_t* pl = data + pos + 10;
        uint32_t stored = ByteReader(pl + len, 4).u32();
        bool crcOk = (stored == crc32(pl, len));
        pos += 10 + len + 4;
        std::string sid(id);
        bool required = (sid == "LEVL" || sid == "TIME" || sid == "MACR");
        bool derived = (sid == "BASE" || sid == "ANAL");
        if (!required && !derived) continue;  // unknown section: skip by length (Part 2, 2.2)
        ByteReader pr(pl, len);
        if (required) {
            if (!crcOk) return damaged("bad CRC in " + sid);
            bool ok = (sid == "LEVL") ? readLevel(pr, f.level) : (sid == "TIME") ? readTiming(pr, f.timing) : readMacro(pr, f.macro, sver);
            if (!ok) return damaged("malformed " + sid);
            if (sid == "LEVL") haveLevel = true; else if (sid == "TIME") haveTime = true; else haveMacro = true;
        } else {
            bool ok = crcOk;
            if (ok && sid == "BASE") { Baseline b; ok = readBase(pr, b); if (ok) f.baseline = std::move(b); }
            if (ok && sid == "ANAL") { AnalysisData a; ok = readAnal(pr, a, sver); if (ok) f.analysis = std::move(a); }
            if (!ok) res.loadWarnings.push_back({std::string(code::DerivedDropped), sid + " section dropped (bad CRC or malformed)"});
        }
    }
    if (!haveLevel || !haveTime || !haveMacro) return damaged("missing section");

    // stale rule (Part 2, 2.9)
    uint64_t h = computeMacroHash(f.macro);
    if (f.baseline && f.baseline->macroHash != h) f.baseline->stale = true;
    if (f.analysis && f.analysis->macroHash != h) f.analysis->stale = true;
    f.macro.macroHash = h;
    res.file = std::move(f);
    res.ok = true;
    return res;
}

LevelMatch compareLevel(const LevelInfo& inFile, bool haveLoadedLevel, int64_t loadedId, uint64_t loadedHash) {
    if (!haveLoadedLevel) return LevelMatch::NoLevelLoaded;
    if (inFile.levelId != loadedId) return LevelMatch::Mismatch;
    return inFile.dataHash == loadedHash ? LevelMatch::Same : LevelMatch::Changed;
}

void applyLevelMatch(GdfwrFile& f, LevelMatch m, std::vector<Warning>& out) {
    if (m == LevelMatch::Changed) {
        out.push_back({std::string(code::LevelChanged), "same level ID, different level data"});
        if (f.baseline) f.baseline->stale = true;
        if (f.analysis) f.analysis->stale = true;
    } else if (m == LevelMatch::Mismatch) {
        out.push_back({std::string(code::LevelMismatch), "the file belongs to a different level"});
    }
}

bool readWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz < 0) return false;
    in.seekg(0);
    out.resize(size_t(sz));
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return bool(in) || in.eof();
}

bool writeAtomicBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, bool keepBackup, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { if (err) *err = "cannot create " + tmp.string(); return false; }
        o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        o.flush();
        if (!o) { o.close(); fs::remove(tmp, ec); if (err) *err = "write failed (disk full?)"; return false; }
    }
    if (keepBackup && fs::exists(path, ec)) {
        fs::path bak = path;
        bak += ".bak";
        fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);  // SHOULD, not MUST (8.4.2)
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); if (err) *err = "rename failed: " + ec.message(); return false; }
    return true;
}

SaveResult saveAtomic(const std::filesystem::path& path, const GdfwrFile& f, bool keepBackup) {
    SaveResult r;
    std::string err;
    if (!writeAtomicBytes(path, serialize(f), keepBackup, &err)) {
        r.errorCode = std::string(code::SaveWriteFailed);
        r.errorText = "Could not save: " + err + ". The old file is unchanged.";
        return r;
    }
    r.ok = true;
    return r;
}

std::string sanitizeFileName(std::string s) {
    for (auto& c : s) if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s.empty() ? "level" : s;
}

std::string defaultFileName(const std::string& levelName, const std::tm& t, bool withTime, const std::string& ext) {
    char buf[96];
    if (withTime) std::snprintf(buf, sizeof buf, "_%04d-%02d-%02d_%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    else std::snprintf(buf, sizeof buf, "_%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return sanitizeFileName(levelName + buf) + ext;
}

bool hasWarning(const GdfwrFile& f, std::string_view c) {
    return std::any_of(f.header.warnings.begin(), f.header.warnings.end(), [&](const Warning& w) { return w.code == c; });
}

std::string analysisGate(const GdfwrFile& f, bool levelMatches) {
    if (f.level.platformer) return "Platformer levels are not supported.";
    if (f.timing.tps != kTps) return "Analysis needs 240 TPS.";
    // TPS_CHANGED is only a warning now: the first step of Analyze replays the macro at 240 TPS, and a macro that was recorded at another
    // TPS does not survive that replay (the check says so). Refusing here blocked recordings on a detector that can be wrong.
    if (!f.macro.finalRun.exists) return "The recording has no final run.";
    // An imported macro stays PARTIAL until a verification reaches the end of the level (D2). That verification is step 1 of
    // Analyze (there is no separate Verify button), so Analyze must accept an imported PARTIAL file. A recorded PARTIAL file
    // (the player did not finish the level) is still refused: it has no run to the end to verify.
    bool importAwaitingVerification = f.header.origin == Origin::Imported && f.header.rec == RecStatus::Partial;
    if (f.header.rec != RecStatus::Completed && !importAwaitingVerification) return "Analysis needs a completed run.";
    if (!validateEvents(finalRunEvents(f.macro)).ok) return "The event sequence is invalid. Fix it first.";
    if (!levelMatches) return "The file belongs to a different level (LEVEL_MISMATCH).";
    return {};
}

const char* analysisStateName(const GdfwrFile& f) {
    if (!f.analysis) return "NONE";
    return f.analysis->stale ? "STALE" : "CURRENT";
}

}  // namespace gdfwr
