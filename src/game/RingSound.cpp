#include "RingSound.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace gdfwr::game {

// [BINDING] Mod::getSavedValue<int>(key, default) / setSavedValue<int>(key, value): whole numbers only, the safest part of the saved-value API.
int ringPitchPercent(gdfwr::ReplayBucket b) {
    int saved = Mod::get()->getSavedValue<int>(gdfwr::ringPitchKey(b), gdfwr::defaultRingPitchPercent(b));
    return gdfwr::clampPitchPercent(saved);
}

void setRingPitch(gdfwr::ReplayBucket b, int percent) {
    Mod::get()->setSavedValue<int>(gdfwr::ringPitchKey(b), gdfwr::clampPitchPercent(percent));
}

int changeRingPitch(gdfwr::ReplayBucket b, int deltaPercent) {
    // the delta is limited first, so the sum cannot overflow whatever is passed in; the buttons only use -10, -1, +1 and +10
    int next = gdfwr::clampPitchPercent(ringPitchPercent(b) + std::clamp(deltaPercent, -1000, 1000));
    setRingPitch(b, next);
    return next;
}

void resetRingPitches() {
    for (int i = 0; i < gdfwr::kReplayBucketCount; i++) {
        auto b = gdfwr::ReplayBucket(i);
        setRingPitch(b, gdfwr::defaultRingPitchPercent(b));
    }
}

void playRingSoundAt(int pitchPercent, float volume) {
    // "_spr" puts the mod id in front, which is how a file of the resources folder is found. The path goes through a std::string, so this
    // compiles whatever type "_spr" returns.
    std::string path("classic.wav"_spr);
    // [BINDING] FMODAudioEngine::playEffect(gd::string path, float speed, float pan, float volume): the second argument is the playback
    // speed (1 = normal; it changes the pitch and the length together), the third the pan (0 = centre). If the real signature is another
    // one, this line is the only one to change.
    FMODAudioEngine::sharedEngine()->playEffect(path, gdfwr::pitchSpeed(pitchPercent), 0.f, std::clamp(volume, 0.f, 1.f));
}

void playRingSound(gdfwr::ReplayBucket b) {
    float volume = float(Mod::get()->getSettingValue<double>("ring-sound-volume"));
    playRingSoundAt(ringPitchPercent(b), volume);
}

}  // namespace gdfwr::game
