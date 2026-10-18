// Input Buffer — a Geode mod for Geometry Dash 2.2081 (Windows)
//
// What it does
// ------------
// Every time you press or release the jump button in a classic level, the mod
// quickly simulates what would happen next. If your input as-is leads to a
// death, it tries moving that input up to N physics steps later (you clicked
// early) or up to N steps earlier (you clicked late) and uses the closest
// timing that survives. N is the "Buffer steps" setting (1 step = 1/240 s).
//
// If you die without having clicked yet, the death is held back for up to N
// steps. If you click inside that window, the mod rewinds the player to the
// last timing that would have passed.
//
// Built-in safe mode: while this mod is active in a level, nothing is saved
// (no completions, no new-best percentages), and an on-screen label is shown.
//
// How the simulation works
// ------------------------
// Prediction uses two invisible "ghost" PlayerObjects that are stepped against
// the current level (solids, hazards, slopes, pads, orbs, gravity / speed /
// size portals). Moving objects are treated as frozen during a prediction,
// which is accurate for the short windows involved. Rewinds only move the
// player(s) back, never the level.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/GJGameLevel.hpp>

#include <deque>
#include <unordered_set>
#include <algorithm>

using namespace geode::prelude;

namespace ib {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct Config {
    bool enabled = true;
    int steps = 4;        // max shift in physics steps, each way
    int lookahead = 90;   // prediction length for tap-style gamemodes
    bool debug = false;
};
static Config cfg;

static void loadConfig() {
    auto mod = Mod::get();
    cfg.enabled = mod->getSettingValue<bool>("enabled");
    cfg.steps = static_cast<int>(mod->getSettingValue<int64_t>("steps"));
    cfg.lookahead = static_cast<int>(mod->getSettingValue<int64_t>("lookahead"));
    cfg.debug = mod->getSettingValue<bool>("debug-log");
}

// ---------------------------------------------------------------------------
// Player state snapshots
// ---------------------------------------------------------------------------

// Physics-relevant PlayerObject members (names checked against the 2.2081
// bindings). Game-mode flags are restored separately through GD's own
// PlayerCheckpoint so the icon / vehicle stays consistent.
#define IB_PLAYER_FIELDS(X)                                                    \
    X(m_wasTeleported) X(m_fixGravityBug) X(m_reverseSync)                     \
    X(m_yVelocityBeforeSlope) X(m_dashX) X(m_dashY) X(m_dashAngle)             \
    X(m_dashStartTime) X(m_dashRing) X(m_slopeStartTime)                       \
    X(m_maybeLastGroundObject) X(m_lastCollisionBottom) X(m_lastCollisionTop)  \
    X(m_lastCollisionLeft) X(m_lastCollisionRight) X(m_unk50C) X(m_unk510)     \
    X(m_currentSlope2) X(m_preLastGroundObject) X(m_slopeAngle)                \
    X(m_slopeSlidingMaybeRotated) X(m_quickCheckpointMode) X(m_collidedObject) \
    X(m_lastGroundObject) X(m_collidingWithLeft) X(m_collidingWithRight)       \
    X(m_scaleXRelated2) X(m_groundYVelocity) X(m_yVelocityRelated)             \
    X(m_scaleXRelated3) X(m_scaleXRelated4)         \
    X(m_scaleXRelated5) X(m_isCollidingWithSlope) X(m_isBallRotating)          \
    X(m_unk669) X(m_currentPotentialSlope) X(m_currentSlope)                   \
    X(m_collidingWithSlopeId) X(m_slopeFlipGravityRelated)                     \
    X(m_slopeAngleRadians) X(m_rotateObjectsRelated) X(m_rotationSpeed)        \
    X(m_rotateSpeed) X(m_isRotating) X(m_isBallRotating2)                      \
    X(m_speedMultiplier) X(m_yStart) X(m_gravity) X(m_gameModeChangedTime)     \
    X(m_padRingRelated) X(m_maybeIsFalling) X(m_isOnGround3)                   \
    X(m_lastJumpTime) X(m_lastFlipTime) X(m_lastSpiderFlipTime)                \
    X(m_accelerationOrSpeed) X(m_snapDistance) X(m_ringJumpRelated)            \
    X(m_ringRelatedSet) X(m_objectSnappedTo) X(m_slopeRotation)                \
    X(m_currentSlopeYVelocity) X(m_unk3d0) X(m_blackOrbRelated) X(m_unk3e0)    \
    X(m_unk3e1) X(m_isAccelerating) X(m_isCurrentSlopeTop)                     \
    X(m_collidedTopMinY) X(m_collidedBottomMaxY) X(m_collidedLeftMaxX)         \
    X(m_collidedRightMinX) X(m_maybeIsColliding) X(m_jumpBuffered)             \
    X(m_stateRingJump) X(m_wasJumpBuffered) X(m_wasRobotJump)                  \
    X(m_stateJumpBuffered) X(m_stateRingJump2) X(m_touchedRing)                \
    X(m_touchedCustomRing) X(m_touchedGravityPortal)                           \
    X(m_maybeTouchedBreakableBlock) X(m_jumpRelatedAC2) X(m_touchedPad)        \
    X(m_yVelocity) X(m_fallSpeed) X(m_isOnSlope) X(m_wasOnSlope)               \
    X(m_slopeVelocity) X(m_maybeUpsideDownSlope) X(m_isOnGround)               \
    X(m_reverseRelated) X(m_maybeReverseSpeed) X(m_maybeReverseAcceleration)   \
    X(m_xVelocityRelated2) X(m_isDashing) X(m_dashFireFrame)                   \
    X(m_groundObjectMaterial) X(m_playerSpeed) X(m_isOnGround2)                \
    X(m_lastLandTime) X(m_platformerVelocityRelated) X(m_maybeIsBoosted)       \
    X(m_scaleXRelatedTime) X(m_decreaseBoostSlide) X(m_unkA29) X(m_isLocked)   \
    X(m_controlsDisabled) X(m_touchedRings) X(m_hasEverJumped)                 \
    X(m_hasEverHitRing) X(m_isSecondPlayer) X(m_totalTime)                     \
    X(m_yVelocityRelated3) X(m_playerFollowFloats) X(m_stateOnGround)          \
    X(m_stateUnk) X(m_stateNoStickX) X(m_stateNoStickY) X(m_stateUnk2)         \
    X(m_stateBoostX) X(m_stateBoostY) X(m_maybeStateForce2) X(m_stateScale)    \
    X(m_platformerXVelocity) X(m_holdingRight) X(m_holdingLeft)                \
    X(m_leftPressedFirst) X(m_scaleXRelated) X(m_maybeHasStopped)              \
    X(m_xVelocityRelated) X(m_maybeGoingCorrectSlopeDirection) X(m_isSliding)  \
    X(m_maybeSlopeForce) X(m_isOnIce) X(m_physDeltaRelated) X(m_isOnGround4)   \
    X(m_maybeSlidingTime) X(m_maybeSlidingStartTime)                           \
    X(m_changedDirectionsTime) X(m_slopeEndTime) X(m_isMoving)                 \
    X(m_stateNoAutoJump) X(m_stateDartSlide) X(m_stateHitHead)                 \
    X(m_stateFlipGravity) X(m_gravityMod) X(m_stateForce)                      \
    X(m_stateForceVector) X(m_affectedByForces) X(m_jumpPadRelated)            \
    X(m_fixRobotJump) X(m_holdingButtons) X(m_inputsLocked) X(m_isOutOfBounds) \
    X(m_fallStartY) X(m_shipRotation) X(m_lastPortalPos) X(m_lastGroundedPos)  \
    X(m_lastActivatedPortal) X(m_position)

struct ModeSig {
    bool ship = false, ufo = false, ball = false, wave = false, robot = false,
         spider = false, swing = false, upsideDown = false, sideways = false,
         goingLeft = false;
    float size = 1.f;
    bool operator==(ModeSig const&) const = default;
};

static ModeSig modeOf(PlayerObject* p) {
    ModeSig m;
    m.ship = p->m_isShip;
    m.ufo = p->m_isBird;
    m.ball = p->m_isBall;
    m.wave = p->m_isDart;
    m.robot = p->m_isRobot;
    m.spider = p->m_isSpider;
    m.swing = p->m_isSwing;
    m.upsideDown = p->m_isUpsideDown;
    m.sideways = p->m_isSideways;
    m.goingLeft = p->m_isGoingLeft;
    m.size = p->m_vehicleSize;
    return m;
}

// Gamemodes where *holding* matters continuously (vs. single taps).
static bool isHoldMode(PlayerObject* p) {
    return (p->m_isShip && !p->m_isBird) || p->m_isDart || p->m_isRobot;
}

struct PlayerSnap {
#define IB_DECL(f) std::remove_cvref_t<decltype(std::declval<PlayerObject&>().f)> f{};
    IB_PLAYER_FIELDS(IB_DECL)
#undef IB_DECL
    CCPoint nodePos;
    float nodeRot = 0.f;
    ModeSig mode;
    Ref<PlayerCheckpoint> checkpoint;
    std::vector<Ref<CCObject>> touchingRings;
};

static void capture(PlayerObject* p, PlayerSnap& s) {
#define IB_GET(f) s.f = p->f;
    IB_PLAYER_FIELDS(IB_GET)
#undef IB_GET
    s.nodePos = p->getPosition();
    s.nodeRot = p->getRotation();
    s.mode = modeOf(p);
    s.checkpoint = PlayerCheckpoint::create();
    p->saveToCheckpoint(s.checkpoint);
    s.touchingRings.clear();
    if (p->m_touchingRings) {
        for (auto obj : CCArrayExt<CCObject*>(p->m_touchingRings)) {
            s.touchingRings.emplace_back(obj);
        }
    }
}

static void apply(PlayerObject* p, PlayerSnap const& s) {
    if (modeOf(p) != s.mode && s.checkpoint) {
        p->loadFromCheckpoint(s.checkpoint);
    }
#define IB_SET(f) p->f = s.f;
    IB_PLAYER_FIELDS(IB_SET)
#undef IB_SET
    p->setPosition(s.nodePos);
    p->setRotation(s.nodeRot);
    if (p->m_touchingRings) {
        p->m_touchingRings->removeAllObjects();
        for (auto const& obj : s.touchingRings) {
            p->m_touchingRings->addObject(obj);
        }
    }
}

// ---------------------------------------------------------------------------
// Runtime state (one PlayLayer at a time)
// ---------------------------------------------------------------------------

struct HistEntry {
    int step = 0;
    PlayerSnap p1;
    PlayerSnap p2;
};

struct ActivationLog {
    int step;
    EnhancedGameObject* obj;
    int player; // 1 or 2
};

struct PendingInput {
    int step;
    bool down;
    bool isPlayer1;
};

struct SimInput {
    int at;     // relative step
    bool down;
};

struct State {
    PlayLayer* layer = nullptr;
    Ref<PlayerObject> ghost1;
    Ref<PlayerObject> ghost2;
    CCLabelBMFont* label = nullptr;

    int step = 0;                 // physics steps since the last reset
    bool inProcessCommands = false;
    bool inQueuedButtons = false;
    bool injecting = false;      // we are calling handleButton ourselves
    bool killing = false;         // we are letting a held-back death through
    bool assisted = false;        // the buffer was active in this level session

    float playerDt = 0.25f;       // dt PlayerObject::update gets per step

    std::deque<HistEntry> history;
    std::deque<ActivationLog> activations;
    std::vector<PendingInput> incoming;  // inputs that arrived mid-step
    std::vector<PendingInput> scheduled; // delayed inputs (early clicks)
    int delayOffset = 0;
    int lastEventStep = -1000000;

    int lastPressStep = -1;
    float avgTap = 6.f;           // average tap length in steps

    // held-back death
    bool grace = false;
    int graceStart = 0;
    PlayerObject* gracePlayer = nullptr;
    GameObject* graceKiller = nullptr;

    // simulation
    bool simActive = false;
    bool simDied = false;
    bool simStopped = false;      // ghost reached a gamemode portal etc.
    int simExcludeFrom = 0;       // real activations at >= this step "never happened"
    std::unordered_set<EnhancedGameObject*> simActivated;

    std::string lastLabel;

    void resetRun() {
        step = 0;
        history.clear();
        activations.clear();
        incoming.clear();
        scheduled.clear();
        delayOffset = 0;
        lastEventStep = -1000000;
        lastPressStep = -1;
        grace = false;
        gracePlayer = nullptr;
        graceKiller = nullptr;
        simActive = false;
        simDied = false;
        simStopped = false;
        simActivated.clear();
    }
};
static State g;

static bool isGhost(PlayerObject* p) {
    return p && (p == g.ghost1.data() || p == g.ghost2.data());
}

static bool dualActive(PlayLayer* pl) {
    return pl->m_gameState.m_isDualMode && pl->m_player2;
}

// Is the buffer allowed to act right now?
static bool active(PlayLayer* pl) {
    if (!pl || pl != g.layer || !cfg.enabled || cfg.steps <= 0) return false;
    if (!g.ghost1 || !g.ghost2) return false;
    if (pl->m_isPlatformer) return false;                    // classic only
    if (pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode) return false;
    if (pl->m_isPaused || pl->m_hasCompletedLevel) return false;
    if (!pl->m_player1 || pl->m_player1->m_isDead) return false;
    return true;
}

template <class... Args>
static void dbg(fmt::format_string<Args...> f, Args&&... args) {
    if (cfg.debug) log::info(f, std::forward<Args>(args)...);
}

// ---------------------------------------------------------------------------
// Player-only stepping (used for ghosts, and for the real player on rewinds)
// ---------------------------------------------------------------------------

static void stepPlayer(PlayLayer* pl, PlayerObject* p, float dt) {
    if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
    if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
    if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
    if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();
    p->update(dt);
    pl->checkCollisions(p, dt, false);
    p->updateRotation(dt);
}

static void pressOn(PlayerObject* p, bool down) {
    if (down) p->pushButton(PlayerButton::Jump);
    else p->releaseButton(PlayerButton::Jump);
}

// Runs a prediction. Returns true if no ghost dies within `total` steps.
//   base1/base2: state to start from (nullptr = the real player's current state)
//   excludeFrom: activations by the real player at or after this step are
//                treated as not having happened yet (for rewind candidates)
//   landFrom:    if >= 0, the prediction ends successfully once the ghost has
//                left the ground after this step and landed again. That way a
//                tap is only judged on its own jump, not on later obstacles
//                that need another click.
static bool simulate(PlayLayer* pl, PlayerSnap const* base1, PlayerSnap const* base2,
                     std::vector<SimInput> const& inputs, int total, int excludeFrom,
                     int landFrom = -1) {
    bool dual = dualActive(pl);
    PlayerSnap tmp;

    if (base1) apply(g.ghost1, *base1);
    else { capture(pl->m_player1, tmp); apply(g.ghost1, tmp); }
    if (dual) {
        if (base2) apply(g.ghost2, *base2);
        else { capture(pl->m_player2, tmp); apply(g.ghost2, tmp); }
    }

    g.simActive = true;
    g.simDied = false;
    g.simStopped = false;
    g.simExcludeFrom = excludeFrom;
    g.simActivated.clear();

    float dt = g.playerDt;
    bool survived = true;
    int airborneSteps = 0;
    for (int i = 0; i < total; ++i) {
        for (auto const& in : inputs) {
            if (in.at == i) {
                pressOn(g.ghost1, in.down);
                if (dual) pressOn(g.ghost2, in.down);
            }
        }
        stepPlayer(pl, g.ghost1, dt);
        if (dual && !g.simDied && !g.simStopped) stepPlayer(pl, g.ghost2, dt);
        if (g.simDied) { survived = false; break; }
        if (g.simStopped) break;

        if (landFrom >= 0 && i >= landFrom) {
            if (!g.ghost1->m_isOnGround) airborneSteps++;
            else if (airborneSteps >= 4) break; // jump finished safely
        }
    }

    g.simActive = false;
    g.simActivated.clear();
    g.ghost1->setVisible(false);
    g.ghost2->setVisible(false);
    return survived;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

static void pushHistory(PlayLayer* pl, int step) {
    HistEntry e;
    e.step = step;
    capture(pl->m_player1, e.p1);
    if (pl->m_player2) capture(pl->m_player2, e.p2);
    g.history.push_back(std::move(e));
    size_t keep = static_cast<size_t>(cfg.steps) + 3;
    while (g.history.size() > keep) g.history.pop_front();
}

static HistEntry const* historyAt(int step) {
    for (auto it = g.history.rbegin(); it != g.history.rend(); ++it) {
        if (it->step == step) return &*it;
        if (it->step < step) break;
    }
    return nullptr;
}

static void trimActivations() {
    int oldest = g.step - (cfg.steps + 8);
    while (!g.activations.empty() && g.activations.front().step < oldest) {
        g.activations.pop_front();
    }
}

// ---------------------------------------------------------------------------
// Applying inputs to the real game
// ---------------------------------------------------------------------------

static void inject(PlayLayer* pl, bool down, bool isPlayer1) {
    g.injecting = true;
    pl->handleButton(down, 1, isPlayer1);
    g.injecting = false;
    g.lastEventStep = g.step;

    if (down) {
        g.lastPressStep = g.step;
    } else if (g.lastPressStep >= 0) {
        int tap = std::clamp(g.step - g.lastPressStep, 1, 30);
        g.avgTap = g.avgTap * 0.75f + tap * 0.25f;
        g.lastPressStep = -1;
    }
}

// Move the real player(s) back `k` steps, apply the input there, then
// re-step the players forward to the present.
static void rewindAndApply(PlayLayer* pl, int k, PendingInput const& ev) {
    int now = g.step;
    int target = now - k;
    auto base = historyAt(target);
    if (!base) return;

    HistEntry baseCopy = *base;
    bool dual = dualActive(pl);

    // Objects (pads, orbs, portals) the real player activated in the window
    // need to be usable again on the replay.
    for (auto const& a : g.activations) {
        if (a.step >= target && a.obj) {
            if (a.player == 1) a.obj->m_activatedByPlayer1 = false;
            else a.obj->m_activatedByPlayer2 = false;
        }
    }
    std::erase_if(g.activations, [&](ActivationLog const& a) { return a.step >= target; });
    while (!g.history.empty() && g.history.back().step >= target) g.history.pop_back();

    apply(pl->m_player1, baseCopy.p1);
    if (dual) apply(pl->m_player2, baseCopy.p2);

    if (g.grace) {
        g.grace = false;
        g.gracePlayer = nullptr;
        g.graceKiller = nullptr;
    }

    float dt = g.playerDt;
    for (int b = target; b < now; ++b) {
        g.step = b; // so activations / history get logged at the right step
        if (b == target) inject(pl, ev.down, ev.isPlayer1);
        pushHistory(pl, b);
        stepPlayer(pl, pl->m_player1, dt);
        if (dual) stepPlayer(pl, pl->m_player2, dt);
    }
    g.step = now;
    g.lastEventStep = target;
}

// Decide what to do with a fresh input at the current step boundary.
static void decide(PlayLayer* pl, PendingInput ev) {
    int now = g.step;
    int n = cfg.steps;

    // An earlier input is still waiting (it was delayed); keep the order and
    // the hold length intact by delaying this one by the same amount.
    if (!g.scheduled.empty()) {
        g.scheduled.push_back({ now + g.delayOffset, ev.down, ev.isPlayer1 });
        dbg("[IB] step {}: {} queued behind delayed input (+{})", now, ev.down ? "press" : "release", g.delayOffset);
        return;
    }

    bool hold = isHoldMode(pl->m_player1);

    // In tap gamemodes the moment of release doesn't matter.
    if (!ev.down && !hold) {
        inject(pl, ev.down, ev.isPlayer1);
        return;
    }

    int horizon = hold ? (2 * n + 8) : std::max(cfg.lookahead, 2 * n + 8);
    int tap = std::clamp(static_cast<int>(g.avgTap + 0.5f), 1, 12);

    auto inputsAt = [&](int at) {
        std::vector<SimInput> v { { at, ev.down } };
        if (ev.down && !hold) v.push_back({ at + tap, false });
        return v;
    };
    // tap gamemodes: judge a click on its own jump (see simulate)
    auto landAt = [&](int at) { return hold ? -1 : at; };

    // 1. Does the input work as-is?
    if (simulate(pl, nullptr, nullptr, inputsAt(0), horizon, now, landAt(0))) {
        inject(pl, ev.down, ev.isPlayer1);
        return;
    }

    // 2. Nearest shift first: +1 (later), -1 (earlier), +2, -2, ...
    for (int d = 1; d <= n; ++d) {
        // later: you clicked early, so hold the input back
        if (simulate(pl, nullptr, nullptr, inputsAt(d), d + horizon, now, landAt(d))) {
            g.scheduled.push_back({ now + d, ev.down, ev.isPlayer1 });
            g.delayOffset = d;
            dbg("[IB] step {}: {} early -> delayed {} step(s)", now, ev.down ? "press" : "release", d);
            return;
        }
        // earlier: you clicked late, so rewind the player
        int target = now - d;
        if (target > g.lastEventStep) {
            if (auto h = historyAt(target)) {
                if (simulate(pl, &h->p1, &h->p2, inputsAt(0), d + horizon, target, landAt(0))) {
                    dbg("[IB] step {}: {} late -> moved back {} step(s)", now, ev.down ? "press" : "release", d);
                    rewindAndApply(pl, d, ev);
                    return;
                }
            }
        }
    }

    // 3. Nothing in range helps: play it as it was.
    dbg("[IB] step {}: {} left as-is (no timing within {} steps passes)", now, ev.down ? "press" : "release", n);
    inject(pl, ev.down, ev.isPlayer1);
}

static void killHeldDeath(PlayLayer* pl) {
    auto p = g.gracePlayer;
    auto k = g.graceKiller;
    g.grace = false;
    g.gracePlayer = nullptr;
    g.graceKiller = nullptr;
    if (!p) return;
    dbg("[IB] step {}: no rescue, death goes through", g.step);
    g.killing = true;
    pl->destroyPlayer(p, k);
    g.killing = false;
}

// ---------------------------------------------------------------------------
// UI label
// ---------------------------------------------------------------------------

static void updateLabel(PlayLayer* pl) {
    if (!g.label) return;
    std::string text;
    bool show = cfg.enabled || g.assisted;
    if (cfg.enabled && cfg.steps > 0 && !pl->m_isPlatformer) {
        text = fmt::format("Input Buffer: {} step{} ({:.1f} ms)", cfg.steps,
                           cfg.steps == 1 ? "" : "s", cfg.steps * 1000.0 / 240.0);
    } else if (cfg.enabled && pl->m_isPlatformer) {
        text = "Input Buffer: off (platformer)";
    } else {
        text = "Input Buffer: off";
    }
    if (g.assisted) text += " - safe mode";
    g.label->setVisible(show);
    if (text != g.lastLabel) {
        g.label->setString(text.c_str());
        g.lastLabel = text;
    }
}

// Run fn with "test mode" on so GD doesn't save progress.
template <class F>
static void withSafeMode(PlayLayer* pl, F&& fn) {
    if (!g.assisted) { fn(); return; }
    bool old = pl->m_isTestMode;
    pl->m_isTestMode = true;
    fn();
    pl->m_isTestMode = old;
}

} // namespace ib

using namespace ib;

// ---------------------------------------------------------------------------
// Settings + keybinds
// ---------------------------------------------------------------------------

$on_mod(Loaded) {
    loadConfig();
    listenForSettingChanges<bool>("enabled", [](bool v) { cfg.enabled = v; });
    listenForSettingChanges<int64_t>("steps", [](int64_t v) { cfg.steps = static_cast<int>(v); });
    listenForSettingChanges<int64_t>("lookahead", [](int64_t v) { cfg.lookahead = static_cast<int>(v); });
    listenForSettingChanges<bool>("debug-log", [](bool v) { cfg.debug = v; });

    listenForKeybindSettingPresses("key-toggle", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) Mod::get()->setSettingValue<bool>("enabled", !cfg.enabled);
    });
    listenForKeybindSettingPresses("key-steps-up", [](Keybind const&, bool down, bool, double) {
        if (down) Mod::get()->setSettingValue<int64_t>("steps", std::min(cfg.steps + 1, 16));
    });
    listenForKeybindSettingPresses("key-steps-down", [](Keybind const&, bool down, bool, double) {
        if (down) Mod::get()->setSettingValue<int64_t>("steps", std::max(cfg.steps - 1, 0));
    });
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

class $modify(IBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g.resetRun();
        g.layer = nullptr;
        g.ghost1 = nullptr;
        g.ghost2 = nullptr;
        g.label = nullptr;
        g.assisted = false;
        g.lastLabel.clear();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g.layer = this;
        return true;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (g.layer != this) return;

        for (int i = 0; i < 2; ++i) {
            auto ghost = PlayerObject::create(1, 1, this, m_objectLayer, true);
            if (!ghost) continue;
            ghost->setVisible(false);
            ghost->m_playEffects = false;
            ghost->m_hasGroundParticles = false;
            ghost->m_hasShipParticles = false;
            ghost->setPosition({ 0, 105 });
            m_objectLayer->addChild(ghost);
            if (i == 0) g.ghost1 = ghost; else g.ghost2 = ghost;
        }

        auto label = CCLabelBMFont::create("Input Buffer", "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 1.f });
        label->setScale(0.35f);
        label->setOpacity(170);
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({ 5.f, win.height - 5.f });
        label->setID("input-buffer-label"_spr);
        this->addChild(label, 1000);
        g.label = label;

        if (cfg.enabled && cfg.steps > 0 && !m_isPlatformer) g.assisted = true;
        updateLabel(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (g.layer == this) g.resetRun();
    }

    void onQuit() {
        if (g.layer == this) {
            g.resetRun();
            g.layer = nullptr;
            g.ghost1 = nullptr;
            g.ghost2 = nullptr;
            g.label = nullptr;
        }
        PlayLayer::onQuit();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (g.layer == this) updateLabel(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Ghost hit something: that's a failed prediction, nothing else.
        if (isGhost(player)) {
            g.simDied = true;
            return;
        }
        if (g.simActive) return;

        // The anticheat spike at the start of every attempt must go through.
        if (object && object == m_anticheatSpike) return PlayLayer::destroyPlayer(player, object);

        if (g.killing || !active(this) || (player != m_player1 && player != m_player2)) {
            withSafeMode(this, [&] { PlayLayer::destroyPlayer(player, object); });
            return;
        }

        // Hold the death back for up to N steps in case a late click arrives.
        if (!g.grace) {
            g.grace = true;
            g.graceStart = g.step;
            g.gracePlayer = player;
            g.graceKiller = object;
            dbg("[IB] step {}: death held back, waiting up to {} step(s) for a click", g.step, cfg.steps);
        }
    }

    void levelComplete() {
        if (g.simActive) return;
        withSafeMode(this, [&] { PlayLayer::levelComplete(); });
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (g.simActive) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playGravityEffect(bool flip) {
        if (g.simActive) return;
        PlayLayer::playGravityEffect(flip);
    }
};

class $modify(IBBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (g.injecting || button != 1 || !pl || static_cast<GJBaseGameLayer*>(pl) != this) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (!active(pl)) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        g.assisted = true;

        if (g.inProcessCommands && !g.inQueuedButtons) {
            // Arrived in the middle of a physics step; handle it at the next one.
            g.incoming.push_back({ g.step, down, isPlayer1 });
            return;
        }
        decide(pl, { g.step, down, isPlayer1 });
    }

    // GD applies queued clicks here, right before a step's physics, so inputs
    // arriving through it are still "on the boundary".
    void processQueuedButtons(float dt, bool clearInputQueue) {
        bool old = g.inQueuedButtons;
        g.inQueuedButtons = true;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        g.inQueuedButtons = old;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || pl != g.layer) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        if (active(pl)) {
            g.assisted = true;

            // Held-back death ran out of time.
            if (g.grace && g.step > g.graceStart + cfg.steps) {
                killHeldDeath(pl);
            }

            // Delayed (early) inputs that are due now.
            if (!pl->m_player1->m_isDead && !g.scheduled.empty()) {
                std::vector<PendingInput> due;
                std::erase_if(g.scheduled, [&](PendingInput const& p) {
                    if (p.step <= g.step) { due.push_back(p); return true; }
                    return false;
                });
                for (auto const& p : due) inject(pl, p.down, p.isPlayer1);
            }

            // Inputs that arrived mid-step.
            if (!pl->m_player1->m_isDead && !g.incoming.empty()) {
                auto list = std::move(g.incoming);
                g.incoming.clear();
                for (auto const& p : list) decide(pl, p);
            }

            if (!pl->m_player1->m_isDead) pushHistory(pl, g.step);
            trimActivations();
        } else if (g.grace) {
            // The buffer got switched off while a death was being held back.
            killHeldDeath(pl);
        }

        g.inProcessCommands = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g.inProcessCommands = false;
        g.step++;
    }

    // While a ghost is being simulated, only let it touch things that matter
    // for survival, and stop the prediction at gamemode-changing portals.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!g.simActive || !isGhost(player) || !objects) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        std::vector<GameObject*> disabled;
        int n = std::min<int>(count, static_cast<int>(objects->size()));
        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj || obj->m_isDisabled || obj->m_isDisabled2) continue;
            switch (obj->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Slope:
                case GameObjectType::YellowJumpPad:
                case GameObjectType::PinkJumpPad:
                case GameObjectType::GravityPad:
                case GameObjectType::RedJumpPad:
                case GameObjectType::SpiderPad:
                case GameObjectType::YellowJumpRing:
                case GameObjectType::PinkJumpRing:
                case GameObjectType::GravityRing:
                case GameObjectType::GreenRing:
                case GameObjectType::DropRing:
                case GameObjectType::RedJumpRing:
                case GameObjectType::CustomRing:
                case GameObjectType::DashRing:
                case GameObjectType::GravityDashRing:
                case GameObjectType::SpiderOrb:
                case GameObjectType::InverseGravityPortal:
                case GameObjectType::NormalGravityPortal:
                case GameObjectType::GravityTogglePortal:
                case GameObjectType::RegularSizePortal:
                case GameObjectType::MiniSizePortal:
                case GameObjectType::Modifier:
                    break; // simulate normally
                case GameObjectType::ShipPortal:
                case GameObjectType::CubePortal:
                case GameObjectType::BallPortal:
                case GameObjectType::UfoPortal:
                case GameObjectType::WavePortal:
                case GameObjectType::RobotPortal:
                case GameObjectType::SpiderPortal:
                case GameObjectType::SwingPortal:
                case GameObjectType::DualPortal:
                case GameObjectType::SoloPortal:
                case GameObjectType::TeleportPortal:
                    // Can't predict past a mode change; count it as "made it".
                    if (player->getObjectRect().intersectsRect(obj->getObjectRect())) {
                        g.simStopped = true;
                    }
                    [[fallthrough]];
                default:
                    disabled.push_back(obj);
                    obj->m_isDisabled = true;
                    obj->m_isDisabled2 = true;
                    break;
            }
        }

        GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);

        for (auto obj : disabled) {
            obj->m_isDisabled = false;
            obj->m_isDisabled2 = false;
        }
    }

    // Side effects the ghosts must not cause.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (g.simActive) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (g.simActive) return;
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (g.simActive) return;
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }
};

class $modify(IBEnhancedObject, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        if (isGhost(player)) {
            g.simActivated.insert(this);
            return;
        }
        EnhancedGameObject::activatedByPlayer(player);
        auto pl = PlayLayer::get();
        if (pl && pl == g.layer && !g.simActive) {
            if (player == pl->m_player1) g.activations.push_back({ g.step, this, 1 });
            else if (player == pl->m_player2) g.activations.push_back({ g.step, this, 2 });
        }
    }

    bool hasBeenActivatedByPlayer(PlayerObject* player) {
        if (!isGhost(player)) return EnhancedGameObject::hasBeenActivatedByPlayer(player);

        if (g.simActivated.contains(this)) return true;
        auto pl = PlayLayer::get();
        if (!pl) return false;
        int idx = player == g.ghost2.data() ? 2 : 1;
        auto real = idx == 2 ? pl->m_player2 : pl->m_player1;
        if (!real) return false;
        if (!EnhancedGameObject::hasBeenActivatedByPlayer(real)) return false;
        // Activated, but after the moment we're predicting from?
        for (auto const& a : g.activations) {
            if (a.obj == this && a.player == idx && a.step >= g.simExcludeFrom) return false;
        }
        return true;
    }
};

class $modify(IBPlayerObject, PlayerObject) {
    void update(float dt) {
        if (!g.simActive) {
            auto pl = PlayLayer::get();
            if (pl && this == pl->m_player1) g.playerDt = dt;
        }
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        if (isGhost(this)) return;
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (isGhost(this)) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(IBGameObject, GameObject) {
    void playShineEffect() {
        if (g.simActive) return;
        GameObject::playShineEffect();
    }
};

class $modify(IBHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (g.simActive) return;
        HardStreak::addPoint(point);
    }
};

// Safe mode: never save a percentage while the buffer was used in this level.
class $modify(IBLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        auto pl = PlayLayer::get();
        if (pl && pl == g.layer && g.assisted) return;
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};
