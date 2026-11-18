// Game hooks. They only FEED the core (SessionModel) with facts; all decisions live in core/.
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <cmath>

#include "game/Bridge.hpp"
#include "game/Jobs.hpp"
#include "ui/Overlays.hpp"
#include "ui/Panel.hpp"

using namespace geode::prelude;
namespace G = gdfwr::game;

static bool isThePlayLayer(GJBaseGameLayer* l) {
    auto* pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == l;
}

// ---- input + tick: Part 1, 2.3 / 2.4 --------------------------------------------------------------------------------------
class $modify(GdfwrBaseLayer, GJBaseGameLayer) {
    // The effective button change that the GAME applies to a player (never a device event): Part 1, 2.3.
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& S = G::State::get();
        if (S.sim.active || button != gdfwr::kJump || !S.session.capturing() || !isThePlayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        bool before = G::holding(p);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        bool after = G::holding(p);
        if (before != after) S.session.onButton(isPlayer1 ? 0 : 1, after, G::tickOf(this), "other");  // source: not known at game level
    }

    // One call = one physics tick. The tick counter comes from the game's own level time (never a counter of ours).
    // GD 2.2081: the game passes isHalfTick / isLastTick; the hook just forwards them untouched.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& S = G::State::get();
        if (S.sim.active || !isThePlayLayer(this)) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        int tick = G::tickOf(this);
        double now = m_gameState.m_levelTime;
        if (S.replay.active) G::feedReplay(PlayLayer::get(), tick);
        if (S.session.capturing() && !S.justReset) {
            // the counter must go up by exactly 1 per call, and one tick must last 1/240 s (Part 1, 2.4)
            if (S.lastTick >= 0 && tick != S.lastTick + 1) S.session.flagSegment(gdfwr::SegFlag::UnreliableTick);
            // TPS watch. Only whole ticks are compared (a half tick has half the length), time warp is allowed for, and one odd step
            // (a lag spike, a pause, a checkpoint) is not enough: the step must be wrong kTpsConfirmSteps times in a row.
            if (!isHalfTick) {
                if (S.tpsLastTick >= 0 && tick == S.tpsLastTick + 1) {
                    double delta = now - S.tpsLastTime;
                    double tw = m_gameState.m_timeWarp;
                    if (gdfwr::tickStepLooksNormal(delta, tw)) {
                        S.tpsOffSteps = 0;
                    } else if (++S.tpsOffSteps >= G::kTpsConfirmSteps && !S.tpsWarned) {
                        S.tpsWarned = true;
                        log::warn("GDFWR: the step length looked wrong {} times in a row (delta {} s, time warp {}): marking TPS_CHANGED", S.tpsOffSteps, delta, tw);
                        S.session.onTpsChange(tick, 240.f, float(1.0 / std::max(1e-6, delta)));
                    }
                } else {
                    S.tpsOffSteps = 0;
                }
                S.tpsLastTick = tick;
                S.tpsLastTime = now;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        S.lastTick = tick;
        S.lastLevelTime = now;
        S.justReset = false;
        if (S.session.capturing()) S.session.checkHold(G::holdState(this, S.session.impliedHold()));
    }
};

// ---- level flow: Part 1, 2.2 / 2.5 -----------------------------------------------------------------------------------------
class $modify(GdfwrPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (m_uiLayer) m_uiLayer->addChild(gdfwr::ui::RecHud::create(), 100);
    }

    // Called after a death, a restart and a checkpoint load. The segment tree is built here (Part 1, 2.2).
    void resetLevel() {
        auto& S = G::State::get();
        if (S.sim.active) { PlayLayer::resetLevel(); return; }
        int before = G::tickOf(this);
        if (S.replay.active) {
            G::stopReplay(("Replay stopped at tick " + std::to_string(before) + ": the player died or the level was restarted. The macro does not pass here.").c_str());
        }
        if (S.session.unfinished()) S.session.noteFps(S.fpsAvg, S.fpsMin);
        PlayLayer::resetLevel();
        int after = G::tickOf(this);
        S.fpsMin = 1000.f;
        S.justReset = true;
        S.lastTick = -1;
        S.lastLevelTime = -1;
        S.tpsLastTick = -1;
        S.tpsOffSteps = 0;
        if (S.pendingStart) {  // the restart that Record triggered: segment 0 starts here
            S.pendingStart = false;
            G::beginSession(this);
            return;
        }
        if (S.session.unfinished()) {
            size_t n = m_checkpointArray ? m_checkpointArray->count() : 0;
            S.session.onReset(before, after, true, G::holdState(this, {0, 0}), n);
        }
    }

    // The game reaches the end of the level HERE (checkForEnd / end trigger): it starts the end animation, and only some REAL
    // time later does it call levelComplete(). Our simulation runs far faster than real time, so it would never see that call
    // and every verification would end with "did not reach the end of the level". In our own run the START of the end
    // animation is the end of the level: remember it and skip the animation (no end screen, no timed action left behind,
    // nothing written to the save data).
    void playEndAnimationToPos(CCPoint position) {
        auto& S = G::State::get();
        if (S.sim.active) { S.sim.done = true; return; }
        PlayLayer::playEndAnimationToPos(position);
    }

    void levelComplete() {
        auto& S = G::State::get();
        if (S.sim.active) { S.sim.done = true; return; }  // verification: no end screen, nothing written to the save data
        if (S.replay.active) G::stopReplay("Replay finished.");
        if (S.session.unfinished()) {
            S.session.complete(G::tickOf(this));
            PlayLayer::levelComplete();
            G::finishRecording(this, gdfwr::EndReason::Completion);
            return;
        }
        PlayLayer::levelComplete();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto& S = G::State::get();
        if (S.sim.active) {  // our own run: remember who died, skip the death animation
            S.sim.dead = true;
            S.sim.diedMask |= (player == m_player2 && player != m_player1) ? 2 : 1;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
        if (S.session.capturing() && player && player->m_isDead) S.session.onDeath(G::tickOf(this));
    }

    // Practice-mode checkpoints are the bookmarks of the recording (Part 1, 2.2).
    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        auto& S = G::State::get();
        if (S.sim.active || !S.session.unfinished()) return;
        S.session.onCheckpointCreated(reinterpret_cast<uint64_t>(cp), G::tickOf(this), G::holdState(this, S.session.impliedHold()),
                                      m_player1->getPositionX(), m_player1->getPositionY());
    }
    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        auto& S = G::State::get();
        if (!S.sim.active && S.session.unfinished()) S.session.onCheckpointRemoved(first);
    }
    void removeAllCheckpoints() {
        PlayLayer::removeAllCheckpoints();
        auto& S = G::State::get();
        if (!S.sim.active && S.session.unfinished()) S.session.onAllCheckpointsRemoved();
    }

    void pauseGame(bool b) {
        auto& S = G::State::get();
        if (!S.sim.active && S.session.capturing()) S.session.onGamePause(G::tickOf(this));
        PlayLayer::pauseGame(b);
    }
    void resume() {
        PlayLayer::resume();
        auto& S = G::State::get();
        if (S.sim.active || !S.session.capturing()) return;
        S.session.onGameResume(G::tickOf(this), G::holdState(this, S.session.impliedHold()));  // pause fix (Part 1, 2.5)
        S.justReset = true;
    }

    // Leaving the level ends the session: it becomes a PARTIAL recording and is saved (Part 1, 4.1).
    void onQuit() {
        auto& S = G::State::get();
        if (!S.sim.active) {
            G::stopReplay(nullptr);
            G::finishRecording(this, gdfwr::EndReason::Exit);
        }
        PlayLayer::onQuit();
    }
};

class $modify(GdfwrPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto btn = CCMenuItemSpriteExtra::create(ButtonSprite::create("GDFWR", "bigFont.fnt", "GJ_button_01.png", .6f), this,
                                                 menu_selector(GdfwrPause::onGdfwr));
        if (auto* menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto win = CCDirector::get()->getWinSize();
            auto m = CCMenu::create();
            m->setPosition({win.width - 50.f, win.height * 0.5f});
            m->addChild(btn);
            this->addChild(m);
        }
    }
    void onGdfwr(CCObject*) { gdfwr::ui::openPanel(); }

    // Part 1, 4.1: leaving while a recording is unfinished asks first and the level stays loaded until the user decides.
    void onQuit(CCObject* sender) {
        auto& S = G::State::get();
        if (!S.sim.active && S.session.unfinished()) {
            createQuickPopup("Recording not finished", "The recording wasn't finished.\nStop recording or continue back?", "Continue back",
                             "Stop recording", [this, sender](FLAlertLayer*, bool stop) {
                                 if (!stop) { this->onResume(nullptr); return; }
                                 G::finishRecording(PlayLayer::get(), gdfwr::EndReason::Exit);
                                 PauseLayer::onQuit(sender);
                             });
            return;
        }
        PauseLayer::onQuit(sender);
    }
};
