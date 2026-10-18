// Input Buffer — a Geode mod for Geometry Dash 2.2081 (Windows)
//
// Every time you press or release the jump button in a classic level, the mod
// quickly simulates what would happen next. If your input as-is leads to a
// death, it tries moving that input up to N physics steps later (you clicked
// early) or up to N steps earlier (you clicked late) and uses the closest
// timing that survives. N is the "Buffer steps" setting (1 step = 1/240 s).
//
// If you hit something before clicking, the player is held at the point of
// contact for as long as a late click could still save the run. If no click
// could, you die immediately, exactly like vanilla.
//
// Predictions use invisible "ghost" PlayerObjects stepped against the current
// level. Late clicks are also replayed on a ghost and the result is copied onto
// the real player, so the correction happens inside one physics step and never
// shows on screen.
//
// Works with Click Between Frames: clicks that pass as-is keep CBF's exact
// timing; only clicks that need correcting are snapped to whole steps.
//
// Built-in safe mode: while the buffer is active in a level, nothing is saved
// (no completions, no new-best percentages) and a small "IB" tag is shown.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>

#include <deque>
#include <unordered_map>
#include <algorithm>

using namespace geode::prelude;

namespace ib {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

constexpr int kMaxSteps = 16;

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

static void setSteps(int v) {
    Mod::get()->setSettingValue<int64_t>("steps", std::clamp(v, 0, kMaxSteps));
}
static void setEnabled(bool v) {
    Mod::get()->setSettingValue<bool>("enabled", v);
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
    bool valid = false;
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
    s.valid = true;
}

static void apply(PlayerObject* p, PlayerSnap const& s) {
    if (!s.valid) return;
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
    bool injecting = false;       // we are calling handleButton ourselves
    bool killing = false;         // we are letting a held-back death through
    bool assisted = false;        // the buffer was active in this level session

    float playerDt = 0.25f;       // full-step dt PlayerObject::update gets
    float dtAccum = 0.f;          // sums CBF's sub-steps into a full step

    bool realHeld = false;        // jump currently held in the real game

    std::deque<HistEntry> history;
    std::deque<ActivationLog> activations;
    std::vector<PendingInput> incoming;  // inputs waiting for a step boundary
    std::vector<PendingInput> scheduled; // delayed inputs (early clicks)
    int delayOffset = 0;
    int lastEventStep = -1000000;

    int lastPressStep = -1;
    float avgTap = 6.f;           // average tap length in steps

    // held-back death
    bool grace = false;
    bool graceEvaluated = false;
    int graceStart = 0;           // step in which the hit happened
    int graceLatestTarget = 0;    // latest step a late click could still go to
    PlayerObject* gracePlayer = nullptr;
    GameObject* graceKiller = nullptr;
    PlayerSnap deathP1, deathP2;  // pose at the moment of contact

    // simulation
    bool simActive = false;
    bool simDied = false;
    bool simStopped = false;      // ghost reached a gamemode portal etc.
    int simExcludeFrom = 0;       // real activations at >= this step "never happened"
    std::unordered_map<EnhancedGameObject*, int> simActivated; // obj -> ghost 1/2

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
        realHeld = false;
        grace = false;
        graceEvaluated = false;
        gracePlayer = nullptr;
        graceKiller = nullptr;
        deathP1 = {};
        deathP2 = {};
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

static bool midStep() {
    return g.inProcessCommands && !g.inQueuedButtons;
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
// Ghost simulation
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

struct SimOptions {
    PlayerSnap const* base1 = nullptr; // nullptr = real player's current state
    PlayerSnap const* base2 = nullptr;
    std::vector<SimInput> inputs;
    int total = 0;
    int excludeFrom = 0;   // real activations at/after this step count as not happened
    int landFrom = -1;     // >= 0: succeed once the ghost has jumped and landed again
    std::deque<HistEntry>* record = nullptr; // replay: record per-step ghost states
    int recordFrom = 0;                      // step number of the first record
};

// Runs ghosts forward. Returns true if no ghost dies.
// The real players are captured before and restored afterwards, so nothing a
// ghost touches (speed portals, pads, ...) can leak into the actual run.
static bool simulate(PlayLayer* pl, SimOptions const& o) {
    bool dual = dualActive(pl);

    PlayerSnap real1, real2;
    capture(pl->m_player1, real1);
    if (pl->m_player2) capture(pl->m_player2, real2);

    if (o.base1) apply(g.ghost1, *o.base1);
    else apply(g.ghost1, real1);
    if (dual) {
        if (o.base2) apply(g.ghost2, *o.base2);
        else apply(g.ghost2, real2);
    }

    g.simActive = true;
    g.simDied = false;
    g.simStopped = false;
    g.simExcludeFrom = o.excludeFrom;
    g.simActivated.clear();

    float dt = g.playerDt;
    bool survived = true;
    int airborneSteps = 0;
    for (int i = 0; i < o.total; ++i) {
        for (auto const& in : o.inputs) {
            if (in.at == i) {
                pressOn(g.ghost1, in.down);
                if (dual) pressOn(g.ghost2, in.down);
            }
        }
        if (o.record) {
            HistEntry e;
            e.step = o.recordFrom + i;
            capture(g.ghost1, e.p1);
            if (dual) capture(g.ghost2, e.p2);
            else e.p2 = real2;
            o.record->push_back(std::move(e));
        }
        stepPlayer(pl, g.ghost1, dt);
        if (dual && !g.simDied && !g.simStopped) stepPlayer(pl, g.ghost2, dt);
        if (g.simDied) { survived = false; break; }
        if (g.simStopped) break;

        if (o.landFrom >= 0 && i >= o.landFrom) {
            if (!g.ghost1->m_isOnGround) airborneSteps++;
            else if (airborneSteps >= 4) break; // jump finished safely
        }
    }

    g.simActive = false;
    g.ghost1->setVisible(false);
    g.ghost2->setVisible(false);

    apply(pl->m_player1, real1);
    if (pl->m_player2) apply(pl->m_player2, real2);
    return survived;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

static void trimHistory() {
    size_t keep = static_cast<size_t>(kMaxSteps) + 4;
    while (g.history.size() > keep) g.history.pop_front();
}

static void pushHistory(PlayLayer* pl, int step) {
    HistEntry e;
    e.step = step;
    capture(pl->m_player1, e.p1);
    if (pl->m_player2) capture(pl->m_player2, e.p2);
    g.history.push_back(std::move(e));
    trimHistory();
}

static HistEntry const* historyAt(int step) {
    for (auto it = g.history.rbegin(); it != g.history.rend(); ++it) {
        if (it->step == step) return &*it;
        if (it->step < step) break;
    }
    return nullptr;
}

static void trimActivations() {
    int oldest = g.step - (kMaxSteps + 8);
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
    g.realHeld = down;

    if (down) {
        g.lastPressStep = g.step;
    } else if (g.lastPressStep >= 0) {
        int tap = std::clamp(g.step - g.lastPressStep, 1, 30);
        g.avgTap = g.avgTap * 0.75f + tap * 0.25f;
        g.lastPressStep = -1;
    }
}

static void clearGrace() {
    g.grace = false;
    g.graceEvaluated = false;
    g.gracePlayer = nullptr;
    g.graceKiller = nullptr;
}

// Late click: replay the last `k` steps on the ghosts with the input applied
// `k` steps ago, then copy the result onto the real player. Happens inside one
// physics step, so nothing is ever drawn going backwards.
static bool rewindAndApply(PlayLayer* pl, int k, PendingInput const& ev) {
    int now = g.step;
    int target = now - k;
    auto base = historyAt(target);
    if (!base) return false;
    HistEntry baseCopy = *base;
    bool dual = dualActive(pl);

    std::deque<HistEntry> replayHist;
    SimOptions o;
    o.base1 = &baseCopy.p1;
    o.base2 = &baseCopy.p2;
    o.inputs = { { 0, ev.down } };
    o.total = k;
    o.excludeFrom = target;
    o.record = &replayHist;
    o.recordFrom = target;
    simulate(pl, o);
    if (g.simStopped || g.simDied) return false; // can't replay safely

    // Final ghost state = where the player would be now with the on-time click.
    PlayerSnap final1, final2;
    capture(g.ghost1, final1);
    if (dual) capture(g.ghost2, final2);
    auto ghostTouched = g.simActivated;

    // Let GD register the click (counters, held state), then overwrite the
    // player with the corrected state.
    inject(pl, ev.down, ev.isPlayer1);
    final1.m_isSecondPlayer = pl->m_player1->m_isSecondPlayer;
    apply(pl->m_player1, final1);
    if (dual) {
        final2.m_isSecondPlayer = pl->m_player2->m_isSecondPlayer;
        apply(pl->m_player2, final2);
    }

    // Objects: undo what the late timeline used, mark what the corrected one used.
    for (auto const& a : g.activations) {
        if (a.step >= target && a.obj) {
            if (a.player == 1) a.obj->m_activatedByPlayer1 = false;
            else a.obj->m_activatedByPlayer2 = false;
        }
    }
    std::erase_if(g.activations, [&](ActivationLog const& a) { return a.step >= target; });
    for (auto const& [obj, idx] : ghostTouched) {
        if (idx == 2) obj->m_activatedByPlayer2 = true;
        else obj->m_activatedByPlayer1 = true;
        g.activations.push_back({ now - 1, obj, idx });
    }

    // History now follows the corrected timeline.
    while (!g.history.empty() && g.history.back().step >= target) g.history.pop_back();
    for (auto& e : replayHist) {
        if (!dual) e.p2 = baseCopy.p2;
        g.history.push_back(std::move(e));
    }
    trimHistory();

    clearGrace();
    g.lastEventStep = target;
    return true;
}

static int horizonFor(PlayLayer* pl) {
    int n = cfg.steps;
    return isHoldMode(pl->m_player1) ? (2 * n + 8) : std::max(cfg.lookahead, 2 * n + 8);
}

static std::vector<SimInput> inputsFor(PlayLayer* pl, bool down, int at) {
    std::vector<SimInput> v { { at, down } };
    if (down && !isHoldMode(pl->m_player1)) {
        int tap = std::clamp(static_cast<int>(g.avgTap + 0.5f), 1, 12);
        v.push_back({ at + tap, false });
    }
    return v;
}

static int landFor(PlayLayer* pl, int at) {
    return isHoldMode(pl->m_player1) ? -1 : at;
}

// Would this input be fine exactly when it happened?
static bool passesAsIs(PlayLayer* pl, PendingInput const& ev) {
    if (!ev.down && !isHoldMode(pl->m_player1)) return true; // releases don't matter in tap modes
    SimOptions o;
    o.inputs = inputsFor(pl, ev.down, 0);
    o.total = horizonFor(pl);
    o.excludeFrom = g.step;
    o.landFrom = landFor(pl, 0);
    return simulate(pl, o);
}

// Decide what to do with an input at the current step boundary.
static void decide(PlayLayer* pl, PendingInput ev) {
    int now = g.step;
    int n = cfg.steps;
    const char* what = ev.down ? "press" : "release";

    // An earlier input is still waiting (it was delayed); keep the order and
    // the hold length intact by delaying this one by the same amount.
    if (!g.grace && !g.scheduled.empty()) {
        g.scheduled.push_back({ now + g.delayOffset, ev.down, ev.isPlayer1 });
        dbg("[IB] step {}: {} queued behind delayed input (+{})", now, what, g.delayOffset);
        return;
    }

    if (!ev.down && !isHoldMode(pl->m_player1)) {
        inject(pl, ev.down, ev.isPlayer1);
        return;
    }

    int horizon = horizonFor(pl);

    if (!g.grace) {
        // 1. Does the input work as-is?
        if (passesAsIs(pl, ev)) {
            inject(pl, ev.down, ev.isPlayer1);
            return;
        }
    }

    // 2. Nearest shift first: +1 (later), -1 (earlier), +2, -2, ...
    for (int d = 1; d <= n; ++d) {
        if (!g.grace) {
            SimOptions later;
            later.inputs = inputsFor(pl, ev.down, d);
            later.total = d + horizon;
            later.excludeFrom = now;
            later.landFrom = landFor(pl, d);
            if (simulate(pl, later)) {
                g.scheduled.push_back({ now + d, ev.down, ev.isPlayer1 });
                g.delayOffset = d;
                dbg("[IB] step {}: {} early -> delayed {} step(s)", now, what, d);
                return;
            }
        }
        int target = now - d;
        if (target <= g.lastEventStep) continue;
        if (g.grace && target > g.graceStart) continue; // must be before the hit
        auto h = historyAt(target);
        if (!h) continue;
        SimOptions earlier;
        earlier.base1 = &h->p1;
        earlier.base2 = &h->p2;
        earlier.inputs = inputsFor(pl, ev.down, 0);
        earlier.total = d + horizon;
        earlier.excludeFrom = target;
        earlier.landFrom = landFor(pl, 0);
        if (simulate(pl, earlier) && rewindAndApply(pl, d, ev)) {
            dbg("[IB] step {}: {} late -> moved back {} step(s)", now, what, d);
            return;
        }
    }

    // 3. Nothing in range helps: play it as it was.
    dbg("[IB] step {}: {} left as-is (no timing within {} steps passes)", now, what, n);
    inject(pl, ev.down, ev.isPlayer1);
}

static void drainIncoming(PlayLayer* pl) {
    if (g.incoming.empty()) return;
    auto list = std::move(g.incoming);
    g.incoming.clear();
    for (auto const& p : list) {
        if (pl->m_player1->m_isDead) break;
        decide(pl, p);
    }
}

// ---------------------------------------------------------------------------
// Held-back deaths
// ---------------------------------------------------------------------------

static void freezeAtContact(PlayLayer* pl) {
    apply(pl->m_player1, g.deathP1);
    if (pl->m_player2 && g.deathP2.valid) apply(pl->m_player2, g.deathP2);
}

static void killHeldDeath(PlayLayer* pl) {
    auto p = g.gracePlayer;
    auto k = g.graceKiller;
    freezeAtContact(pl);
    clearGrace();
    if (!p) return;
    dbg("[IB] step {}: death goes through", g.step);
    g.killing = true;
    pl->destroyPlayer(p, k);
    g.killing = false;
}

// Right after a hit: could any late click within range still have avoided it?
// If not, die now, at the point of contact, like vanilla.
static void evaluateHeldDeath(PlayLayer* pl) {
    g.graceEvaluated = true;
    int d = g.graceStart;
    int n = cfg.steps;

    // What would a late input be? A press if not holding; a release if
    // holding in ship / wave / robot. Holding in a tap mode: nothing helps.
    bool hold = isHoldMode(pl->m_player1);
    std::optional<bool> rescue;
    if (!g.realHeld) rescue = true;
    else if (hold) rescue = false;

    if (rescue && g.scheduled.empty()) {
        int horizon = horizonFor(pl);
        // Latest possible target first: a click arriving now goes to d + 1 - k.
        for (int t = d; t >= d + 1 - n; --t) {
            if (t <= g.lastEventStep) break;
            auto h = historyAt(t);
            if (!h) continue;
            SimOptions o;
            o.base1 = &h->p1;
            o.base2 = &h->p2;
            o.inputs = inputsFor(pl, *rescue, 0);
            o.total = (d - t) + horizon;
            o.excludeFrom = t;
            o.landFrom = landFor(pl, 0);
            if (simulate(pl, o)) {
                g.graceLatestTarget = t;
                dbg("[IB] step {}: hit, a click within {} step(s) could still save it", d, t - (d + 1 - n) + 1);
                return;
            }
        }
    }
    killHeldDeath(pl);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static void updateLabel(PlayLayer* pl) {
    if (!g.label) return;
    bool on = cfg.enabled && cfg.steps > 0 && !pl->m_isPlatformer;
    std::string text = on ? fmt::format("IB {}", cfg.steps) : "IB off";
    g.label->setVisible(on || g.assisted);
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

class BufferPopup : public Popup {
protected:
    CCLabelBMFont* m_stepsLabel = nullptr;
    CCLabelBMFont* m_msLabel = nullptr;
    CCMenuItemToggler* m_toggle = nullptr;

    bool init() {
        if (!Popup::init(260.f, 190.f)) return false;
        this->setTitle("Input Buffer");

        auto size = m_mainLayer->getContentSize();

        // enabled toggle
        m_toggle = CCMenuItemExt::createTogglerWithStandardSprites(0.75f, [this](CCMenuItemToggler* t) {
            setEnabled(!t->isToggled());
            this->refresh();
        });
        m_toggle->toggle(cfg.enabled);
        m_buttonMenu->addChildAtPosition(m_toggle, Anchor::Top, { -45.f, -55.f });
        auto enLabel = CCLabelBMFont::create("Enabled", "bigFont.fnt");
        enLabel->setScale(0.5f);
        enLabel->setAnchorPoint({ 0.f, 0.5f });
        m_mainLayer->addChildAtPosition(enLabel, Anchor::Top, { -25.f, -55.f });

        // steps - / value / +
        auto makeArrow = [&](bool right, int delta) {
            auto spr = CCSprite::createWithSpriteFrameName("navArrowBtn_001.png");
            spr->setScale(0.55f);
            if (!right) spr->setFlipX(true);
            return CCMenuItemExt::createSpriteExtra(spr, [this, delta](auto) {
                setSteps(cfg.steps + delta);
                this->refresh();
            });
        };
        m_buttonMenu->addChildAtPosition(makeArrow(false, -1), Anchor::Center, { -75.f, -5.f });
        m_buttonMenu->addChildAtPosition(makeArrow(true, 1), Anchor::Center, { 75.f, -5.f });

        m_stepsLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_mainLayer->addChildAtPosition(m_stepsLabel, Anchor::Center, { 0.f, 0.f });
        m_msLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_msLabel->setScale(0.5f);
        m_mainLayer->addChildAtPosition(m_msLabel, Anchor::Center, { 0.f, -22.f });

        auto note = CCLabelBMFont::create("Safe mode: nothing is saved while the buffer is used.", "chatFont.fnt");
        note->setScale(0.55f);
        note->setOpacity(180);
        m_mainLayer->addChildAtPosition(note, Anchor::Bottom, { 0.f, 22.f });

        (void)size;
        this->refresh();
        return true;
    }

    void refresh() {
        if (m_stepsLabel) {
            m_stepsLabel->setString(fmt::format("{} step{}", cfg.steps, cfg.steps == 1 ? "" : "s").c_str());
            m_stepsLabel->setScale(0.7f);
            m_stepsLabel->setOpacity(cfg.enabled ? 255 : 120);
        }
        if (m_msLabel) {
            m_msLabel->setString(fmt::format("+/- {:.1f} ms", cfg.steps * 1000.0 / 240.0).c_str());
        }
        if (g.layer) updateLabel(g.layer);
    }

public:
    static BufferPopup* create() {
        auto ret = new BufferPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

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
        if (down && !repeat) setEnabled(!cfg.enabled);
    });
    listenForKeybindSettingPresses("key-steps-up", [](Keybind const&, bool down, bool, double) {
        if (down) setSteps(cfg.steps + 1);
    });
    listenForKeybindSettingPresses("key-steps-down", [](Keybind const&, bool down, bool, double) {
        if (down) setSteps(cfg.steps - 1);
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

        auto label = CCLabelBMFont::create("IB", "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 1.f });
        label->setScale(0.25f);
        label->setOpacity(110);
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({ 4.f, win.height - 4.f });
        label->setID("input-buffer-tag"_spr);
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

        // Hit something. Remember the pose at contact; whether a late click
        // could still save this is checked right after the current step.
        if (!g.grace) {
            g.grace = true;
            g.graceEvaluated = false;
            g.graceStart = g.step;
            g.gracePlayer = player;
            g.graceKiller = object;
            capture(m_player1, g.deathP1);
            if (m_player2) capture(m_player2, g.deathP2);
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
        PendingInput ev { g.step, down, isPlayer1 };

        if (midStep()) {
            // Click Between Frames feeds clicks in partway through a step.
            // If the click is fine as-is, let it through right now so CBF's
            // exact timing is kept. Otherwise correct it at the next step.
            bool clear = g.incoming.empty() && g.scheduled.empty() && !g.grace;
            if (clear && passesAsIs(pl, ev)) {
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                g.lastEventStep = g.step;
                g.realHeld = down;
                if (down) g.lastPressStep = g.step;
                return;
            }
            g.incoming.push_back(ev);
            return;
        }

        // Keep inputs strictly in order (fixes quick press+release pairs).
        drainIncoming(pl);
        if (pl->m_player1->m_isDead) return;
        decide(pl, ev);
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

        bool on = active(pl);
        if (on) {
            g.assisted = true;

            if (g.grace) {
                // No click can reach a saving timing any more: die at contact.
                if (g.graceEvaluated && g.step - cfg.steps > g.graceLatestTarget) {
                    killHeldDeath(pl);
                }
            }

            // Delayed (early) inputs that are due now.
            if (!pl->m_player1->m_isDead && !g.scheduled.empty()) {
                std::vector<PendingInput> due;
                std::erase_if(g.scheduled, [&](PendingInput const& p) {
                    if (p.step <= g.step) { due.push_back(p); return true; }
                    return false;
                });
                for (auto const& p : due) {
                    if (g.grace) decide(pl, { g.step, p.down, p.isPlayer1 });
                    else inject(pl, p.down, p.isPlayer1);
                }
            }

            // Inputs that arrived mid-step last time.
            if (!pl->m_player1->m_isDead) drainIncoming(pl);

            if (!pl->m_player1->m_isDead && !g.grace) pushHistory(pl, g.step);
            trimActivations();
        } else if (g.grace) {
            // The buffer got switched off while a death was being held back.
            killHeldDeath(pl);
        }

        g.dtAccum = 0.f;
        g.inProcessCommands = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g.inProcessCommands = false;
        if (g.dtAccum > 0.f) g.playerDt = g.dtAccum;

        if (g.grace && active(pl)) {
            if (!g.graceEvaluated) evaluateHeldDeath(pl);
            if (g.grace) freezeAtContact(pl); // stay at the point of contact
        }
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
            bool touching = false;
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
                    break; // simulate normally (real player is restored afterwards)

                case GameObjectType::Modifier:
                    // Speed portals change *every* player's speed in GD, so
                    // the ghost applies them to itself only.
                    touching = player->getObjectRect().intersectsRect(obj->getObjectRect());
                    if (touching) {
                        switch (obj->m_objectID) {
                            case 200: player->m_playerSpeed = 0.7f; break;
                            case 201: player->m_playerSpeed = 0.9f; break;
                            case 202: player->m_playerSpeed = 1.1f; break;
                            case 203: player->m_playerSpeed = 1.3f; break;
                            case 1334: player->m_playerSpeed = 1.6f; break;
                            default: break;
                        }
                    }
                    disabled.push_back(obj);
                    obj->m_isDisabled = true;
                    obj->m_isDisabled2 = true;
                    break;

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
            g.simActivated[this] = player == g.ghost2.data() ? 2 : 1;
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
        // Sum the real player's update time within one step. With CBF a step
        // is split into several smaller updates; the ghosts need the full step.
        if (!g.simActive && g.inProcessCommands) {
            auto pl = PlayLayer::get();
            if (pl && this == pl->m_player1) g.dtAccum += dt;
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

// Pause menu button that opens the settings popup.
class $modify(IBPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Buffer", "bigFont.fnt", "GJ_button_04.png", 0.6f);
        spr->setScale(0.7f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](auto) {
            if (auto popup = BufferPopup::create()) popup->show();
        });
        btn->setID("input-buffer-button"_spr);

        // Use the right-side menu if the node IDs mod named it; otherwise our own.
        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto win = CCDirector::get()->getWinSize();
            auto ownMenu = CCMenu::create();
            ownMenu->setPosition({ win.width - 45.f, 25.f });
            ownMenu->addChild(btn);
            this->addChild(ownMenu, 10);
        }
    }
};
