// The ring sound (v0.3.1): resources/classic.wav plays when a ring with a measured window appears in the replay, at the pitch the user set for
// that label. Everything that touches the sound engine or the saved pitches is in this one file pair.
#pragma once
#include "core/Metrics.hpp"

namespace gdfwr::game {

// The pitch of a label in percent of the normal speed of the sound: the saved value (pitch popup), or the default when none was saved, and
// always inside 10..400 (a hand-edited save file cannot push it out of range).
int ringPitchPercent(gdfwr::ReplayBucket b);
void setRingPitch(gdfwr::ReplayBucket b, int percent);          // clamps, then saves
int changeRingPitch(gdfwr::ReplayBucket b, int deltaPercent);   // the -10 / -1 / +1 / +10 buttons: adds, clamps, saves, returns the new value
void resetRingPitches();                                        // every label back to its default

// Plays the sound of one label NOW at the saved pitch and the volume of the setting "ring-sound-volume". It does not look at the setting
// "ring-sound": the replay checks that one itself, and the Play button of the pitch popup always plays.
void playRingSound(gdfwr::ReplayBucket b);
// The ONE call into the game's sound engine. If the binding differs in the real SDK, this is the only place to change (see README, [BINDING]).
void playRingSoundAt(int pitchPercent, float volume);

}  // namespace gdfwr::game
