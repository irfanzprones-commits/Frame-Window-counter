// Small overlays and result views. All positions are fractions of the window size, so every element scales with the
// screen (Part 10: "if the GD screen becomes smaller, every box, button and element becomes smaller too").
#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>

#include <functional>

#include "core/Analysis.hpp"
#include "core/Metrics.hpp"

namespace gdfwr::ui {

// Recording HUD (Part 1, section 6). Never takes touches, so it can never swallow a game click (section 7).
class RecHud : public cocos2d::CCNode {
public:
    static RecHud* create();
    bool init() override;
    void update(float dt) override;
private:
    cocos2d::CCLabelBMFont* m_label = nullptr;
};

// Replay HUD (Part 9): bucket counters on the LEFT, precision readout BOTTOM LEFT, a ring at every action.
class ReplayHud : public cocos2d::CCNode {
public:
    static ReplayHud* create(int levelTicks);
    bool init(int levelTicks);
    void onAction(PlayLayer* pl, gdfwr::ReplayRing const& ring, cocos2d::CCPoint where);
    void clear();
    void update(float dt) override;
private:
    void refreshText();
    std::array<cocos2d::CCLabelBMFont*, gdfwr::kReplayBucketCount> m_lines{};
    std::array<int, gdfwr::kReplayBucketCount> m_count{};
    std::vector<std::pair<geode::Ref<cocos2d::CCNode>, float>> m_rings;
    std::vector<double> m_windows;   // continuous widths (ticks) of the counted actions
    cocos2d::CCLabelBMFont* m_precision = nullptr;
    int m_levelTicks = 0;
};

// Progress and analysis overlay (Part 5, steps 1 and 4-5). Drives a job by calling pump() every frame.
class ProgressPopup : public geode::Popup {
public:
    struct Hooks {
        std::function<bool(double)> pump;      // runs for ~ms; true when the job is finished
        std::function<std::string()> status;   // text under the bar
        std::function<float()> progress;       // 0..1
        std::function<void()> cancel;          // the user pressed Cancel
        std::function<void()> done;            // finished OR cancelled (called once)
    };
    static ProgressPopup* create(std::string const& title, Hooks h);
protected:
    bool init(std::string const& title, Hooks h);
    void tick(float dt);
    void onClose(cocos2d::CCObject*) override;
private:
    void finish();
    Hooks m_h;
    cocos2d::CCLabelBMFont* m_info = nullptr;
    cocos2d::CCLayerColor* m_bar = nullptr;
    float m_barW = 0, m_elapsed = 0;
    bool m_finished = false;
};

// Result view (Part 5, step 6): summary on top, one row per click below.
class ResultPopup : public geode::Popup {
public:
    static ResultPopup* create(gdfwr::GdfwrFile const& f);
protected:
    bool init(gdfwr::GdfwrFile const& f);
};

std::string windowText(gdfwr::WindowResult const& w);  // "14 (5/8)" , "16+ (3/12+)" , "N/A"
std::string flagsText(uint32_t flags);

}  // namespace gdfwr::ui
