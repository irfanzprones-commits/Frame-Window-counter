#include "Overlays.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "core/Trace.hpp"
#include "game/State.hpp"

using namespace geode::prelude;

namespace gdfwr::ui {
namespace {
ccColor3B rgb(uint32_t c) { return ccColor3B{uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}; }
CCLabelBMFont* label(char const* txt, char const* font, float scale, CCPoint anchor) {
    auto l = CCLabelBMFont::create(txt, font);
    l->setScale(scale);
    l->setAnchorPoint(anchor);
    return l;
}
}  // namespace

// ---- recording HUD ---------------------------------------------------------------------------------------------
RecHud* RecHud::create() {
    auto r = new RecHud();
    if (r->init()) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}
bool RecHud::init() {
    if (!CCNode::init()) return false;
    auto win = CCDirector::get()->getWinSize();
    m_label = label("", "bigFont.fnt", 0.32f, {0.f, 1.f});
    m_label->setPosition({win.width * 0.012f, win.height * 0.985f});
    m_label->setOpacity(190);
    this->addChild(m_label);
    this->scheduleUpdate();
    return true;
}
void RecHud::update(float dt) {
    auto& S = game::State::get();
    if (dt > 0.f && !S.sim.active) {  // frame-rate statistics for the timing section (Part 2, 2.5)
        float fps = 1.f / dt;
        S.fpsAvg = S.fpsAvg * 0.98f + fps * 0.02f;
        S.fpsMin = std::min(S.fpsMin, fps);
    }
    if (S.trace.recording && !S.sim.active) S.trace.noteFrame(dt);   // v0.3.2: counts the display frames, so the trace can say how many steps ran in each
    using St = gdfwr::SessionModel::State;
    std::string t;
    ccColor3B col{255, 255, 255};
    switch (S.session.state()) {
        case St::Active: {
            char b[64];
            std::snprintf(b, sizeof b, "REC  %d actions", S.session.liveActionCount());
            t = b;
            col = {255, 110, 110};
            int id = S.session.currentSegment();
            if (id >= 0 && S.session.macro().segments[size_t(id)].flags) t += "  [check]";
            break;
        }
        case St::Suspended: t = "REC PAUSED - nothing is captured"; col = {255, 220, 120}; break;
        case St::Armed: t = "REC ARMED - restart the level to continue"; col = {255, 220, 120}; break;
        default: break;
    }
    if (!Mod::get()->getSettingValue<bool>("show-hud")) t.clear();
    m_label->setString(t.c_str());
    m_label->setColor(col);
}

// ---- replay HUD --------------------------------------------------------------------------------------------------
ReplayHud* ReplayHud::create(int levelTicks) {
    auto r = new ReplayHud();
    if (r->init(levelTicks)) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}
bool ReplayHud::init(int levelTicks) {
    if (!CCNode::init()) return false;
    m_levelTicks = levelTicks;
    auto win = CCDirector::get()->getWinSize();
    for (int i = 0; i < gdfwr::kReplayBucketCount; i++) {  // 25+ on top ... 1 at the bottom (9.4, v2: nine buckets)
        auto l = label("", "bigFont.fnt", 0.42f, {0.f, 0.5f});
        l->setPosition({win.width * 0.015f, win.height * (0.84f - 0.055f * float(i))});
        l->setColor(rgb(gdfwr::replayBucketColor(gdfwr::ReplayBucket(i))));
        this->addChild(l);
        m_lines[size_t(i)] = l;
    }
    m_precision = label("", "bigFont.fnt", 0.34f, {0.f, 0.f});
    m_precision->setPosition({win.width * 0.015f, win.height * 0.03f});
    m_precision->setColor({255, 255, 255});
    this->addChild(m_precision);
    this->refreshText();
    this->scheduleUpdate();
    return true;
}
void ReplayHud::refreshText() {
    for (int i = 0; i < gdfwr::kReplayBucketCount; i++) {
        char b[48];
        std::snprintf(b, sizeof b, "%s : %d", gdfwr::replayBucketLabel(gdfwr::ReplayBucket(i)), m_count[size_t(i)]);
        m_lines[size_t(i)]->setString(b);
    }
    if (!Mod::get()->getSettingValue<bool>("precision-readout")) { m_precision->setString(""); return; }
    // PROVISIONAL model (Appendix A item 5): the label is the one of the reference, the model is not confirmed yet.
    double v = gdfwr::rawPrecision24h(m_windows, m_levelTicks);
    std::string t = "Raw Precision (24h) in sigma/s\n" + gdfwr::formatPrecision(v) + " sigma/s";
    m_precision->setString(t.c_str());
}
void ReplayHud::onAction(PlayLayer* pl, gdfwr::ReplayRing const& ring, CCPoint where) {
    if (ring.known) {
        m_count[size_t(ring.bucket)]++;
        m_windows.push_back(ring.width);
        this->refreshText();
    }
    m_hold = std::max(0.f, float(Mod::get()->getSettingValue<double>("ring-hold")));
    m_fade = std::max(0.05f, float(Mod::get()->getSettingValue<double>("ring-fade")));
    Ring r;
    r.colour = ring.known ? gdfwr::replayBucketColor(ring.bucket) : 0x888888;
    // "?" = this click could not be measured; "any" = measured, but the release never mattered (nothing to count)
    std::string txt = ring.known ? std::to_string(ring.window) + (ring.plus ? "+" : "") : (ring.insensitive ? "any" : "?");
    r.node = CCNode::create();
    // v0.3.0: the ring is drawn where the player was ON THE SCREEN when the click happened and stays there while it fades. (Before, it was
    // fixed in the level and scrolled out of the screen within a second, and a ring that was still alive when the replay stopped stayed in the
    // level for good.)
    CCPoint screen = pl->m_objectLayer->convertToWorldSpace(where);
    r.node->setPosition(this->convertToNodeSpace(screen));
    r.text = label(txt.c_str(), "bigFont.fnt", 0.33f, {0.5f, 0.5f});
    r.text->setColor(rgb(r.colour));
    r.node->addChild(r.text, 1);
    this->addChild(r.node.data(), 10);
    this->paintRing(r, 1.f);
    m_rings.push_back(std::move(r));
}
// The circle is drawn again with the opacity it has now (a CCDrawNode has no opacity of its own).
void ReplayHud::paintRing(Ring& r, float opacity) {
    float a = std::clamp(opacity, 0.f, 1.f);
    if (r.draw) r.draw->removeFromParent();
    r.draw = CCDrawNode::create();
    constexpr int N = 28;
    CCPoint v[N];
    for (int i = 0; i < N; i++) {
        float t = float(i) * 6.2831853f / float(N);
        v[i] = CCPoint(std::cos(t) * 15.f, std::sin(t) * 15.f);   // not {..}: CCPoint::operator= is ambiguous with CCSize
    }
    float cr = float(r.colour >> 16 & 255) / 255.f, cg = float(r.colour >> 8 & 255) / 255.f, cb = float(r.colour & 255) / 255.f;
    // the colour is premultiplied by the opacity: that is how the draw node blends
    r.draw->drawPolygon(v, N, ccColor4F{0.f, 0.f, 0.f, 0.f}, 1.6f, ccColor4F{cr * a, cg * a, cb * a, a});
    r.node->addChild(r.draw, 0);
    r.text->setOpacity(uint8_t(a * 255.f));
    r.shown = a;
}
void ReplayHud::clear() {
    for (auto& r : m_rings) if (r.node) r.node->removeFromParent();
    m_rings.clear();
}
// No growing, no sudden end: full strength at once, a steady hold, then a slow fade (ringOpacity in core/Metrics).
void ReplayHud::update(float dt) {
    for (auto& r : m_rings) {
        r.age += dt;
        float a = gdfwr::ringOpacity(r.age, m_hold, m_fade);
        if (a != r.shown) this->paintRing(r, a);
    }
    std::erase_if(m_rings, [&](Ring& r) {
        if (gdfwr::ringOpacity(r.age, m_hold, m_fade) > 0.f) return false;
        r.node->removeFromParent();
        return true;
    });
}

// ---- progress popup ----------------------------------------------------------------------------------------------
ProgressPopup* ProgressPopup::create(std::string const& title, Hooks h) {
    auto r = new ProgressPopup();
    if (r->init(title, std::move(h))) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}
bool ProgressPopup::init(std::string const& title, Hooks h) {
    auto win = CCDirector::get()->getWinSize();
    float w = win.width * 0.82f, hgt = win.height * 0.72f;
    if (!Popup::init(w, hgt)) return false;
    m_h = std::move(h);
    this->setTitle(title);
    m_barW = w * 0.8f;
    auto back = CCLayerColor::create({0, 0, 0, 120}, m_barW, hgt * 0.06f);
    m_mainLayer->addChildAtPosition(back, Anchor::BottomLeft, {w * 0.1f, hgt * 0.62f}, false);
    m_bar = CCLayerColor::create({120, 220, 140, 255}, 1.f, hgt * 0.06f);
    m_mainLayer->addChildAtPosition(m_bar, Anchor::BottomLeft, {w * 0.1f, hgt * 0.62f}, false);
    m_info = label("", "chatFont.fnt", 0.6f, {0.f, 1.f});
    m_mainLayer->addChildAtPosition(m_info, Anchor::BottomLeft, {w * 0.1f, hgt * 0.55f}, false);
    auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Cancel"), this, menu_selector(ProgressPopup::onClose));
    m_buttonMenu->addChildAtPosition(btn, Anchor::Bottom, {0.f, hgt * 0.1f});
    this->schedule(schedule_selector(ProgressPopup::tick), 0.f);
    return true;
}
void ProgressPopup::tick(float dt) {
    if (m_finished) return;
    m_elapsed += dt;
    // v0.3.0: about half of the last frame, between 8 and 40 ms. A fixed 8 ms left the analysis a tenth of the time at 14 FPS (70 ms per
    // frame); the bar and the text still update every frame, and the game stays as responsive as it is at that frame rate.
    double budget = std::clamp(double(dt) * 500.0, 8.0, 40.0);
    bool fin = m_h.pump(budget);
    float p = std::clamp(m_h.progress ? m_h.progress() : 0.f, 0.f, 1.f);
    m_bar->setContentSize({std::max(1.f, m_barW * p), m_bar->getContentSize().height});
    std::string s = m_h.status ? m_h.status() : "";
    if (p > 0.02f && !fin) {  // time left (Part 5, step 4)
        char b[64];
        std::snprintf(b, sizeof b, "\n%d%%  -  about %d s left", int(p * 100), int(m_elapsed * (1.f - p) / p));
        s += b;
    }
    m_info->setString(s.c_str());
    if (fin) this->finish();
}
void ProgressPopup::finish() {
    if (m_finished) return;
    m_finished = true;
    this->unschedule(schedule_selector(ProgressPopup::tick));
    auto done = m_h.done;
    Popup::onClose(nullptr);
    if (done) done();
}
void ProgressPopup::onClose(CCObject*) {
    if (m_finished) return;
    if (m_h.cancel) m_h.cancel();  // Cancel keeps partial results where the job has some (Part 5, step 4)
    this->finish();
}

// ---- result view -------------------------------------------------------------------------------------------------
std::string windowText(gdfwr::WindowResult const& w) {
    if (w.na) return "N/A";
    if (!w.analysed) return "-";
    char b[64];
    std::snprintf(b, sizeof b, "%d%s (%d%s/%d%s)", w.window(), w.lowerBound() ? "+" : "", w.early,
                  w.stopEarly == gdfwr::StopReason::Death ? "" : "+", w.late, w.stopLate == gdfwr::StopReason::Death ? "" : "+");
    std::string t = b;
    // v2: the continuous width in ticks (what NaN GD counts). Whole-tick runs only know a range of 2 ticks, so it is shown for
    // sub-tick runs only; the whole-tick count can read one higher or lower depending on where the click fell.
    if (w.subTick && !w.lowerBound()) {
        char c[40];
        std::snprintf(c, sizeof c, " w=%.2f-%.2f", w.widthLo(), w.widthHi());
        t += c;
    }
    if (w.extended) t += " long";
    return t;
}
std::string flagsText(uint32_t f) {
    std::string s;
    using namespace gdfwr::ClickFlag;
    if (f & Unreliable) s += " UNRELIABLE";
    if (f & Random) s += " RANDOM";
    if (f & NotAnalysed) s += " NOT_ANALYSED";
    if (f & Clamp) s += " CLAMP";
    if (f & Cluster) s += " CLUSTER";
    if (f & Drift) s += " DRIFT";
    if (f & FramePerfect) s += " FRAME-PERFECT";
    if (f & ReleaseInsensitive) s += " RELEASE-INSENSITIVE";
    if (f & Edited) s += " EDITED";
    return s;
}

ResultPopup* ResultPopup::create(gdfwr::GdfwrFile const& f, std::vector<std::string> const& extraRows) {
    auto r = new ResultPopup();
    if (r->init(f, extraRows)) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}
bool ResultPopup::init(gdfwr::GdfwrFile const& f, std::vector<std::string> const& extraRows) {
    auto win = CCDirector::get()->getWinSize();
    float w = win.width, h = win.height;  // fills the whole screen (Part 10)
    if (!Popup::init(w, h)) return false;
    this->setTitle(f.analysis && f.analysis->incomplete ? "Analysis result (INCOMPLETE)" : "Analysis result");
    auto const& a = *f.analysis;
    std::vector<std::string> rows;
    // v0.3.2: the silent replay against the real playback comes first (what was measured, not what the analysis concluded from it)
    if (!extraRows.empty()) {
        for (auto const& l : extraRows) for (auto const& w : gdfwr::wrapLines(l, 150)) rows.push_back(w);
        rows.push_back("");
    }
    auto ids = [&](std::vector<uint32_t> const& v, bool cluster) {
        std::string s;
        for (auto i : v) s += " #" + std::to_string(i + 1) + (cluster ? "" : "");
        return v.empty() ? std::string(" none") : s;
    };
    char b[220];
    // a long explanation goes on several rows instead of running off the screen
    auto addWrapped = [&](std::string s, size_t width = 150) {
        while (s.size() > width) {
            size_t cut = s.rfind(' ', width);
            if (cut == std::string::npos || cut < width / 2) cut = width;
            rows.push_back(s.substr(0, cut));
            s = "   " + s.substr(cut + (s[cut] == ' ' ? 1 : 0));
        }
        rows.push_back(s);
    };
    // v0.3.0: the recorded ticks next to the ticks the clicks have now (the replay fits a click that does not pass to the game's own timing).
    // key: {player, click index inside the player's stream} -> {recorded press, press, recorded release, release}
    std::map<std::pair<int, int>, std::array<int, 4>> ticks;
    {
        auto evs = gdfwr::finalRunEvents(f.macro);
        std::map<uint32_t, gdfwr::Event const*> bySeq;
        for (auto const& e : evs) bySeq[e.seq] = &e;
        int idx[2] = {0, 0};
        for (auto const& e : evs) {
            if (e.type != gdfwr::EventType::Press) continue;
            int p = e.player ? 1 : 0;
            auto rel = bySeq.find(uint32_t(e.partner));
            int recRel = -1, rel2 = -1;
            if (e.partner >= 0 && rel != bySeq.end()) { recRel = rel->second->tick; rel2 = rel->second->editedTick; }
            ticks[{p, idx[p]++}] = {e.tick, e.editedTick, recRel, rel2};
        }
    }
    // v0.3.1: when the Fit moved clicks, the macro in this file is NOT the one that was recorded. That is what made a macro that passes in
    // normal playback die at its spike (and a tight jump read "13+"), so it is the FIRST thing the result says.
    {
        int movedClicks = 0;
        for (auto const& kv : ticks) {
            auto const& t = kv.second;
            if (t[0] != t[1] || t[2] != t[3]) movedClicks++;
        }
        if (movedClicks > 0) {
            addWrapped("WARNING: " + std::to_string(movedClicks) + " click(s) of this macro are NOT at the tick they were recorded at (moved by the Fit, or edited by you). The windows below belong to the ticks the clicks have now, not to the recorded ones.");
            addWrapped("If the recorded macro passes when you play it back normally, this moved macro may die. Edit clicks > Restore all brings the recorded ticks back.");
            rows.push_back("");
        }
    }
    auto tightest = [&](std::vector<uint32_t> const& v, bool inWin, bool cl) {
        if (v.empty()) return std::string("none");
        auto const& w0 = cl ? a.clusters[v[0]].win : (inWin ? a.clicks[v[0]].in : a.clicks[v[0]].out);
        return std::to_string(w0.window()) + " ticks:" + ids(v, cl);
    };
    // v2: every action has its own window. v1 files (wholeClick) measured the whole click moved as one piece.
    const char* inName = a.wholeClick ? "click" : "press";
    rows.push_back(std::string("Tightest ") + inName + " window: " + tightest(a.summary.tightestInput, true, false));
    rows.push_back("Tightest release window: " + tightest(a.summary.tightestOutput, false, false));
    rows.push_back("Tightest cluster: " + tightest(a.summary.tightestCluster, true, true));
    std::snprintf(b, sizeof b, "Average %s window: %.1f%s   release window: %.1f%s   (%d reliable clicks)", inName, a.summary.avgInput,
                  a.summary.avgInputLower ? "+" : "", a.summary.avgOutput, a.summary.avgOutputLower ? "+" : "", a.summary.reliableClicks);
    rows.push_back(b);
    auto const& k = a.counters;
    std::snprintf(b, sizeof b, "1:%d  2:%d  3:%d  4:%d  5-6:%d  7-10:%d  11-24:%d  25+:%d  N+:%d  CLAMP:%d  UNRELIABLE:%d  RANDOM:%d  NOT_ANALYSED:%d",
                  k.b1, k.b2, k.b3, k.b4, k.b5_6, k.b7_10, k.b11_24, k.b25p, k.nPlus, k.clamp, k.unreliable, k.random, k.notAnalysed);
    rows.push_back(b);
    addWrapped("A window is the number of ticks a click can sit at and still pass (early + 1 + late). A '+' means one side never ended in a death (it passed every shift that was tested, or a neighbouring click stopped the search), so the window is AT LEAST that wide.");
    if (a.summary.reliableClicks == 0 && !a.clicks.empty())
        addWrapped("NOTHING WAS COUNTED: clicks marked UNRELIABLE, RANDOM or NOT_ANALYSED are never counted (flags at the end of each click below).");
    else if (k.unreliable + k.random + k.notAnalysed > 0) {
        std::snprintf(b, sizeof b, "%d of %zu clicks were counted; %d are marked UNRELIABLE / RANDOM / NOT_ANALYSED (flags at the end of each click below).",
                      a.summary.reliableClicks, a.clicks.size(), k.unreliable + k.random + k.notAnalysed);
        addWrapped(b);
    }
    for (auto const& wr : a.warnings)
        addWrapped("! " + game::describeCode(wr.code) + (wr.code == "RESTORE_REPLAYED" && !wr.detail.empty() ? " (" + wr.detail + ")" : std::string()));
    {   // the clicks the fit moved
        std::vector<std::string> moved;
        for (auto const& c : a.clicks) {
            auto it = ticks.find({int(c.player), int(c.index)});
            if (it == ticks.end() || (it->second[0] == it->second[1] && it->second[2] == it->second[3])) continue;
            auto const& t = it->second;
            std::snprintf(b, sizeof b, "   #%u  P%d  press %d -> %d (%+d)", c.index + 1, c.player + 1, t[0], t[1], t[1] - t[0]);
            std::string s = b;
            if (t[2] >= 0 && t[2] != t[3]) { std::snprintf(b, sizeof b, "   release %d -> %d (%+d)", t[2], t[3], t[3] - t[2]); s += b; }
            moved.push_back(s);
        }
        if (!moved.empty()) {
            addWrapped("The clicks that are not at their recorded tick (moved by the Fit, or edited):");
            for (auto& m : moved) rows.push_back(m);
            addWrapped("   Edit clicks > Restore all brings the recorded ticks back. The windows below are around the ticks the clicks have NOW.");
        }
    }
    if (f.timing.clickResolution > 1) {
        std::snprintf(b, sizeof b, "This recording is only exact to %d ticks per click (one frame at about %d FPS, no click-between-frames), so a recorded tick can be up to that far from the tick the game used.",
                      int(f.timing.clickResolution), int(std::lround(240.0 / double(f.timing.clickResolution))));
        addWrapped(b);
    }
    if (!a.warnings.empty() && a.summary.reliableClicks > 0)
        addWrapped("The recording warnings above do not change these numbers: every window was measured by replaying the level.");
    rows.push_back("");
    for (size_t i = 0; i < a.clicks.size(); i++) {
        auto const& c = a.clicks[i];
        std::string rec;
        if (auto it = ticks.find({int(c.player), int(c.index)}); it != ticks.end() && (it->second[0] != c.press || (it->second[2] >= 0 && it->second[2] != c.release))) {
            std::snprintf(b, sizeof b, " (recorded %d-%d)", it->second[0], it->second[2]);
            rec = b;
        }
        std::snprintf(b, sizeof b, "#%zu  P%d  ticks %d-%d%s   %s %s   RELEASE %s%s", i + 1, c.player + 1, c.press, c.release, rec.c_str(),
                      a.wholeClick ? "CLICK" : "PRESS", windowText(c.in).c_str(), windowText(c.out).c_str(), flagsText(c.flags).c_str());
        rows.push_back(b);
        for (auto const* wr : {&c.in, &c.out})
            if (wr->fullScan) {
                std::snprintf(b, sizeof b, "      %s pass map: %s | %s   holes %d  traps %d", wr == &c.in ? (a.wholeClick ? "click  " : "press  ") : "release", wr->mapEarly.c_str(), wr->mapLate.c_str(), wr->holes, wr->traps);
                rows.push_back(b);
            }
    }
    float listW = w * 0.94f, listH = h * 0.78f, rowH = h * 0.034f;
    auto scroll = ScrollLayer::create({listW, listH});
    float total = std::max(listH, rowH * float(rows.size()) + 6.f);
    scroll->m_contentLayer->setContentSize({listW, total});
    for (size_t i = 0; i < rows.size(); i++) {
        auto l = label(rows[i].c_str(), "chatFont.fnt", 0.5f, {0.f, 0.5f});
        l->setPosition({w * 0.01f, total - rowH * (float(i) + 0.5f)});
        scroll->m_contentLayer->addChild(l);
    }
    scroll->moveToTop();
    m_mainLayer->addChildAtPosition(scroll, Anchor::BottomLeft, {w * 0.03f, h * 0.08f}, false);
    return true;
}

// ---- a scrolling text popup with two buttons (v0.3.2) -----------------------------------------------------------------------------------
ChoicePopup* ChoicePopup::create(std::string const& title, std::vector<std::string> const& rows, std::string const& noText, std::string const& yesText,
                                 std::function<void(bool)> answer) {
    auto r = new ChoicePopup();
    if (r->init(title, rows, noText, yesText, std::move(answer))) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}
bool ChoicePopup::init(std::string const& title, std::vector<std::string> const& rows, std::string const& noText, std::string const& yesText,
                       std::function<void(bool)> answer) {
    auto win = CCDirector::get()->getWinSize();
    float w = win.width, h = win.height;  // fills the whole screen, like the result view (Part 10)
    if (!Popup::init(w, h)) return false;
    m_answer = std::move(answer);
    this->setTitle(title);
    // every row is wrapped so that nothing runs off the screen; the list scrolls, the two buttons stay at the bottom
    std::vector<std::string> lines;
    for (auto const& r : rows) for (auto const& l : gdfwr::wrapLines(r, 135)) lines.push_back(l);
    float listW = w * 0.94f, listH = h * 0.66f, rowH = h * 0.034f;
    auto scroll = ScrollLayer::create({listW, listH});
    float total = std::max(listH, rowH * float(lines.size()) + 6.f);
    scroll->m_contentLayer->setContentSize({listW, total});
    for (size_t i = 0; i < lines.size(); i++) {
        auto l = label(lines[i].c_str(), "chatFont.fnt", 0.5f, {0.f, 0.5f});
        l->setPosition({w * 0.01f, total - rowH * (float(i) + 0.5f)});
        scroll->m_contentLayer->addChild(l);
    }
    scroll->moveToTop();
    m_mainLayer->addChildAtPosition(scroll, Anchor::BottomLeft, {w * 0.03f, h * 0.20f}, false);
    auto no = CCMenuItemSpriteExtra::create(ButtonSprite::create(noText.c_str()), this, menu_selector(ChoicePopup::onNo));
    auto yes = CCMenuItemSpriteExtra::create(ButtonSprite::create(yesText.c_str()), this, menu_selector(ChoicePopup::onYes));
    m_buttonMenu->addChildAtPosition(no, Anchor::Bottom, {-w * 0.15f, h * 0.10f});
    m_buttonMenu->addChildAtPosition(yes, Anchor::Bottom, {w * 0.15f, h * 0.10f});
    return true;
}
// Whatever closes the popup (a button, the back key, the close button of the base class) answers once; the answer of the callback never runs twice.
void ChoicePopup::answerOnce(bool yes) {
    if (m_answered) return;
    m_answered = true;
    auto answer = std::move(m_answer);
    Popup::onClose(nullptr);
    if (answer) answer(yes);
}
void ChoicePopup::onNo(CCObject*) { this->answerOnce(false); }
void ChoicePopup::onYes(CCObject*) { this->answerOnce(true); }
void ChoicePopup::onClose(CCObject*) { this->answerOnce(false); }

}  // namespace gdfwr::ui
