#include "PitchPopup.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "game/RingSound.hpp"

using namespace geode::prelude;
namespace G = gdfwr::game;

namespace gdfwr::ui {
namespace {

ccColor3B rgb(uint32_t c) { return ccColor3B{uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}; }

// Button tags: row * 10 + what. what 0..3 = the steps, 4 = Play.
constexpr int kSteps[4] = {-10, -1, 1, 10};
constexpr int kPlay = 4;

// The ring drawn with polygons (the picture file could not be loaded): dark outline, cyan band, dark inner outline, light middle. The
// shape is the one of the picture, the node is centred on its own origin.
CCNode* drawnRing(float width) {
    auto d = CCDrawNode::create();
    constexpr int N = 40;
    float R = width * 0.5f;
    auto disc = [&](float radius, ccColor4F c) {
        CCPoint v[N];
        for (int i = 0; i < N; i++) {
            float t = float(i) * 6.2831853f / float(N);
            v[i] = CCPoint(std::cos(t) * radius, std::sin(t) * radius);   // not {..}: CCPoint::operator= is ambiguous with CCSize
        }
        d->drawPolygon(v, N, c, 0.f, c);
    };
    disc(R * 1.00f, ccColor4F{0.02f, 0.05f, 0.14f, 1.f});
    disc(R * 0.85f, ccColor4F{0.00f, 0.91f, 0.98f, 1.f});
    disc(R * 0.57f, ccColor4F{0.02f, 0.05f, 0.14f, 1.f});
    disc(R * 0.45f, ccColor4F{1.00f, 1.00f, 0.94f, 1.f});
    return d;
}

}  // namespace

CCNode* makeRingIcon(float width) {
    auto box = CCNode::create();
    box->setContentSize({width, width});
    box->setAnchorPoint({0.5f, 0.5f});   // a plain node is anchored at its corner; a button picture has to turn and bounce around its middle
    // "_spr": the file of the resources folder, with the mod id in front. Through a std::string, so it compiles whatever type "_spr" returns.
    std::string path("ring-button.png"_spr);
    CCNode* ring = CCSprite::create(path.c_str());
    if (ring) {
        float cw = ring->getContentSize().width;
        if (cw > 0.f) ring->setScale(width / cw);
    } else {
        ring = drawnRing(width);
    }
    ring->setPosition({width * 0.5f, width * 0.5f});
    box->addChild(ring);
    return box;
}

CCNode* makeRingCaptionButtonSprite(float height, char const* caption) {
    float gap = height * 0.2f;
    auto text = CCLabelBMFont::create(caption, "bigFont.fnt");
    text->setScale(height * 0.014f);
    text->setAnchorPoint({0.f, 0.5f});
    float textW = text->getContentSize().width * text->getScale();
    auto box = CCNode::create();
    box->setContentSize({height + gap + textW, height});
    box->setAnchorPoint({0.5f, 0.5f});
    auto ring = makeRingIcon(height);
    ring->setPosition({height * 0.5f, height * 0.5f});
    text->setPosition({height + gap, height * 0.5f});
    box->addChild(ring);
    box->addChild(text);
    return box;
}

PitchPopup* PitchPopup::create() {
    auto r = new PitchPopup();
    if (r->init()) { r->autorelease(); return r; }
    delete r;
    return nullptr;
}

bool PitchPopup::init() {
    auto win = CCDirector::get()->getWinSize();
    float w = win.width, h = win.height;
    if (!Popup::init(w, h)) return false;   // fills the whole screen, like the panel and the click editor
    this->setTitle("Ring pitch");
    auto note = CCLabelBMFont::create("Pitch of the ring sound in percent of normal: lower = deeper and longer, higher = sharper and shorter.", "chatFont.fnt");
    note->setScale(0.5f);
    m_mainLayer->addChildAtPosition(note, Anchor::BottomLeft, {w * 0.5f, h * 0.885f}, false);
    m_status = CCLabelBMFont::create("", "chatFont.fnt");
    m_status->setScale(0.5f);
    m_mainLayer->addChildAtPosition(m_status, Anchor::BottomLeft, {w * 0.5f, h * 0.850f}, false);

    float rowH = h * 0.072f;
    for (int i = 0; i < gdfwr::kReplayBucketCount; i++) {   // HUD order: 25+ on top ... 1 at the bottom
        auto b = gdfwr::ReplayBucket(i);
        float y = h * 0.78f - rowH * float(i);
        auto name = CCLabelBMFont::create(gdfwr::replayBucketLabel(b), "bigFont.fnt");
        name->setScale(0.5f);
        name->setAnchorPoint({1.f, 0.5f});    // right-aligned: the labels all end at the same x
        name->setColor(rgb(gdfwr::replayBucketColor(b)));
        m_mainLayer->addChildAtPosition(name, Anchor::BottomLeft, {w * 0.20f, y}, false);
        m_value[size_t(i)] = CCLabelBMFont::create("", "bigFont.fnt");
        m_value[size_t(i)]->setScale(0.5f);
        m_mainLayer->addChildAtPosition(m_value[size_t(i)], Anchor::BottomLeft, {w * 0.33f, y}, false);
        auto mk = [&](char const* txt, int what, float fx) {
            auto spr = ButtonSprite::create(txt, "bigFont.fnt", "GJ_button_01.png", .5f);
            float bh = spr->getContentSize().height;
            if (bh > 0.f) spr->setScale(rowH * 0.78f / bh);   // as high as the row allows, whatever the height of the button picture is
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(PitchPopup::onBtn));
            btn->setTag(i * 10 + what);
            m_buttonMenu->addChildAtPosition(btn, Anchor::BottomLeft, {w * fx, y});
        };
        mk("-10", 0, 0.45f); mk("-1", 1, 0.53f); mk("+1", 2, 0.61f); mk("+10", 3, 0.69f);
        mk("Play", kPlay, 0.82f);
    }
    auto reset = CCMenuItemSpriteExtra::create(ButtonSprite::create("Reset all", "bigFont.fnt", "GJ_button_01.png", .7f), this, menu_selector(PitchPopup::onResetAll));
    m_buttonMenu->addChildAtPosition(reset, Anchor::Bottom, {-w * 0.12f, h * 0.07f});
    auto close = CCMenuItemSpriteExtra::create(ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_01.png", .8f), this, menu_selector(PitchPopup::onClose));
    m_buttonMenu->addChildAtPosition(close, Anchor::Bottom, {w * 0.12f, h * 0.07f});
    this->refresh();
    return true;
}

void PitchPopup::onBtn(CCObject* sender) {
    int tag = static_cast<CCNode*>(sender)->getTag();
    int row = tag / 10, what = tag % 10;
    if (row < 0 || row >= gdfwr::kReplayBucketCount) return;
    auto b = gdfwr::ReplayBucket(row);
    if (what >= 0 && what < 4) G::changeRingPitch(b, kSteps[what]);
    else if (what == kPlay) G::playRingSound(b);   // always plays: it does not look at the setting "ring-sound"
    this->refresh();
}

void PitchPopup::onResetAll(CCObject*) {
    createQuickPopup("Reset pitch", "Set the pitch of all nine labels back to their defaults?", "Cancel", "Reset", [this](FLAlertLayer*, bool yes) {
        if (!yes) return;
        G::resetRingPitches();
        this->refresh();
    });
}

void PitchPopup::refresh() {
    for (int i = 0; i < gdfwr::kReplayBucketCount; i++) {
        auto b = gdfwr::ReplayBucket(i);
        int v = G::ringPitchPercent(b);
        char t[16];
        std::snprintf(t, sizeof t, "%d%%", v);
        m_value[size_t(i)]->setString(t);
        m_value[size_t(i)]->setColor(v == gdfwr::defaultRingPitchPercent(b) ? ccColor3B{255, 255, 255} : ccColor3B{255, 230, 120});   // yellow = changed
    }
    auto* mod = Mod::get();
    char s[128];
    if (mod->getSettingValue<bool>("ring-sound"))
        std::snprintf(s, sizeof s, "Ring sound is ON, volume %d%% (mod settings). A yellow value was changed.", int(std::lround(mod->getSettingValue<double>("ring-sound-volume") * 100.0)));
    else
        std::snprintf(s, sizeof s, "Ring sound is OFF in the mod settings: the rings stay silent, Play still plays.");
    m_status->setString(s);
}

void openPitchPopup() {
    if (auto* p = PitchPopup::create()) p->show();
}

}  // namespace gdfwr::ui
