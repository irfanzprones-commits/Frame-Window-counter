// SYNTAX-CHECK STUB. Mirrors only the part of the Geode / GD 2.2081 API that the GDFWR glue code uses, to catch typos and
// type errors in OUR code. It is NOT the real SDK and proves nothing about binding names.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace cocos2d {
struct CCPoint { float x = 0, y = 0; CCPoint() = default; CCPoint(float a, float b) : x(a), y(b) {} };
struct CCSize { float width = 0, height = 0; CCSize() = default; CCSize(float a, float b) : width(a), height(b) {} };
struct ccColor3B { uint8_t r, g, b; };
struct ccColor4B { uint8_t r, g, b, a; };
struct ccColor4F { float r, g, b, a; };
class CCObject { public: virtual ~CCObject() = default; CCObject* autorelease() { return this; } void retain() {} void release() {} };
using SEL_MenuHandler = void (CCObject::*)(CCObject*);
#define menu_selector(f) (static_cast<cocos2d::SEL_MenuHandler>(reinterpret_cast<void (cocos2d::CCObject::*)(cocos2d::CCObject*)>(&f)))
#define schedule_selector(f) (&f)
class CCNode : public CCObject {
public:
    virtual bool init() { return true; }
    virtual void update(float) {}
    void scheduleUpdate() {}
    template <class F> void schedule(F, float = 0) {}
    template <class F> void unschedule(F) {}
    // A tiny scene graph (v0.3.1), so tests/hud_test.cpp can look at what the glue code built: the children with their z order, and the parent.
    std::vector<std::pair<CCNode*, int>> m_kids;
    CCNode* m_parent = nullptr;
    CCNode* getParent() const { return m_parent; }
    void addChild(CCNode* c, int z = 0) { if (!c) return; m_kids.push_back({c, z}); c->m_parent = this; }
    void removeFromParent() {
        if (!m_parent) return;
        auto& k = m_parent->m_kids;
        k.erase(std::remove_if(k.begin(), k.end(), [this](auto const& p) { return p.first == this; }), k.end());
        m_parent = nullptr;
    }
    float m_px = 0, m_py = 0;                  // stored so that tests/verify_sim_test.cpp can build a fake game
    void setPosition(CCPoint p) { m_px = p.x; m_py = p.y; }
    void setPositionX(float v) { m_px = v; }
    void setPositionY(float v) { m_py = v; }
    CCPoint getPosition() { return {m_px, m_py}; }
    // The stub's "camera": where the origin of this node is in the world. Tests move it to scroll the level (hud_test.cpp). The real ones: cocos2d-x CCNode.
    float m_wx = 0, m_wy = 0;
    CCPoint convertToWorldSpace(CCPoint const& p) { return {p.x + m_wx, p.y + m_wy}; }
    CCPoint convertToNodeSpace(CCPoint const& p) { return {p.x - m_wx, p.y - m_wy}; }
    float getPositionX() { return m_px; }
    float getPositionY() { return m_py; }
    float getRotation() { return 0; }
    float m_scale = 1;
    void setScale(float s) { m_scale = s; }
    float getScale() { return m_scale; }
    CCPoint m_anchor{0.f, 0.f};
    void setAnchorPoint(CCPoint a) { m_anchor = a; }
    int m_tag = 0;
    void setTag(int t) { m_tag = t; }
    int getTag() { return m_tag; }
    CCSize m_cs;
    CCSize getContentSize() { return m_cs; }
    void setContentSize(CCSize s) { m_cs = s; }
    CCNode* getChildByID(char const*) { return nullptr; }
    template <class T> T* getChildByType(int) { return nullptr; }
    void updateLayout() {}
    void setID(std::string const&) {}
    // geode::Anchor (declared below): Center, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left. The position is the anchor point of
    // THIS node's box plus the offset, as in the real Geode layout, so a test can check that everything lies inside the popup.
    template <class A> void addChildAtPosition(CCNode* c, A anchor, CCPoint off = CCPoint(), bool = true) {
        static const float fx[] = {.5f, 0.f, .5f, 1.f, 1.f, 1.f, .5f, 0.f, 0.f}, fy[] = {.5f, 1.f, 1.f, 1.f, .5f, 0.f, 0.f, 0.f, .5f};
        int a = static_cast<int>(anchor);
        this->addChild(c);
        if (c) c->setPosition(CCPoint(m_cs.width * fx[a] + off.x, m_cs.height * fy[a] + off.y));
    }
    static CCNode* create() { return new CCNode; }
};
class CCLayer : public CCNode {};
class CCLayerColor : public CCLayer { public: static CCLayerColor* create(ccColor4B, float, float) { return new CCLayerColor; } };
class CCSprite : public CCNode {
public:
    static bool& stubMissingFile() { static bool b = false; return b; }   // tests: the picture file is not there (the real create() then returns nullptr)
    static CCSprite* create(char const*) { if (stubMissingFile()) return nullptr; auto* s = new CCSprite; s->m_cs = CCSize(256, 256); return s; }
};
class CCMenu : public CCLayer { public: static CCMenu* create() { return new CCMenu; } };
class CCLabelBMFont : public CCSprite {
public:
    std::string m_text;
    static CCLabelBMFont* create(char const* t, char const*) { auto* l = new CCLabelBMFont; l->setString(t); return l; }
    void setString(char const* t) { m_text = t; m_cs = CCSize(28.f * float(m_text.size()), 40.f); }   // about the size of bigFont at scale 1
    void setColor(ccColor3B c) { m_col = c; }
    void setOpacity(uint8_t) {}
    ccColor3B m_col{255, 255, 255};
};
class CCDrawNode : public CCNode {
public:
    static CCDrawNode* create() { return new CCDrawNode; }
    void drawPolygon(CCPoint*, unsigned, ccColor4F const&, float, ccColor4F const&) {}
};
class CCMenuItem : public CCNode {};
class CCMenuItemSprite : public CCMenuItem { public: void setNormalImage(CCNode*) {} };
class CCArray { public: unsigned count() { return 0; } };
class CCScheduler { public: bool isTargetPaused(CCObject*) { return false; } void pauseTarget(CCObject*) {} void resumeTarget(CCObject*) {} };
class CCScene : public CCNode {};
class CCDirector { public: static CCDirector* get() { static CCDirector d; return &d; } CCSize getWinSize() { return {569, 320}; } CCScene* getRunningScene() { return nullptr; } CCScheduler* getScheduler() { static CCScheduler s; return &s; } };
}  // namespace cocos2d

namespace geode {
enum class Anchor { Center, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
template <class T> struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(T* x) : p(x) {}
    Ref& operator=(T* x) { p = x; return *this; }
    T* operator->() const { return p; }
    T* data() const { return p; }
    explicit operator bool() const { return p; }
};
template <class T> struct Result {
    bool isErr() const { return false; }
    T unwrap() { return {}; }
};
struct Keybind {};
enum class NotificationIcon { Success, Error };
class Notification { public: static Notification* create(std::string const&, NotificationIcon) { return new Notification; } void show() {} };
class Mod {
public:
    static Mod* get() { static Mod m; return &m; }
    // Tests can change a number setting through stubInts(); the defaults are the ones of mod.json.
    static std::map<std::string, int64_t>& stubInts() {
        static std::map<std::string, int64_t> m{{"max-checkpoints", 3000}, {"search-reach", 12}, {"repair-max-shift", 10}, {"full-scan-top", 0}};
        return m;
    }
    static std::map<std::string, bool>& stubBools() {   // v0.3.0: the defaults of mod.json (a setting that is not listed is false)
        static std::map<std::string, bool> m{{"count-untrusted-recording", true}, {"fast-converge", true}, {"ring-follow-level", true}, {"ring-sound", true}};
        return m;
    }
    static std::map<std::string, double>& stubDoubles() {
        static std::map<std::string, double> m{{"ring-hold", 1.0}, {"ring-fade", 2.0}, {"ring-sound-volume", 1.0}};
        return m;
    }
    // v0.3.1: saved values (Mod::getSavedValue / setSavedValue). The stub keeps whole numbers only, which is all the mod stores.
    static std::map<std::string, int64_t>& stubSaved() { static std::map<std::string, int64_t> m; return m; }
    template <class T> T getSavedValue(std::string_view key, T const& def) {
        auto it = stubSaved().find(std::string(key));
        return it != stubSaved().end() ? T(it->second) : def;
    }
    template <class T> T getSavedValue(std::string_view key) { return getSavedValue<T>(key, T()); }
    template <class T> T setSavedValue(std::string_view key, T const& v) {   // the real one returns the old value as well
        T old = getSavedValue<T>(key);
        stubSaved()[std::string(key)] = int64_t(v);
        return old;
    }
    template <class T> T getSettingValue(std::string const& key) {
        if constexpr (std::is_same_v<T, int64_t>) { auto it = stubInts().find(key); return it != stubInts().end() ? it->second : 3000; }
        else if constexpr (std::is_same_v<T, bool>) { auto it = stubBools().find(key); return it != stubBools().end() ? it->second : false; }
        else if constexpr (std::is_same_v<T, double>) { auto it = stubDoubles().find(key); return it != stubDoubles().end() ? it->second : 0.0; }
        else return T{};
    }
    std::filesystem::path getSaveDir() { return {}; }
};
class Loader {
public:
    static Loader* get() { static Loader l; return &l; }
    std::string getGameVersion() { return "2.2081"; }
    bool isModLoaded(std::string const&) { return false; }
};
template <class C> void listenForKeybindSettingPresses(std::string, C&&) {}
namespace file {
enum class PickMode { OpenFile, SaveFile };
struct FilePickOptions {
    struct Filter { std::string description; std::unordered_set<std::string> files; };
    std::optional<std::filesystem::path> defaultPath;
    std::vector<Filter> filters;
};
struct PickFuture {};
inline PickFuture pick(PickMode, FilePickOptions const&) { return {}; }
}  // namespace file
namespace async { template <class F, class C> void spawn(F, C&&) {} }
namespace log {
template <class... A> void debug(char const*, A&&...) {}
template <class... A> void info(char const*, A&&...) {}
template <class... A> void warn(char const*, A&&...) {}
template <class... A> void error(char const*, A&&...) {}
}  // namespace log
}  // namespace geode

// ---- Geometry Dash classes (members used by the glue) ----
struct gd { };
class FLAlertLayer : public cocos2d::CCNode {
public:
    static FLAlertLayer* create(char const*, std::string, char const*) { return new FLAlertLayer; }
    static FLAlertLayer*& stubShown() { static FLAlertLayer* p = nullptr; return p; }   // tests: the layer show() was called on last
    void show() { stubShown() = this; }
};
// tests: the confirm that was asked last. Calling it with true / false is the tap on its right / left button.
inline std::function<void(FLAlertLayer*, bool)>& stubQuickCallback() { static std::function<void(FLAlertLayer*, bool)> f; return f; }
inline void createQuickPopup(char const*, std::string const&, char const*, char const*, std::function<void(FLAlertLayer*, bool)> cb) { stubQuickCallback() = std::move(cb); }
namespace geode {
class Popup : public FLAlertLayer {
public:
    bool init(float w, float h) {   // the real one builds the background, the title and these two layers; here they exist so a test can run the popup code
        m_mainLayer = new cocos2d::CCLayer; m_mainLayer->setContentSize({w, h});
        m_buttonMenu = new cocos2d::CCMenu; m_buttonMenu->setContentSize({w, h});
        stubLast() = this;
        return true;
    }
    void setTitle(std::string const& t) { m_title = t; }
    virtual void onClose(cocos2d::CCObject*) {}
    // Tests only (the real Popup keeps the two layers protected): the popup that was built last, and what it holds.
    static Popup*& stubLast() { static Popup* p = nullptr; return p; }
    cocos2d::CCLayer* stubMain() { return m_mainLayer; }
    cocos2d::CCMenu* stubMenu() { return m_buttonMenu; }
    std::string m_title;
protected:
    cocos2d::CCLayer* m_mainLayer = nullptr;
    cocos2d::CCMenu* m_buttonMenu = nullptr;
};
class ScrollLayer : public cocos2d::CCLayer {
public:
    static ScrollLayer* create(cocos2d::CCSize) { auto* s = new ScrollLayer; s->m_contentLayer = new cocos2d::CCLayer; return s; }
    void moveToTop() {}
    cocos2d::CCLayer* m_contentLayer = nullptr;
};
}  // namespace geode
class ButtonSprite : public cocos2d::CCSprite {
public:
    std::string m_caption;   // tests find a button by its caption
    static ButtonSprite* create(char const* c) { auto* b = new ButtonSprite; b->m_caption = c; b->m_cs = cocos2d::CCSize(60.f, 30.f); return b; }
    static ButtonSprite* create(char const* c, char const*, char const*, float) { return create(c); }
};
enum class CircleBaseColor { Green, Pink, Blue, Cyan, Gray };
enum class CircleBaseSize { Tiny, Small, Medium, MediumAlt, Big };
class CircleButtonSprite : public cocos2d::CCSprite {
public:
    static CircleButtonSprite* create(cocos2d::CCNode*, CircleBaseColor = CircleBaseColor::Green, CircleBaseSize = CircleBaseSize::Medium) { return new CircleButtonSprite; }
};
inline std::string operator""_spr(char const* s, size_t n) { return "clirfan.gdfwr/" + std::string(s, n); }
class CCMenuItemSpriteExtra : public cocos2d::CCMenuItemSprite {
public:
    cocos2d::CCObject* m_target = nullptr;
    cocos2d::SEL_MenuHandler m_sel = nullptr;
    static CCMenuItemSpriteExtra* create(cocos2d::CCNode* spr, cocos2d::CCObject* t, cocos2d::SEL_MenuHandler s) {
        auto* b = new CCMenuItemSpriteExtra;
        b->m_target = t; b->m_sel = s;
        if (spr) { b->addChild(spr); b->m_cs = spr->getContentSize(); }   // like the real one: the picture is a child, the button is as big as the picture
        return b;
    }
    void activate() { if (m_target && m_sel) (m_target->*m_sel)(this); }   // what a tap on the button does (tests)
};
// FMOD (v0.3.1): the stub remembers every effect that was played, so tests/hud_test.cpp can check the pitch and the volume.
class FMODAudioEngine {
public:
    struct Played { std::string path; float speed, pan, volume; };
    static std::vector<Played>& stubPlayed() { static std::vector<Played> v; return v; }
    static FMODAudioEngine* sharedEngine() { static FMODAudioEngine e; return &e; }
    int playEffect(std::string path, float speed, float pan, float volume) { stubPlayed().push_back({path, speed, pan, volume}); return 0; }
};
enum class PlayerButton { Jump = 1, Left = 2, Right = 3 };
struct SeedValueRSV { int value() const { return 0; } };
struct GJGameLevel {
    std::string m_levelString, m_levelName;
    SeedValueRSV m_levelID;
    int m_levelVersion = 0;
    bool isPlatformer() { return false; }
};
struct GameObject : cocos2d::CCNode { int m_objectID = 0; };
struct PlayerObject : cocos2d::CCNode {
    std::map<int, bool> m_holdingButtons;  // 2.2081: a map keyed by PlayerButton, not an array
    double m_yVelocity = 0;
    float m_playerSpeed = 0, m_gravityMod = 1;
    bool m_isUpsideDown = false, m_isSideways = false, m_isShip = false, m_isBird = false, m_isBall = false, m_isDart = false, m_isRobot = false,
        m_isSpider = false, m_isSwing = false, m_isOnGround = false, m_isDashing = false, m_isGoingLeft = false, m_isDead = false;   // initialised: the state hash reads them
};
struct CheckpointObject : cocos2d::CCObject {};
struct GJGameState { double m_levelTime = 0; float m_timeWarp = 1; bool m_isDualMode = false; };
struct LevelSettingsObject { bool m_twoPlayerMode = false, m_startDual = false; };
struct EndPortalObject : GameObject {};
namespace geode { template <class T, class U> T typeinfo_cast(U* p) { return dynamic_cast<T>(p); } }
template <class T> struct CCArrayExtImpl { std::vector<T> v; auto begin() { return v.begin(); } auto end() { return v.end(); } };
template <class T> CCArrayExtImpl<T> CCArrayExt(cocos2d::CCArray*) { return {}; }
class GJBaseGameLayer : public cocos2d::CCLayer {
public:
    PlayerObject* m_player1 = nullptr; PlayerObject* m_player2 = nullptr;
    GJGameState m_gameState; LevelSettingsObject* m_levelSettings = nullptr;
    cocos2d::CCArray* m_objects = nullptr; cocos2d::CCNode* m_objectLayer = nullptr;
    bool m_isPracticeMode = false; cocos2d::CCArray* m_checkpointArray = nullptr;
    GJGameLevel* m_level = nullptr; cocos2d::CCNode* m_uiLayer = nullptr;
    bool m_isPlatformer = false;
    bool m_levelEndAnimationStarted = false;   // 2.2081 GJBaseGameLayer
    EndPortalObject* m_endPortal = nullptr;    // 2.2081 GJBaseGameLayer
    virtual void handleButton(bool, int, bool) {}
    virtual void processCommands(float, bool, bool) {}  // 2.2081: (dt, isHalfTick, isLastTick)
    bool m_started = false;                    // 2.2081 GJBaseGameLayer
    void update(float) override {}             // 2.2081 GJBaseGameLayer::update
};
class PlayLayer : public GJBaseGameLayer {
public:
    static PlayLayer*& current() { static PlayLayer* p = nullptr; return p; }   // tests/verify_sim_test.cpp sets this
    static PlayLayer* get() { return current(); }
    virtual void setupHasCompleted() {}
    virtual void resetLevel() {}
    virtual void resetLevelFromStart() {}
    virtual void levelComplete() {}
    virtual void destroyPlayer(PlayerObject*, GameObject*) {}
    virtual void storeCheckpoint(CheckpointObject*) {}
    virtual void removeCheckpoint(bool) {}
    virtual void removeAllCheckpoints() {}
    virtual void pauseGame(bool) {}
    virtual void resume() {}
    virtual void onQuit() {}
    virtual void checkForEnd() {}
    bool m_isPaused = false;                   // 2.2081 PlayLayer
    GameObject* m_anticheatSpike = nullptr;    // OPTIONAL binding: Bridge.cpp isAnticheatSpike compiles with and without it (see tests/verify_sim.sh)
    virtual void startGame() {}
    void playEndAnimationToPos(cocos2d::CCPoint) {}
    void togglePracticeMode(bool) {}
    virtual CheckpointObject* createCheckpoint() { return nullptr; }
    virtual void loadFromCheckpoint(CheckpointObject*) {}
};
class PauseLayer : public cocos2d::CCLayer {
public:
    virtual void customSetup() {}
    virtual void onQuit(cocos2d::CCObject*) {}
    void onResume(cocos2d::CCObject*) {}
};
#define $modify(Name, Base) Name : public Base
#define $on_game(x) static void on_game_##x()
namespace geode { namespace prelude {
using namespace cocos2d;
using namespace geode;
} }
