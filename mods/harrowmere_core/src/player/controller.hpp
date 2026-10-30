#ifndef HARROWMERE_CORE_PLAYER_CONTROLLER_HPP
#define HARROWMERE_CORE_PLAYER_CONTROLLER_HPP

// controller.hpp — the sim-side of the player (task 3.10): the intent queue,
// the kinematic transform, the AFK clock, and the action log.
//
// ---- Phase-1 slice (card 3.10) -----------------------------------------------
//
// No Scheduler and no character records exist in this phase, so this file is
// the SPINE the rest of the card activates around — no new cards:
//
//   * the intent types and the queue. The vocabulary is published in the
//     capability header (SimCommand: Move{dir} / Attack{target} /
//     Interact{target, action} / Emote{act} — the input mod compiles against
//     nothing else); IntentQueue below is the drain side. The queue takes
//     new variants without breaking the contract (a new Kind value + payload
//     field is source-compatible): consent-gated intents (2.7) arrive that
//     way.
//   * kinematic movement of the sim-side player transform (D-PHYS-1: no
//     physics bodies for characters — the physics capability stays optional,
//     ground raycasts only, and NPCs never get rigid bodies).
//   * the AFK clock (D-AFK-1, A-58).
//   * the action-log skeleton (PlayerActions — the reqActions inputs of 2.3
//     and quest objectives 6.4 once their owners land).
//
// Everything runs on the fixed-update subscription: ONE entry point,
// applyIntents() — 3.1 (Phase 2) registers it into step 9 of the 14-step
// table (§4.4 names step 9 "Player") as a RE-REGISTRATION, with these tests
// green unchanged. Phase 1 has no Scheduler and no runtime sim instance (the
// same line task 3.8's DEVIATIONS.md row records), so the tests drive
// applyIntents directly — headless, intents injected into the queue without
// the UI mod (the card's Done-when). There is deliberately NO composition
// function that advances the clocks: the clocks belong to step 0 (0.3's
// WorldClock; 3.8's Phase-1 slice advances them) — applyIntents READS the
// clock and never advances it (pinned by a test).
//
// ---- Move is motion state, not a displacement instruction --------------------
//
// The input layer (7.4P1) polls keys per FRAME and submits the
// camera-relative direction every frame; a 10 Hz fixed update therefore
// receives bursts of Move intents. Integrating per intent would make the
// player's speed frame-rate dependent (six submitted intents = six steps per
// tick) — so the drain is: every intent updates the motion state (the
// LATEST Move in the slice wins), and exactly ONE kinematic integration
// (speed × dt) runs per fixed update. Frame-rate independent, deterministic,
// and the reading that keeps D-AFK-1 coherent: intents flow continuously
// while the player plays, so "no intents for 120 real seconds" means the
// player really went idle. Attack/Interact/Emote stay EDGE events — each
// drained intent applies once, in FIFO order.
//
// ---- Determinism (R5–R8) ------------------------------------------------------
//
//   * The integration is f64 Position math — the docs make Position f64 and
//     R5's carve-out covers exactly this ("f64 appears only in Position and
//     rendering/presentation code"); the compare helpers come from
//     region.hpp's D-WORLD-1 seam, never raw coordinate math. No float
//     enters a game-rule (SCALE-grid) computation here.
//   * The AFK comparison is u64 on WorldClock.realTime (D-TIME-1: AFK
//     detection is one of the three real-time doc rules) — never the wall
//     clock (R8), never worldTime. lastIntentRealTime is session-scoped
//     (realTime restarts at 0 each session) and is NOT save data (7.1 will
//     persist the transform, the act toggles and the counters; afk state
//     re-derives per session).
//   * FIFO everywhere; no unordered container; iteration order is the queue
//     order (R7).
//
// ---- What activates later (do not build ahead) --------------------------------
//
//   Phase 2: the player mirror + needs (sprint ×2 costs rest — 2.1; the
//            Phase-1 sprint is free, per the card), interaction handlers
//            with their owners (trade 3.7, loot 3.3, pick_up/drop 2.4),
//            consent-gated intents (2.7), the Scheduler registration (3.1).
//   Phase 3: report_crime + bribe handlers (4.1), surrender (4.3),
//            mask/crouch as suspicion acts READ by 4.5 (this file only
//            exposes the acts on the action log).
//   Phase 4: the noble title/claim dialog (6.3) and AFK activity validation
//            (6.3 — "AFK time never counts").

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "core/clock.hpp"            // WorldClock — the one heartbeat (task 0.3, D-TIME-1)
#include "core/enums.hpp"            // SuspicionAct — the act vocabulary 4.5 reads
#include "core/types.hpp"            // Position (task 0.2)
#include "harrowmere_capability.hpp" // SimCommand — the published intent vocabulary
#include "player/interactions.hpp"   // the Interact routing (gate + registry + resolver)

namespace harrowmere::player
{

// ---- the action log ------------------------------------------------------------

// PlayerActions — the card's action log: counters mirroring NpcState's
// actionCounters (task 2.0 defines the NPC side; "kills by sapience class,
// crafts, purchases") plus the observable act surface 4.5 reads ("mask/
// crouch as suspicion acts read by 4.5" — the card's Phase-3 activation).
//
// The counters are the reqActions inputs of progression (2.3) and quest
// objectives (6.4). Their writers are the owner systems (kills: combat
// 3.3/aftermath 3.3; crafts: crafting 2.4; purchases: trade 3.7) — the
// record* helpers below are the single write path, so those systems land
// without touching this struct.
//
// The act surface: the three player-side toggles 3.10 owns (crouch, the
// torch's light, the mask) mapped onto core/enums.hpp's SuspicionAct. The
// remaining acts are NOT player-toggled and read false here — their owners
// set them where they live: TRESPASS (4.5's private-property context),
// BLOODSTAINED (4.1's combat aftermath), FLEE (3.2's flee detection). The
// split is pinned by a test.
//
// UNLIT is the ABSENCE of a light: exposes(UNLIT) is true while no torch
// burns. Whether unlit-ness is SUSPICIOUS is 4.5's read (the NIGHT +
// no-darkvision masks gate it there; design reference: "UNLIT applies only
// to species without darkvision") — this log only carries the honest state.
struct PlayerActions
{
    // ---- counters (mirroring NpcState.actionCounters, task 2.0) ----
    std::uint64_t killsSapient    = 0; // kills of sapient characters (people)
    std::uint64_t killsNonSapient = 0; // kills of animals/monsters
    std::uint64_t crafts          = 0; // items crafted
    std::uint64_t purchases       = 0; // purchases made

    // ---- the observable act toggles (the SuspicionAct surface) ----
    bool crouched = false; // CROUCH while crouched
    bool torchLit = false; // UNLIT while NOT lit (light_torch is UNLIT's counter-act)
    bool masked   = false; // MASKED while masked (the ownership check activates with 2.4's inventory — the Phase-1 toggle is free but the act is logged, per the card)

    // ---- the single write path for the counters (owner systems call these) ----
    void recordKill(
        bool sapient
    ) noexcept
    {
        sapient ? ++killsSapient : ++killsNonSapient;
    }

    void recordCraft() noexcept
    {
        ++crafts;
    }

    void recordPurchase() noexcept
    {
        ++purchases;
    }

    // The read 4.5 consumes: is this act exposed right now? Deterministic,
    // order-free, no allocation. The acts 3.10 does not own read false.
    bool exposes(
        SuspicionAct act
    ) const noexcept
    {
        switch (act)
        {
        case SuspicionAct::CROUCH:
            return crouched;
        case SuspicionAct::UNLIT:
            return !torchLit; // UNLIT is the absence of light
        case SuspicionAct::MASKED:
            return masked;
        case SuspicionAct::TRESPASS:
        case SuspicionAct::BLOODSTAINED:
        case SuspicionAct::FLEE:
            return false; // owned by 4.5 / 4.1 / 3.2 respectively — not player-toggled
        }
        return false; // unreachable: the enum is closed (R9-adjacent closed list)
    }

    // The currently-exposed acts, in SuspicionAct declaration order (R7:
    // the output order is the enum order, never container order). Appends
    // to `out` so callers reuse their buffer on the hot path.
    void appendActiveActs(
        std::vector<SuspicionAct> &out
    ) const
    {
        for (const SuspicionAct act :
            {SuspicionAct::CROUCH, SuspicionAct::UNLIT, SuspicionAct::MASKED, SuspicionAct::TRESPASS, SuspicionAct::BLOODSTAINED, SuspicionAct::FLEE})
        {
            if (exposes(act))
                out.push_back(act);
        }
    }
};

// ---- the sim-side player ---------------------------------------------------------

// PlayerController — the local player's sim-side spine (the Character store
// arrives with 2.0; the mirror then attaches to this, the card's "the player
// mirror and needs ... on the player mirror once 2.1 exists"). Plain struct,
// open fields, like every store-side record (§4.5).
struct PlayerController
{
    // The sim-side transform (D-PHYS-1: kinematic — velocity = the pinned
    // walk speed, no physics body). y is cosmetic (card 1.6) and never
    // touched by the integration.
    Position position{};

    // The action log (above): counters + the act surface.
    PlayerActions actions{};

    // AFK (D-AFK-1): true from the update that observes
    // AFK_TIMEOUT_REAL_S (A-58) real seconds without intents until the next
    // drained intent clears it. Consumed by 4.5 (suspicion exemption:
    // "AFK/scripted states exempt") and 6.3 (activity validation: "AFK time
    // never counts").
    bool afk = false;

    // The realTime stamp of the last drained intent (D-AFK-1's
    // bookkeeping). Session-scoped: realTime restarts at 0 each session,
    // so this is deliberately NOT save data (7.1).
    std::uint64_t lastIntentRealTime = 0;
};

// ---- the intent queue --------------------------------------------------------------

// IntentQueue — the drain side of §4.2's write path: submit() (the
// capability) will push here once the runtime bootstrap wires it; step 9
// (applyIntents) drains it. FIFO — the input layer's intent order is the
// application order (R7: no unordered container, order is load-bearing and
// observable). Unbounded: the engine's accumulator already clamps the
// fixed-update burst (max_fixed_update_steps), and a queue bound would be an
// invented constant; the drain is total every update.
class IntentQueue
{
public:
    // The §4.2 write path's landfall. Producer: the capability's submit()
    // (runtime wiring) or, headless, the tests — "intents injected
    // directly" (the card's Done-when).
    void push(
        const harrowmere::SimCommand &cmd
    )
    {
        queue_.push_back(cmd);
    }

    void push(
        harrowmere::SimCommand &&cmd
    )
    {
        queue_.push_back(std::move(cmd));
    }

    // Pop the oldest intent (FIFO). Only valid while !empty() — applyIntents
    // is the drainer; call sites gate on empty().
    harrowmere::SimCommand pop()
    {
        harrowmere::SimCommand cmd = std::move(queue_.front());
        queue_.pop_front();
        return cmd;
    }

    bool empty() const noexcept
    {
        return queue_.empty();
    }

    std::size_t size() const noexcept
    {
        return queue_.size();
    }

    // Drop everything (a fresh tick source rebinding its queue — between
    // ticks only, never mid-drain).
    void clear() noexcept
    {
        queue_.clear();
    }

private:
    std::deque<harrowmere::SimCommand> queue_;
};

// ---- the one fixed-update entry point ----------------------------------------------

// applyIntents(dt) — THE step-9 slice (§4.4 names step 9 "Player"): drain
// the queue, then integrate. 3.1 (Phase 2) registers this function into the
// 14-step table as a re-registration (the seasons pattern); its adapter
// binds the character-store-backed resolver and the owner-registered
// handler table.
//
//   1. Drain FIFO. Every drained intent refreshes the AFK clock (D-AFK-1:
//      ANY intent — even a rejected one — proves presence) and clears
//      player.afk. Per kind:
//        Move    — motion state: the latest Move in the slice wins (see
//                  the header doc block); the integration runs once, below.
//        Attack  — drained; the application is step 10's applyDamage
//                  (§4.6 trace). The consumer lands with 3.2/3.3 (Phase 2);
//                  Phase 1 leaves the state untouched (deterministic drop,
//                  pinned by a test so Phase 2's arrival is conscious).
//        Interact— routeInteraction(): resolve target, gate on proximity
//                  (INCLUSIVE at INTERACT_RANGE_M), dispatch to the
//                  registered owner handler (NoHandler when the owner has
//                  not landed — D-SINK-1). Rejections are gate outcomes,
//                  TRACE-logged; they surface as PlayerSignals when the
//                  runtime wiring lands (§4.2 rule 2).
//        Emote   — toggle the act (CROUCH / LIGHT_TORCH / MASK); the act is
//                  exposed on the action log (PlayerActions::exposes).
//   2. One kinematic integration for the slice's winning motion: direction
//      normalized (zero direction = the R2-style no-motion fallback, never
//      a NaN), speed = MOVE_SPEED_MPS (A-57) × 2 when sprinting (A-57's
//      "sprint ×2" — free in Phase 1), step = speed × dt along the
//      direction. dt is the fixed step's real-second delta the engine
//      publishes with EngineFixedUpdateEvent — engine-owned timing
//      (D-TICK-1); the sim reads no wall clock (R8).
//   3. AFK verdict: afk = (clock.realTime - lastIntentRealTime >=
//      AFK_TIMEOUT_REAL_S). u64 arithmetic, monotonic clock — no underflow
//      (lastIntentRealTime is always a realTime value the clock produced).
//
// The clock is READ-ONLY: the clocks advance in step 0 (0.3 / 3.8's
// Phase-1 slice) — this function never advances them (pinned by a test).
void applyIntents(
    PlayerController &player,
    IntentQueue &queue,
    const core::WorldClock &clock,
    double dtSeconds,
    const InteractionResolver &resolver = nullInteractionResolver(),
    const InteractionHandlers &handlers = InteractionHandlers::empty()
);

} // namespace harrowmere::player

#endif // HARROWMERE_CORE_PLAYER_CONTROLLER_HPP
