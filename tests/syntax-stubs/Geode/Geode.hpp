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
    void addChild(CCNode*, int = 0) {}
    void removeFromParent() {}
    float m_px = 0, m_py = 0;                  // stored so that tests/verify_sim_test.cpp can build a fake game
    void setPosition(CCPoint p) { m_px = p.x; m_py = p.y; }
    void setPositionX(float v) { m_px = v; }
    CCPoint getPosition() { return {m_px, m_py}; }
    float getPositionX() { return m_px; }
    float getPositionY() { return m_py; }
    float getRotation() { return 0; }
    void setScale(float) {}
    void setAnchorPoint(CCPoint) {}
    void setTag(int) {}
    int getTag() { return 0; }
    CCSize getContentSize() { return {}; }
    void setContentSize(CCSize) {}
    CCNode* getChildByID(char const*) { return nullptr; }
    template <class T> T* getChildByType(int) { return nullptr; }
    void updateLayout() {}
    template <class A> void addChildAtPosition(CCNode*, A, CCPoint = CCPoint(), bool = true) {}
    static CCNode* create() { return new CCNode; }
};
class CCLayer : public CCNode {};
class CCLayerColor : public CCLayer { public: static CCLayerColor* create(ccColor4B, float, float) { return new CCLayerColor; } };
class CCSprite : public CCNode {};
class CCMenu : public CCLayer { public: static CCMenu* create() { return new CCMenu; } };
class CCLabelBMFont : public CCSprite {
public:
    static CCLabelBMFont* create(char const*, char const*) { return new CCLabelBMFont; }
    void setString(char const*) {}
    void setColor(ccColor3B) {}
    void setOpacity(uint8_t) {}
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
        static std::map<std::string, int64_t> m{{"max-checkpoints", 3000}, {"search-reach", 12}, {"repair-max-shift", 10}};
        return m;
    }
    template <class T> T getSettingValue(std::string const& key) {
        if constexpr (std::is_same_v<T, int64_t>) { auto it = stubInts().find(key); return it != stubInts().end() ? it->second : 3000; }
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
    void show() {}
};
inline void createQuickPopup(char const*, std::string const&, char const*, char const*, std::function<void(FLAlertLayer*, bool)>) {}
namespace geode {
class Popup : public FLAlertLayer {
public:
    bool init(float, float) { return true; }
    void setTitle(std::string const&) {}
    virtual void onClose(cocos2d::CCObject*) {}
protected:
    cocos2d::CCLayer* m_mainLayer = nullptr;
    cocos2d::CCMenu* m_buttonMenu = nullptr;
};
class ScrollLayer : public cocos2d::CCLayer {
public:
    static ScrollLayer* create(cocos2d::CCSize) { return new ScrollLayer; }
    void moveToTop() {}
    cocos2d::CCLayer* m_contentLayer = nullptr;
};
}  // namespace geode
class ButtonSprite : public cocos2d::CCSprite {
public:
    static ButtonSprite* create(char const*) { return new ButtonSprite; }
    static ButtonSprite* create(char const*, char const*, char const*, float) { return new ButtonSprite; }
};
class CCMenuItemSpriteExtra : public cocos2d::CCMenuItemSprite {
public:
    static CCMenuItemSpriteExtra* create(cocos2d::CCNode*, cocos2d::CCObject*, cocos2d::SEL_MenuHandler) { return new CCMenuItemSpriteExtra; }
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
