// Runs the REAL replay HUD (src/ui/Overlays.cpp), the ring sound (src/game/RingSound.cpp) and the pitch popup (src/ui/PitchPopup.cpp) against the
// stub of the Geode API in tests/syntax-stubs: a level that can be scrolled, a sound engine that remembers what it was asked to play, saved values
// and settings that live in memory, and a tiny scene graph.
// It proves OUR logic: a ring stays at its place in the level and scrolls away with it, the grey "any" ring is invisible and silent, which ring
// plays which pitch, what every button of the popup does, that nothing lies outside the popup. It also checks the picture files (size, format).
// It does NOT prove that the real Geode / Geometry Dash calls behave like the stub does: that is the [BINDING] list in README.md.
// Build + run: sh tests/hud_test.sh
#include <Geode/Geode.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/Metrics.hpp"
#include "game/Jobs.hpp"
#include "game/RingSound.hpp"
#include "game/State.hpp"
#include "ui/Overlays.hpp"
#include "ui/PitchPopup.hpp"

using namespace gdfwr;
using namespace geode::prelude;
namespace G = gdfwr::game;

static int g_fail = 0, g_checks = 0;
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_fail++; std::printf("  FAIL line %d: %s == %s\n", __LINE__, #a, #b); } } while (0)
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// ---- helpers -----------------------------------------------------------------------------------------------------------------------------
// A level that can be scrolled: scrollTo(x) = the camera is x units further right, so every place of the level is drawn x units further left.
struct FakeLevel : PlayLayer {
    FakeLevel() { m_objectLayer = new CCNode; m_uiLayer = new CCNode; PlayLayer::current() = this; }
    ~FakeLevel() { PlayLayer::current() = nullptr; }
    void scrollTo(float x) { m_objectLayer->m_wx = -x; }
};

static ReplayRing measured(int window) {
    ReplayRing r; r.known = true; r.window = window; r.width = window; r.bucket = replayBucket(window); r.plus = window >= 25;
    return r;
}
static ReplayRing anyRing() { ReplayRing r; r.insensitive = true; return r; }   // measured, but the release never mattered
static ReplayRing unknownRing() { return ReplayRing{}; }                          // "?"

// the ring nodes of a HUD: its children drawn at z 10 (the counters are at 0, the menu with the pitch ring at 20)
static std::vector<CCNode*> ringNodes(ui::ReplayHud* hud) {
    std::vector<CCNode*> v;
    for (auto& [node, z] : hud->m_kids) if (z == 10) v.push_back(node);
    return v;
}
static auto& played() { return FMODAudioEngine::stubPlayed(); }

static void resetWorld() {   // settings and saved values as a fresh install has them
    Mod::stubBools()["ring-follow-level"] = true;
    Mod::stubBools()["ring-sound"] = true;
    Mod::stubDoubles()["ring-sound-volume"] = 1.0;
    Mod::stubDoubles()["ring-hold"] = 1.0;
    Mod::stubDoubles()["ring-fade"] = 2.0;
    Mod::stubSaved().clear();
    played().clear();
    CCSprite::stubMissingFile() = false;
}

// ---- 1. the ring stays in the level ------------------------------------------------------------------------------------------------------
static void testRingFollowsLevel() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    CHECK(hud != nullptr);
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    auto rings = ringNodes(hud);
    CHECK_EQ(rings.size(), size_t(1));
    CCNode* n = rings[0];
    CHECK(near(n->getPositionX(), 300.f) && near(n->getPositionY(), 120.f));   // it appears where the action happened
    hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), 300.f));                                     // the camera has not moved: it has not moved either

    lvl.scrollTo(100.f); hud->update(1.f / 60.f);                              // the level scrolls 100 units to the left ...
    CHECK(near(n->getPositionX(), 200.f)); CHECK(near(n->getPositionY(), 120.f));   // ... and the ring goes with it (this was the bug: it stayed)
    lvl.scrollTo(250.f); hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), 50.f));
    lvl.scrollTo(700.f); hud->update(1.f / 60.f);
    CHECK(n->getPositionX() < 0.f);                                            // gone to the left of the screen, with the level
    lvl.scrollTo(0.f); hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), 300.f));                                     // its place is the level, not the screen: scroll back and it is there again

    // a second ring somewhere else: both keep their distance while the level scrolls
    hud->onAction(&lvl, measured(2), CCPoint(420.f, 60.f));
    CHECK_EQ(ringNodes(hud).size(), size_t(2));
    CCNode* m = ringNodes(hud)[1];
    CHECK(near(m->getPositionX(), 420.f) && near(m->getPositionY(), 60.f));
    lvl.scrollTo(150.f); hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), 150.f)); CHECK(near(m->getPositionX(), 270.f)); CHECK(near(m->getPositionX() - n->getPositionX(), 120.f));

    // a ring that is created while the level is already scrolled is placed at once at the right screen position (no jump on the first frame)
    lvl.scrollTo(500.f);
    hud->onAction(&lvl, measured(9), CCPoint(800.f, 40.f));
    CHECK(near(ringNodes(hud)[2]->getPositionX(), 300.f) && near(ringNodes(hud)[2]->getPositionY(), 40.f));

    // the level is gone (the player left it): the HUD does not touch the old level and does not crash
    float keep = n->getPositionX();
    PlayLayer::current() = nullptr;
    lvl.scrollTo(900.f); hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), keep));
    PlayLayer::current() = &lvl;
}

static void testRingFollowSettingOff() {
    resetWorld();
    Mod::stubBools()["ring-follow-level"] = false;   // the v0.3.0 behaviour: it stays where it appeared on the screen
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    lvl.scrollTo(100.f);
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CCNode* n = ringNodes(hud)[0];
    CHECK(near(n->getPositionX(), 200.f));           // converted once, at the moment of the action
    lvl.scrollTo(400.f); hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), 200.f)); CHECK(near(n->getPositionY(), 120.f));
    Mod::stubBools()["ring-follow-level"] = true;    // the setting is read when the ring appears: this ring stays as it was
    hud->update(1.f / 60.f);
    CHECK(near(n->getPositionX(), 200.f));
}

static void testRingLife() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    hud->update(0.5f);  CHECK_EQ(ringNodes(hud).size(), size_t(1));    // hold 1 s: still there
    hud->update(1.0f);  CHECK_EQ(ringNodes(hud).size(), size_t(1));    // fading
    hud->update(2.0f);  CHECK_EQ(ringNodes(hud).size(), size_t(0));    // hold + fade over: removed from the HUD, nothing stays behind
}

// ---- 1b. game/Jobs.cpp feedReplay: a replayed click becomes a ring where the player is -------------------------------------------------------
// One recorded event for player 0 or 1, fed to the real feedReplay with the real HUD.
static void feedOne(FakeLevel& lvl, ui::ReplayHud* hud, ReplayRing const& ring, int player) {
    auto& R = G::State::get().replay;
    R = G::ReplayState{};
    R.active = true;
    Event e; e.type = EventType::Press; e.tick = 10; e.editedTick = 10; e.player = uint8_t(player);
    R.events = {e};
    R.plan.rings = {ring};
    R.hud = hud;
    G::feedReplay(&lvl, 10);
}
static void testFeedReplay() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    lvl.m_uiLayer->addChild(hud, 99);

    // the normal case: the player hangs directly under the level layer, so its position IS the place in the level
    auto* p1 = new PlayerObject; lvl.m_player1 = p1; lvl.m_objectLayer->addChild(p1); p1->setPosition(CCPoint(300.f, 120.f));
    feedOne(lvl, hud, measured(4), 0);
    CHECK_EQ(ringNodes(hud).size(), size_t(1));
    CCNode* a = ringNodes(hud).empty() ? nullptr : ringNodes(hud)[0];
    if (a) {
        CHECK(near(a->getPositionX(), 300.f) && near(a->getPositionY(), 120.f));
        lvl.scrollTo(100.f); hud->update(1.f / 60.f);
        CHECK(near(a->getPositionX(), 200.f));                                 // and it goes away with the level
    }
    CHECK_EQ(played().size(), size_t(1)); CHECK(!played().empty() && near(played()[0].speed, 1.0f));   // the 4 rings sounds at normal speed
    lvl.scrollTo(0.f);

    // the player hangs under ANOTHER node (offset 40 / -10 to the screen): the ring must still land on the player, in the level's coordinates
    auto* holder = new CCNode; holder->m_wx = 40.f; holder->m_wy = -10.f;
    auto* p2 = new PlayerObject; lvl.m_player2 = p2; holder->addChild(p2); p2->setPosition(CCPoint(260.f, 130.f));   // on the screen: 300 / 120
    hud->clear(); played().clear();
    feedOne(lvl, hud, measured(2), 1);
    CHECK_EQ(ringNodes(hud).size(), size_t(1));
    CCNode* b = ringNodes(hud).empty() ? nullptr : ringNodes(hud)[0];
    if (b) {
        CHECK(near(b->getPositionX(), 300.f) && near(b->getPositionY(), 120.f));   // on the player, where it is drawn
        lvl.scrollTo(100.f); hud->update(1.f / 60.f);
        CHECK(near(b->getPositionX(), 200.f));                                     // and now it is a place in the level like any other
    }
    CHECK(!played().empty() && near(played()[0].speed, 0.6f));                     // the 2 ring: 60 %
    lvl.scrollTo(0.f);

    // the level is already scrolled when the click is replayed: the ring still starts exactly on the player
    lvl.scrollTo(100.f);
    hud->clear();
    feedOne(lvl, hud, measured(2), 1);
    b = ringNodes(hud).empty() ? nullptr : ringNodes(hud)[0];
    if (b) CHECK(near(b->getPositionX(), 300.f) && near(b->getPositionY(), 120.f));
    lvl.scrollTo(0.f);

    // no player 2 in the level (single player): nothing happens, nothing crashes
    hud->clear(); played().clear();
    lvl.m_player2 = nullptr;
    feedOne(lvl, hud, measured(2), 1);
    CHECK_EQ(ringNodes(hud).size(), size_t(0)); CHECK_EQ(played().size(), size_t(0));

    // the grey "any" ring and the grey "?" ring through the same chain: "any" shows nothing and is silent, "?" shows a ring and is silent
    feedOne(lvl, hud, anyRing(), 0);
    CHECK_EQ(ringNodes(hud).size(), size_t(0)); CHECK_EQ(played().size(), size_t(0));
    feedOne(lvl, hud, unknownRing(), 0);
    CHECK_EQ(ringNodes(hud).size(), size_t(1)); CHECK_EQ(played().size(), size_t(0));

    G::State::get().replay = G::ReplayState{};   // leave the global state clean
}

// ---- 2. which ring is drawn, which ring is heard -----------------------------------------------------------------------------------------
static void testAnyRingIsInvisible() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    hud->onAction(&lvl, anyRing(), CCPoint(300.f, 120.f));
    CHECK_EQ(ringNodes(hud).size(), size_t(0));      // the grey "any" ring is not drawn at all ...
    CHECK_EQ(played().size(), size_t(0));            // ... and it makes no sound
    hud->update(1.f / 60.f);
    CHECK_EQ(ringNodes(hud).size(), size_t(0));

    hud->onAction(&lvl, unknownRing(), CCPoint(300.f, 120.f));
    CHECK_EQ(ringNodes(hud).size(), size_t(1));      // the grey "?" ring stays ...
    CHECK_EQ(played().size(), size_t(0));            // ... silent

    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK_EQ(ringNodes(hud).size(), size_t(2));
    CHECK_EQ(played().size(), size_t(1));            // a measured window is heard
}

// The counter lines at the left of the HUD ("25+ : 0" on top ... "1 : 0" at the bottom): only a ring with a measured window is counted.
static std::vector<std::string> counterLines(ui::ReplayHud* hud) {
    std::vector<std::string> v;
    for (auto& [node, z] : hud->m_kids) {
        auto* l = dynamic_cast<CCLabelBMFont*>(node);
        if (l && z == 0 && l->m_text.find(" : ") != std::string::npos) v.push_back(l->m_text);
    }
    return v;
}
static int counted(std::vector<std::string> const& lines) {   // the sum of all counters
    int sum = 0;
    for (auto& t : lines) sum += std::atoi(t.c_str() + t.find(" : ") + 3);
    return sum;
}
static void testHudCounters() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    auto lines = counterLines(hud);
    CHECK_EQ(lines.size(), size_t(9));
    if (lines.size() == 9) { CHECK_EQ(lines[0], std::string("25+ : 0")); CHECK_EQ(lines[5], std::string("4 : 0")); CHECK_EQ(lines[8], std::string("1 : 0")); }
    hud->onAction(&lvl, measured(4), CCPoint(10.f, 10.f));
    hud->onAction(&lvl, measured(4), CCPoint(20.f, 10.f));
    hud->onAction(&lvl, measured(1), CCPoint(30.f, 10.f));
    hud->onAction(&lvl, measured(30), CCPoint(40.f, 10.f));
    lines = counterLines(hud);
    if (lines.size() == 9) { CHECK_EQ(lines[0], std::string("25+ : 1")); CHECK_EQ(lines[5], std::string("4 : 2")); CHECK_EQ(lines[8], std::string("1 : 1")); }
    CHECK_EQ(counted(lines), 4);
    hud->onAction(&lvl, anyRing(), CCPoint(50.f, 10.f));       // neither the grey "any" ring ...
    hud->onAction(&lvl, unknownRing(), CCPoint(60.f, 10.f));   // ... nor the grey "?" ring is counted
    CHECK_EQ(counted(counterLines(hud)), 4);
}

static void testRingSoundPitches() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    struct Case { int window; float speed; };
    // 1 = 40 %, 2 = 60, 3 = 80, 4 = 100, 5-6 = 120, 7-8 = 140, 9-12 = 160, 13-24 = 180, 25+ = 200
    Case cases[] = {{1, .4f}, {2, .6f}, {3, .8f}, {4, 1.f}, {5, 1.2f}, {6, 1.2f}, {7, 1.4f}, {8, 1.4f}, {9, 1.6f}, {12, 1.6f}, {13, 1.8f}, {24, 1.8f}, {25, 2.f}, {60, 2.f}};
    for (auto const& c : cases) {
        played().clear();
        hud->onAction(&lvl, measured(c.window), CCPoint(300.f, 120.f));
        CHECK_EQ(played().size(), size_t(1));
        if (played().size() != 1) continue;
        CHECK(near(played()[0].speed, c.speed));
        CHECK_EQ(played()[0].path, std::string("clirfan.gdfwr/classic.wav"));   // the file of the resources folder, with the mod id in front
        CHECK(near(played()[0].volume, 1.f)); CHECK(near(played()[0].pan, 0.f));
    }
    // a pitch set in the popup is used at once, by the label of the window
    G::setRingPitch(ReplayBucket::W4, 250);
    played().clear();
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].speed, 2.5f));
    played().clear();
    hud->onAction(&lvl, measured(3), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].speed, .8f));             // the other labels keep theirs
    // a value outside the range in the save file (edited by hand) is played inside the range
    Mod::stubSaved()["pitch-pct-5"] = 100000; played().clear();
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].speed, 4.f));
    Mod::stubSaved()["pitch-pct-5"] = -7; played().clear();
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].speed, .1f));
}

static void testRingSoundSettings() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    Mod::stubBools()["ring-sound"] = false;
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK_EQ(ringNodes(hud).size(), size_t(1)); CHECK_EQ(played().size(), size_t(0));   // sound off: the ring is still drawn, silently
    G::playRingSound(ReplayBucket::W4);
    CHECK_EQ(played().size(), size_t(1));            // the Play button plays even then
    Mod::stubBools()["ring-sound"] = true;
    Mod::stubDoubles()["ring-sound-volume"] = 0.25; played().clear();
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].volume, .25f));
    Mod::stubDoubles()["ring-sound-volume"] = 7.0; played().clear();   // out of range: the engine never gets more than 1
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].volume, 1.f));
    Mod::stubDoubles()["ring-sound-volume"] = -1.0; played().clear();
    hud->onAction(&lvl, measured(4), CCPoint(300.f, 120.f));
    CHECK(near(played()[0].volume, 0.f));
}

static void testSavedPitchApi() {
    resetWorld();
    for (int i = 0; i < kReplayBucketCount; i++) CHECK_EQ(G::ringPitchPercent(ReplayBucket(i)), defaultRingPitchPercent(ReplayBucket(i)));   // nothing saved: the defaults
    CHECK_EQ(G::changeRingPitch(ReplayBucket::W4, 10), 110);
    CHECK_EQ(Mod::stubSaved()["pitch-pct-5"], int64_t(110));                        // stored under the key of its label
    CHECK_EQ(G::changeRingPitch(ReplayBucket::W4, -1), 109);
    CHECK_EQ(G::changeRingPitch(ReplayBucket::W4, 1000000), 400);                   // clamped at the top ...
    CHECK_EQ(G::changeRingPitch(ReplayBucket::W4, 10), 400);
    CHECK_EQ(G::changeRingPitch(ReplayBucket::W4, -1000000), 10);                   // ... and at the bottom, whatever the delta is
    CHECK_EQ(G::changeRingPitch(ReplayBucket::W4, -10), 10);
    CHECK_EQ(G::ringPitchPercent(ReplayBucket::W25P), 200);                         // the other labels did not change
    G::setRingPitch(ReplayBucket::W1, 5); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W1), 10);
    G::setRingPitch(ReplayBucket::W1, 999); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W1), 400);
    // a save file edited by hand: whatever is stored, what is read back is inside 10..400
    Mod::stubSaved()["pitch-pct-3"] = 99999; CHECK_EQ(G::ringPitchPercent(ReplayBucket(3)), 400);
    Mod::stubSaved()["pitch-pct-3"] = -50;   CHECK_EQ(G::ringPitchPercent(ReplayBucket(3)), 10);
    Mod::stubSaved()["pitch-pct-3"] = 0;     CHECK_EQ(G::ringPitchPercent(ReplayBucket(3)), 10);
    Mod::stubSaved()["pitch-pct-3"] = 250;   CHECK_EQ(G::ringPitchPercent(ReplayBucket(3)), 250);
    G::resetRingPitches();
    for (int i = 0; i < kReplayBucketCount; i++) CHECK_EQ(G::ringPitchPercent(ReplayBucket(i)), defaultRingPitchPercent(ReplayBucket(i)));
}

// ---- 3. the blue ring (picture) ----------------------------------------------------------------------------------------------------------
static void testRingIcon() {
    resetWorld();
    auto* icon = ui::makeRingIcon(24.f);
    CHECK(near(icon->getContentSize().width, 24.f) && near(icon->getContentSize().height, 24.f));   // the touch area is exactly that square
    CHECK(near(icon->m_anchor.x, .5f) && near(icon->m_anchor.y, .5f));                               // it turns and bounces around its middle
    CHECK_EQ(icon->m_kids.size(), size_t(1));
    CCNode* pic = icon->m_kids[0].first;
    CHECK(near(pic->getPositionX(), 12.f) && near(pic->getPositionY(), 12.f));                       // centred
    CHECK(near(pic->getContentSize().width * pic->getScale(), 24.f));                                // the 256 px picture is scaled to the asked width
    // the file is missing: a drawn ring takes its place, the button keeps its size and is never invisible
    CCSprite::stubMissingFile() = true;
    auto* drawn = ui::makeRingIcon(24.f);
    CHECK(near(drawn->getContentSize().width, 24.f)); CHECK_EQ(drawn->m_kids.size(), size_t(1));
    CHECK(dynamic_cast<CCDrawNode*>(drawn->m_kids[0].first) != nullptr);
    CHECK(near(drawn->m_kids[0].first->getPositionX(), 12.f) && near(drawn->m_kids[0].first->getPositionY(), 12.f));
    CCSprite::stubMissingFile() = false;
    // the panel's button: ring + caption in ONE picture, the caption right of the ring, everything inside the touch area
    auto* cap = ui::makeRingCaptionButtonSprite(28.8f, "Ring pitch");
    CHECK(cap->getContentSize().width > 28.8f * 2 && near(cap->getContentSize().height, 28.8f));
    CHECK(near(cap->m_anchor.x, .5f) && near(cap->m_anchor.y, .5f));
    CHECK_EQ(cap->m_kids.size(), size_t(2));
    auto* txt = dynamic_cast<CCLabelBMFont*>(cap->m_kids[1].first);
    CHECK(txt != nullptr && txt->m_text == "Ring pitch");
    if (txt) CHECK(txt->getPositionX() > 28.8f && txt->getPositionX() + txt->getContentSize().width * txt->getScale() <= cap->getContentSize().width + 0.01f);
}

// ---- 4. the pieces of the HUD and the popup ------------------------------------------------------------------------------------------------
struct Item { int tag; float x, y, w, h; std::string caption; CCMenuItemSpriteExtra* it; };
static std::vector<Item> itemsOf(geode::Popup* p) {
    std::vector<Item> v;
    for (auto& [node, z] : p->stubMenu()->m_kids) {
        auto* it = dynamic_cast<CCMenuItemSpriteExtra*>(node);
        if (!it) continue;
        Item i{it->getTag(), it->getPositionX(), it->getPositionY(), 0, 0, "", it};
        if (!it->m_kids.empty()) {
            CCNode* pic = it->m_kids[0].first;
            i.w = pic->getContentSize().width * pic->getScale(); i.h = pic->getContentSize().height * pic->getScale();
            if (auto* b = dynamic_cast<ButtonSprite*>(pic)) i.caption = b->m_caption;
        }
        v.push_back(i);
    }
    return v;
}
static Item* byTag(std::vector<Item>& v, int tag) { for (auto& i : v) if (i.tag == tag) return &i; return nullptr; }
static Item* byCaption(std::vector<Item>& v, char const* c) { for (auto& i : v) if (i.caption == c) return &i; return nullptr; }
// the text of the value label of a row (the label that sits at the same height as the row's buttons, in the value column)
static std::string valueOf(geode::Popup* p, float rowY, ccColor3B* col = nullptr) {
    for (auto& [node, z] : p->stubMain()->m_kids)
        if (auto* l = dynamic_cast<CCLabelBMFont*>(node); l && near(l->getPositionY(), rowY) && near(l->getPositionX(), 569.f * 0.33f)) {
            if (col) *col = l->m_col;
            return l->m_text;
        }
    return "(none)";
}

static void testHudHasPitchRing() {
    resetWorld();
    FakeLevel lvl;
    auto* hud = ui::ReplayHud::create(1000);
    CCNode* menu = nullptr;
    for (auto& [node, z] : hud->m_kids) if (z == 20) menu = node;
    CHECK(menu != nullptr);
    if (!menu) return;
    CHECK_EQ(menu->m_kids.size(), size_t(1));
    auto* btn = dynamic_cast<CCMenuItemSpriteExtra*>(menu->m_kids[0].first);
    CHECK(btn != nullptr);
    if (!btn) return;
    CHECK(near(btn->getPositionX(), 569.f * 0.035f, .01f) && near(btn->getPositionY(), 320.f * 0.915f, .01f));   // above the "25+" line (which is at 0.84 of the height)
    CHECK(btn->getPositionY() > 320.f * 0.84f + 14.f);
    CHECK(btn->getContentSize().width > 10.f && near(btn->getContentSize().width, btn->getContentSize().height));
    CHECK(btn->getPositionX() - btn->getContentSize().width / 2 >= 0.f && btn->getPositionY() + btn->getContentSize().height / 2 <= 320.f);   // inside the screen
    FLAlertLayer::stubShown() = nullptr;
    btn->activate();                                  // a tap on the ring above the counters ...
    CHECK(dynamic_cast<ui::PitchPopup*>(FLAlertLayer::stubShown()) != nullptr);   // ... opens the pitch popup
}

static void testPitchPopup() {
    resetWorld();
    FLAlertLayer::stubShown() = nullptr;
    ui::openPitchPopup();
    auto* pop = dynamic_cast<ui::PitchPopup*>(FLAlertLayer::stubShown());
    CHECK(pop != nullptr);
    if (!pop) return;
    geode::Popup* p = pop;
    CHECK_EQ(p->m_title, std::string("Ring pitch"));
    auto items = itemsOf(p);
    CHECK_EQ(items.size(), size_t(kReplayBucketCount * 5 + 2));                     // nine rows of -10 -1 +1 +10 Play, plus Reset all and Close
    CHECK(byCaption(items, "Reset all") != nullptr); CHECK(byCaption(items, "Close") != nullptr);
    // layout: everything inside the popup, rows in HUD order from the top, buttons of a row side by side without touching
    float W = 569.f, H = 320.f;
    for (auto& i : items) CHECK(i.x - i.w / 2 >= 0.f && i.x + i.w / 2 <= W && i.y - i.h / 2 >= 0.f && i.y + i.h / 2 <= H);
    static char const* caps[5] = {"-10", "-1", "+1", "+10", "Play"};
    float prevY = H, titleBand = H * 0.80f;
    for (int row = 0; row < kReplayBucketCount; row++) {
        Item* prev = nullptr;
        for (int what = 0; what < 5; what++) {
            Item* it = byTag(items, row * 10 + what);
            CHECK(it != nullptr);
            if (!it) continue;
            CHECK_EQ(it->caption, std::string(caps[what]));
            CHECK(near(it->y, byTag(items, row * 10)->y));                          // one row = one height
            if (prev) CHECK(it->x - prev->x >= (it->w + prev->w) / 2 + 2.f);        // left to right, not touching
            prev = it;
        }
        float y = byTag(items, row * 10)->y;
        CHECK(y < prevY - 20.f);                                                    // every row is lower than the one above, 20+ units apart
        if (row == 0) CHECK(y <= titleBand + 0.01f);                                // the first row is below the title and the two text lines
        CHECK(byTag(items, row * 10)->h <= 23.1f);                                  // a button is not higher than the row
        prevY = y;
    }
    auto* reset = byCaption(items, "Reset all"); auto* close = byCaption(items, "Close");
    CHECK(reset->y < byTag(items, 8 * 10)->y - 20.f && close->y < byTag(items, 8 * 10)->y - 20.f);   // the bottom buttons are below the last row
    CHECK(reset->x + reset->w / 2 < close->x - close->w / 2);

    // the values shown at the start: the defaults, in white
    ccColor3B col{};
    float rowY[kReplayBucketCount];
    for (int i = 0; i < kReplayBucketCount; i++) rowY[i] = byTag(items, i * 10)->y;
    static char const* want[kReplayBucketCount] = {"200%", "180%", "160%", "140%", "120%", "100%", "80%", "60%", "40%"};
    for (int i = 0; i < kReplayBucketCount; i++) {
        CHECK_EQ(valueOf(p, rowY[i], &col), std::string(want[i]));
        CHECK(col.r == 255 && col.g == 255 && col.b == 255);
    }
    // every label is shown in the colour of its bucket
    for (int i = 0; i < kReplayBucketCount; i++) {
        bool found = false;
        for (auto& [node, z] : p->stubMain()->m_kids)
            if (auto* l = dynamic_cast<CCLabelBMFont*>(node); l && near(l->getPositionY(), rowY[i]) && l->m_text == replayBucketLabel(ReplayBucket(i))) {
                found = true;
                uint32_t c = replayBucketColor(ReplayBucket(i));
                CHECK(l->m_col.r == uint8_t(c >> 16) && l->m_col.g == uint8_t(c >> 8) && l->m_col.b == uint8_t(c));
            }
        CHECK(found);
    }

    // the buttons of the row "4" (row 5): +10, -1, +1, -10
    int R = 5;
    byTag(items, R * 10 + 3)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 110); CHECK_EQ(valueOf(p, rowY[R], &col), std::string("110%"));
    CHECK(col.r == 255 && col.g == 230 && col.b == 120);                              // a changed value is yellow
    byTag(items, R * 10 + 1)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 109); CHECK_EQ(valueOf(p, rowY[R]), std::string("109%"));
    byTag(items, R * 10 + 2)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 110);
    byTag(items, R * 10 + 0)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 100);
    CHECK_EQ(valueOf(p, rowY[R], &col), std::string("100%")); CHECK(col.r == 255 && col.g == 255 && col.b == 255);   // back to the default: white again
    CHECK_EQ(G::ringPitchPercent(ReplayBucket::W3), 80); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W5_6), 120);       // the other rows did not move
    // the ends of the range: 10 and 400
    for (int k = 0; k < 40; k++) byTag(items, R * 10 + 0)->it->activate();            // -10 forty times: far below the bottom
    CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 10); CHECK_EQ(valueOf(p, rowY[R]), std::string("10%"));
    byTag(items, R * 10 + 1)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 10);
    for (int k = 0; k < 45; k++) byTag(items, R * 10 + 3)->it->activate();            // +10 forty-five times: far above the top
    CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 400); CHECK_EQ(valueOf(p, rowY[R]), std::string("400%"));
    byTag(items, R * 10 + 2)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 400);
    // the top row and the bottom row
    byTag(items, 0 * 10 + 3)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W25P), 210); CHECK_EQ(valueOf(p, rowY[0]), std::string("210%"));
    byTag(items, 8 * 10 + 0)->it->activate(); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W1), 30); CHECK_EQ(valueOf(p, rowY[8]), std::string("30%"));

    // Play: plays that label at its pitch, also when the ring sound is switched off in the settings
    played().clear();
    Mod::stubBools()["ring-sound"] = false;
    byTag(items, R * 10 + 4)->it->activate();
    CHECK_EQ(played().size(), size_t(1));
    if (!played().empty()) CHECK(near(played()[0].speed, 4.f));                        // row "4" is at 400 now
    byTag(items, 8 * 10 + 4)->it->activate();
    CHECK_EQ(played().size(), size_t(2));
    if (played().size() == 2) CHECK(near(played()[1].speed, .3f));                     // row "1" is at 30
    Mod::stubBools()["ring-sound"] = true;

    // Reset all asks first: No changes nothing, Yes puts all nine back
    stubQuickCallback() = nullptr;
    byCaption(items, "Reset all")->it->activate();
    CHECK(bool(stubQuickCallback()));
    if (stubQuickCallback()) {
        auto ask = stubQuickCallback();
        ask(nullptr, false);
        CHECK_EQ(G::ringPitchPercent(ReplayBucket::W4), 400); CHECK_EQ(G::ringPitchPercent(ReplayBucket::W25P), 210);   // cancelled: nothing changed
        ask(nullptr, true);
        for (int i = 0; i < kReplayBucketCount; i++) {
            CHECK_EQ(G::ringPitchPercent(ReplayBucket(i)), defaultRingPitchPercent(ReplayBucket(i)));
            CHECK_EQ(valueOf(p, rowY[i], &col), std::string(want[i]));                 // the popup shows the defaults again
            CHECK(col.r == 255 && col.g == 255 && col.b == 255);
        }
    }
    close->it->activate();                                                             // Close does not crash
}

// ---- 5. the files ------------------------------------------------------------------------------------------------------------------------
struct PngInfo { bool ok = false; unsigned w = 0, h = 0; int colourType = -1; };
static PngInfo pngInfo(char const* path) {
    PngInfo r;
    std::ifstream f(path, std::ios::binary);
    unsigned char b[26] = {};
    f.read(reinterpret_cast<char*>(b), sizeof b);
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (!f || std::memcmp(b, sig, 8) != 0 || std::memcmp(b + 12, "IHDR", 4) != 0) return r;
    r.ok = true;
    r.w = (unsigned(b[16]) << 24) | (unsigned(b[17]) << 16) | (unsigned(b[18]) << 8) | b[19];
    r.h = (unsigned(b[20]) << 24) | (unsigned(b[21]) << 16) | (unsigned(b[22]) << 8) | b[23];
    r.colourType = b[25];
    return r;
}
static void testFiles() {
    auto logo = pngInfo("logo.png");
    CHECK(logo.ok); CHECK_EQ(logo.w, 336u); CHECK_EQ(logo.h, 336u);                     // what Geode wants for a mod logo
    auto ring = pngInfo("resources/ring-button.png");
    CHECK(ring.ok); CHECK_EQ(ring.w, 256u); CHECK_EQ(ring.h, 256u);
    CHECK_EQ(ring.colourType, 6);                                                         // RGBA: transparent around the ring
    std::ifstream wav("resources/classic.wav", std::ios::binary);
    char h[12] = {};
    wav.read(h, sizeof h);
    CHECK(wav && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0);   // the user's sound, a WAV file
}

int main() {
    testRingFollowsLevel();
    testRingFollowSettingOff();
    testRingLife();
    testFeedReplay();
    testAnyRingIsInvisible();
    testHudCounters();
    testRingSoundPitches();
    testRingSoundSettings();
    testSavedPitchApi();
    testRingIcon();
    testHudHasPitchRing();
    testPitchPopup();
    testFiles();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
