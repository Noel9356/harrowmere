// controller.cpp — see controller.hpp for the contract (task 3.10).
//
// The Phase-1 slice: drain (Move as motion state, Attack drained for
// Phase 2, Interact routed through the gate + registry, Emote toggled),
// then ONE kinematic integration per fixed update, then the AFK verdict.
// No hidden state: everything the slice touches lives on PlayerController /
// IntentQueue, so 3.1's step-9 registration is a re-registration of this
// entry point with the stores bound — a rewrite of nothing (the seasons
// pattern, DEVIATIONS.md 2026-10-30).

#include "player/controller.hpp"

#include <cmath>

#include "core/constants.hpp" // MOVE_SPEED_MPS / MOVE_SPRINT_FACTOR (A-57), AFK_TIMEOUT_REAL_S (A-58)
#include "core/log.hpp"       // TRACE-only logging — the per-tick hot-path rule

namespace harrowmere::player
{

namespace
{

// One kinematic integration step (D-PHYS-1): position += normalized(dir) ×
// speed × dt. Position math on the f64 docs' struct (R5's carve-out); the
// zero direction is the pinned no-motion fallback (R2's explicit-guard
// pattern — the guard at the only division-shaped site here), so a stale
// zero Move can never produce a NaN. y is cosmetic and stays untouched
// (card 1.6).
void integrateMotion(
    Position &position, const harrowmere::MoveIntent &motion, double dtSeconds
)
{
    const double lengthSquared = motion.dirX * motion.dirX + motion.dirZ * motion.dirZ;
    if (lengthSquared <= 0.0)
        return; // the pinned fallback: no direction, no motion
    const double length = std::sqrt(lengthSquared);
    const double speed  = MOVE_SPEED_MPS * (motion.sprint ? MOVE_SPRINT_FACTOR : 1.0);
    const double step   = speed * dtSeconds / length;
    position.x += motion.dirX * step;
    position.z += motion.dirZ * step;
}

// One Emote{act} toggle: flip the observable act on the action log. Edge-
// triggered by design (one intent = one flip) — the input path owns
// de-duplication (7.4P1's pitfall: mixing polled state with event
// subscriptions double-fires toggles; it produces one intent per press).
// The mask's item-ownership check activates with 2.4's inventory; the
// Phase-1 toggle is free but the act is logged (card 3.10, verbatim).
void applyEmote(
    PlayerActions &log, harrowmere::EmoteIntent::Act act
)
{
    switch (act)
    {
    case harrowmere::EmoteIntent::Act::CROUCH:
        log.crouched = !log.crouched;
        break;
    case harrowmere::EmoteIntent::Act::LIGHT_TORCH:
        log.torchLit = !log.torchLit;
        break;
    case harrowmere::EmoteIntent::Act::MASK:
        log.masked = !log.masked;
        break;
    }
}

} // namespace

void applyIntents(
    PlayerController &player,
    IntentQueue &queue,
    const core::WorldClock &clock,
    double dtSeconds,
    const InteractionResolver &resolver,
    const InteractionHandlers &handlers
)
{
    // ---- 1. drain (FIFO — the input layer's order is the application order, R7) ----
    bool hasMotion            = false;
    harrowmere::MoveIntent motion{};

    while (!queue.empty())
    {
        const harrowmere::SimCommand cmd = queue.pop();

        // D-AFK-1: ANY intent — even one the rules will reject — proves
        // presence. Refresh before classification so the verdict below
        // reads the fresh stamp.
        player.lastIntentRealTime = clock.realTime;
        player.afk                = false;

        switch (cmd.kind)
        {
        case harrowmere::SimCommand::Kind::Move:
            // Motion state, latest wins (the header doc block): bursts of
            // polled per-frame intents collapse into one integration.
            motion    = cmd.move;
            hasMotion = true;
            break;

        case harrowmere::SimCommand::Kind::Attack:
            // Step 10's applyDamage applies attacks (the §4.6 worked
            // trace: "intent Attack{target} -> next tick step 9 consumes
            // it -> step 10 applyDamage"). The consumer lands with
            // 3.2/3.3 (Phase 2); Phase 1 drains deterministically.
            HARROWMERE_LOG_TRACE(sim, "attack intent drained (combat consumer lands in Phase 2)");
            break;

        case harrowmere::SimCommand::Kind::Interact:
            routeInteraction(player.position, cmd.interact.target, cmd.interact.action, clock.worldTime, resolver, handlers);
            break;

        case harrowmere::SimCommand::Kind::Emote:
            applyEmote(player.actions, cmd.emote.act);
            break;
        }
    }

    // ---- 2. one kinematic integration for the slice's winning motion ----
    if (hasMotion)
        integrateMotion(player.position, motion, dtSeconds);

    // ---- 3. the AFK verdict (D-AFK-1, A-58) ----
    // u64 on the monotonic session clock: lastIntentRealTime is always a
    // realTime value, so the subtraction cannot underflow (R1). At exactly
    // A-58 real seconds without intents the player IS AFK (>= — the rule
    // reads "no intents for AFK_TIMEOUT_REAL_S").
    if (clock.realTime - player.lastIntentRealTime >= AFK_TIMEOUT_REAL_S)
        player.afk = true;
}

} // namespace harrowmere::player
