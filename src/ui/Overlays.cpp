#include "Overlays.hpp"

#include <cmath>

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
    uint32_t colour = ring.known ? gdfwr::replayBucketColor(ring.bucket) : 0x888888;
    auto node = CCNode::create();
    node->setPosition(where);
    auto draw = CCDrawNode::create();
    constexpr int N = 28;
    CCPoint v[N];
    for (int i = 0; i < N; i++) {
        float a = float(i) * 6.2831853f / float(N);
        v[i] = CCPoint(std::cos(a) * 15.f, std::sin(a) * 15.f);   // not {..}: CCPoint::operator= is ambiguous with CCSize
    }
    ccColor4F c{float(colour >> 16 & 255) / 255.f, float(colour >> 8 & 255) / 255.f, float(colour & 255) / 255.f, 1.f};
    draw->drawPolygon(v, N, ccColor4F{0.f, 0.f, 0.f, 0.f}, 1.6f, c);
    node->addChild(draw);
    std::string txt = ring.known ? std::to_string(ring.window) + (ring.plus ? "+" : "") : "?";
    auto l = label(txt.c_str(), "bigFont.fnt", 0.33f, {0.5f, 0.5f});
    l->setColor(rgb(colour));
    node->addChild(l);
    pl->m_objectLayer->addChild(node, 1000);
    m_rings.push_back({node, 0.f});
}
void ReplayHud::clear() {
    for (auto& r : m_rings) if (r.first) r.first->removeFromParent();
    m_rings.clear();
}
void ReplayHud::update(float dt) {
    for (auto& r : m_rings) {
        r.second += dt;
        r.first->setScale(1.f + 0.6f * std::min(1.f, r.second / 0.6f));
    }
    std::erase_if(m_rings, [](auto& r) {
        if (r.second < 0.6f) return false;
        r.first->removeFromParent();
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
    bool fin = m_h.pump(8.0);  // a few ms of work per frame keeps the game responsive
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

ResultPopup* ResultPopup::create(gdfwr::GdfwrFile const& f) {
    auto r = new ResultPopup();
    if (r->init(f)) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}
bool ResultPopup::init(gdfwr::GdfwrFile const& f) {
    auto win = CCDirector::get()->getWinSize();
    float w = win.width, h = win.height;  // fills the whole screen (Part 10)
    if (!Popup::init(w, h)) return false;
    this->setTitle(f.analysis && f.analysis->incomplete ? "Analysis result (INCOMPLETE)" : "Analysis result");
    auto const& a = *f.analysis;
    std::vector<std::string> rows;
    auto ids = [&](std::vector<uint32_t> const& v, bool cluster) {
        std::string s;
        for (auto i : v) s += " #" + std::to_string(i + 1) + (cluster ? "" : "");
        return v.empty() ? std::string(" none") : s;
    };
    char b[200];
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
    for (auto const& wr : a.warnings) rows.push_back("! " + game::describeCode(wr.code));
    rows.push_back("");
    for (size_t i = 0; i < a.clicks.size(); i++) {
        auto const& c = a.clicks[i];
        std::snprintf(b, sizeof b, "#%zu  P%d  ticks %d-%d   %s %s   RELEASE %s%s", i + 1, c.player + 1, c.press, c.release, a.wholeClick ? "CLICK" : "PRESS",
                      windowText(c.in).c_str(), windowText(c.out).c_str(), flagsText(c.flags).c_str());
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

}  // namespace gdfwr::ui
