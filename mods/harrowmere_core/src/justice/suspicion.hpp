#ifndef HARROWMERE_CORE_JUSTICE_SUSPICION_HPP
#define HARROWMERE_CORE_JUSTICE_SUSPICION_HPP

// suspicion.hpp — task 4.5: behavioral suspicion that alters trust, prices,
// and services — and NEVER justifies violence or generates evidence (the
// doc's hard rule; TestSuspicion_NeverCreatesEvidence pins it).
//
// What lives here, per the card:
//
//   * THE MEMORY — per (observer NPC, observed id): score + lastSeen +
//     acquaintance, stored on the observer's NpcState.trust (the TrustMemory
//     record 3.4 declared, filled by 4.5 — D-STORE-1: NpcState is the NPC
//     sidecar). Capped at MEMORY_CAP (A-89 = 16) entries; eviction drops the
//     lowest trust tier first and acquaintances outrank strangers (the doc
//     rule; TestSuspicion_MemoryCapEvictsLowestTierStrangerFirst +
//     TestSuspicion_AcquaintanceOutranksStrangerUnderCap).
//
//   * THE LADDER — tierFor(score) verbatim: SHUNNED >= 8000, DISTRUSTED >=
//     5000, WATCHED >= 2000, STRANGER below (A-90).
//
//   * THE EVALUATION — per NEAR/MID observer per game-hour (the step-6
//     driver, near-territory budget): for each (act, contextMask) currently
//     true about the observed character, the matching trigger row's weight
//     accumulates into the memory. Acts read 3.10's act surface (the player:
//     crouched/unlit/masked live; the acts other cards own read false —
//     TRESPASS/BLOODSTAINED/FLEE have no v1 writer, the 4.1 DEVIATIONS row).
//     Contexts: SETTLEMENT (within D-SHELTER-1's 50 m of the settlement
//     center), NIGHT (the dayPhase night band), PRIVATE_PROPERTY (territory
//     data flag — none exists in v1, reads false), MARKET (rides the
//     settlement radius in v1 — no separate market geometry ships; the
//     DEVIATIONS row notes it). A trigger row fires when the act is exposed
//     AND EVERY mask bit of its row is true — the reading that keeps the
//     shipped rows honest (UNLIT's [NIGHT, SETTLEMENT] must not fire at noon;
//     the doc's own "unlit-at-night" act name pins the direction).
//
//   * THE DECAY — score -SUSPICION_DECAY (A-91) per game-day (lazy, at every
//     evaluation), -SUSPICION_DONATION_RELIEF (A-92) extra on donations and
//     service; stranger memories lapse after STRANGER_MEMORY_S (A-56) out of
//     sight, acquaintance memories are permanent.
//
//   * THE EFFECTS — all reads, no writes elsewhere: the 3.7 sell-receipt hook
//     (tier modifier, A-93) through SuspicionPriceModifier (the plugin binds
//     it over the null default), the healer/vendor service refusal
//     (serviceRefused: SHUNNED refuses), and the greeting tone read the UI
//     panels consume (greetingTone; 7.3b reads it via the game view).
//     The 2.6 appearance-frequent signal adds APPEARANCE_SUSPICION_WEIGHT
//     (A-94) once per A-25 window (the sink fires only at threshold
//     crossings — the window is A-25's own).
//
// NEVER-DO list (each enforced by the pinned tests): nothing here writes
// evidenceStore, nothing here feeds npcMayViolate (4.7's gate — suspicion
// appears nowhere in it), nothing here deals damage.

#include <cstdint>

#include "core/constants.hpp"     // A-89..A-94, A-56, A-54
#include "core/enums.hpp"         // SuspicionAct, TrustTier
#include "core/scheduler.hpp"     // IStepSystem (the step-6 driver)
#include "core/types.hpp"         // characterId
#include "core/world_context.hpp" // WorldContext + the sink seams (ISuspicionSink, IPriceModifierSink)
#include "npc/state.hpp"          // NpcState + TrustMemory (the storage)

namespace harrowmere
{

struct Character; // character/character.hpp — the doc struct; reference only here

} // namespace harrowmere

namespace harrowmere::justice
{

// The card's memory view, verbatim: one entry per (observer, observed) pair,
// assembled over the observer's TrustMemory + affinity rows.
struct SuspicionMemoryEntry
{
    characterId who        = 0;
    std::uint32_t score    = 0;
    bool acquaintance      = false;
    std::uint64_t lastSeen = 0;
};

// ---- the ladder + the memory API (the observer's NpcState owns the rows) ----

// tierFor — the card's ladder, verbatim (A-90).
TrustTier tierFor(std::uint32_t score);

// The pair's acquaintance half: the 3.9 rule (affinity >= A-54) — acquaintances
// outrank strangers at eviction and their memories never lapse.
bool isAcquaintance(const NpcState &observer, characterId observed);

// The pair's memory entry (score 0 + lastSeen 0 when none — the honest
// zero-value rule).
SuspicionMemoryEntry memoryEntry(const NpcState &observer, characterId observed);

// The pair's current tier (STRANGER with no memory).
TrustTier tierOf(const NpcState &observer, characterId observed);

// addSuspicion — accumulate one trigger's weight onto the observed id's entry
// (saturating u32), refresh the lastSeen stamp, then enforce MEMORY_CAP.
// The just-written entry keeps its slot; eviction drops the weakest PRIOR
// entry — lowest tier first; equal tiers by lowest score, then the oldest
// lastSeen, then the lower id (R7 determinism); acquaintances are never
// evicted while any stranger entry exists.
void addSuspicion(NpcState &observer, characterId observed, std::uint32_t weight, std::uint64_t now);

// relieveSuspicion — the donation/service relief (A-92): the pair's score
// drops toward zero (never below).
void relieveSuspicion(NpcState &observer, characterId observed, std::uint32_t amount = SUSPICION_DONATION_RELIEF);

// decaySuspicion — the lazy quantum: whole game-day steps since the last
// decay apply SUSPICION_DECAY (A-91) per step toward zero. A stranger entry
// lapses once its score has FADED TO ZERO and sight was lost for
// STRANGER_MEMORY_S (A-56 — lastSeen holds the last sight stamp; the
// evaluation refreshes it while co-present); an acquaintance's memory is
// permanent (it decays to zero and stays). Called at the start of every
// evaluation + read (the 4.4 pattern).
void decaySuspicion(NpcState &observer, std::uint64_t now);

// ---- the evaluation -----------------------------------------------------------

// The context mask the driver computes per observed character (laws.hpp's
// SuspicionMask bits): SETTLEMENT + MARKET ride D-SHELTER-1's settlement
// radius in v1, NIGHT the dayPhase night band, PRIVATE_PROPERTY the territory
// data flag (no v1 data — reads false).
std::uint32_t contextMaskFor(const WorldContext &ctx, const Character &observed);

// The observed character's exposed acts, SuspicionAct order (the player reads
// 3.10's surface; an NPC observed carries no act state in v1 — every act
// reads false, the honest zero-value). UNLIT applies only when the observed
// species LACKS darkvision (species.json — the card's pitfall).
void appendExposedActs(const WorldContext &ctx, const Character &observed, std::vector<SuspicionAct> &out);

// evaluateObserver — one observer's pass: decay first, then every perceived
// character's (act, context) rows accumulate into the observer's memory
// (exposed for the tests' direct per-observer assertions).
void evaluateObserver(
    WorldContext &ctx, characterId observerId, double radiusMeters, std::vector<SuspicionAct> &actsScratch
);

// The 2.6 signal's per-observer half (exposed for the sink tests).
void applyAppearanceSignal(WorldContext &ctx, characterId observerId, characterId who, std::uint64_t at);

// runSuspicionTicks — the step-6 driver (near-territory budget, per game-hour
// cadence): every NEAR/MID NPC observer in the LOD active set evaluates the
// characters within its perception radius against the trigger table
// (laws.json's per-set `suspicion` rows replace the global suspicion.json
// table where non-empty). Mass-suspicious behavior fills memories and moves
// tiers — and creates ZERO evidence (the doc's hard rule).
void runSuspicionTicks(WorldContext &ctx);

// The step system the plugin registers at TickStep::Justice AFTER the case
// ticks (registration order within the step = execution order).
class SuspicionTickSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;

    // ---- save seam (task 7.1): the hourly cadence stamp rides the save's
    // "runtime" block — without it the load tick would re-evaluate the load
    // hour's acts and double-count one suspicion pass.
    [[nodiscard]] std::uint64_t lastRunHour() const noexcept
    {
        return lastRunHour_;
    }
    void restoreLastRunHour(
        std::uint64_t at
    ) noexcept
    {
        lastRunHour_ = at;
    }

private:
    std::uint64_t lastRunHour_ = 0; // A-76-style hourly cadence stamp
};

// ---- the effects (all reads; the writers are the interaction paths) -----------

// serviceRefused — the provider refuses the customer when the provider's
// memory tier for them is SHUNNED ("healer refuses SHUNNED; vendor refuses
// trade"). The roles.cpp healer duty and the trade paths gate through this.
bool serviceRefused(const NpcState &provider, characterId customer);

// A-93's sell-receipt read: the tier modifier the territory's first
// vendor-role resident (id-ascending, R7) holds for the seller — WATCHED
// -5%, DISTRUSTED -10%, SHUNNED -20% (receipts), SCALE when nobody judges
// (no vendor, no memory, or a stranger).
std::uint64_t sellReceiptModifierFor(const WorldContext &ctx, characterId seller, territoryId territory);

// The 3.7 hook's live implementation (the plugin binds it over the null
// default; null context members degrade to SCALE, D-SINK-1).
class SuspicionPriceModifier final : public IPriceModifierSink
{
public:
    void bind(
        const WorldContext *ctx
    )
    {
        ctx_ = ctx;
    }
    std::uint64_t sellReceiptModifier(characterId seller, territoryId territory) const override;

private:
    const WorldContext *ctx_ = nullptr;
};

// greetingTone — the tier the greeter holds for the observed (the 4.6 cold
// greeting read; 7.3b consumes it through the game view).
TrustTier greetingTone(const NpcState &greeter, characterId observed);

// The 2.6 seam's implementation (the plugin binds it into
// WorldContext::suspicion — the A-25 frequent-change signal lands here as
// APPEARANCE_SUSPICION_WEIGHT once per window, on every NEAR/MID observer's
// memory of the changer).
class SuspicionSignalSink final : public ISuspicionSink
{
public:
    void bind(
        WorldContext *ctx
    )
    {
        ctx_ = ctx;
    }
    void onAppearanceFrequent(characterId who, std::uint64_t at) override;

private:
    WorldContext *ctx_ = nullptr;
};

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_SUSPICION_HPP
