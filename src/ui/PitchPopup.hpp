// The ring pitch popup and the blue ring that opens it (v0.3.1). All positions are fractions of the window size, like the rest of the UI.
#pragma once
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

#include <array>

#include "core/Metrics.hpp"

namespace gdfwr::ui {

// The blue ring (resources/ring-button.png) as the picture of a button, `width` units wide. If the file cannot be loaded, a ring drawn with
// polygons takes its place, so the button is never invisible. The picture sits in a square node of exactly that size: that is the touch
// area of the button.
cocos2d::CCNode* makeRingIcon(float width);
// The same ring with a caption on its right: ONE picture for ONE button (the panel's "Ring pitch").
cocos2d::CCNode* makeRingCaptionButtonSprite(float height, char const* caption);

// Pitch of the ring sound, one row per label in HUD order (25+ on top ... 1): the label in its colour, the value in percent of the normal
// speed of the sound, the buttons -10 -1 +1 +10 and Play (which always plays, even when the ring sound is off in the settings).
// The values are saved at once (game/RingSound). Reset all asks first.
class PitchPopup : public geode::Popup {
public:
    static PitchPopup* create();
protected:
    bool init();
    void onBtn(cocos2d::CCObject* sender);
    void onResetAll(cocos2d::CCObject*);
    void refresh();
private:
    std::array<cocos2d::CCLabelBMFont*, gdfwr::kReplayBucketCount> m_value{};
    cocos2d::CCLabelBMFont* m_status = nullptr;
};

// Opens the popup (from the panel and from the ring above the replay counters).
void openPitchPopup();

}  // namespace gdfwr::ui
