#ifndef HARROWMERE_CORE_JUSTICE_ENFORCEMENT_HPP
#define HARROWMERE_CORE_JUSTICE_ENFORCEMENT_HPP

// enforcement.hpp — task 4.6: guards that assess odds, arrest the weak,
// evacuate and call manhunts against the strong, always SIGNAL before
// acting, and handle lawless territories.
//
// Pinned here (design reference verbatim + the card's readings):
//
//   * localGuardPower — the sum of the CURRENT power of every
//     guard/captain/militia_veteran-role NPC whose home is the territory
//     (the card's role list, verbatim; the garrison is who answers the
//     alarm, not whoever happens to be walking through).
//
//   * respondToWarrant — doc verbatim: localGuardPower * ENGAGE_RATIO
//     (A-95, 1.5x) >= getCurrentPower(culprit) * SCALE -> attemptArrest;
//     otherwise evacuateCivilians + spawnEvent("MANHUNT", warrant). The
//     ratio multiplies the GUARD side (the card pitfall: inverting makes
//     suicide-guards — both directions are pinned). The manhunt spawn is
//     ENQUEUED into step 5 (the §4.4 reentrancy rule — never synchronously
//     inside step 6): the driver hands the spawn to the scheduler.
//
//   * SIGNALS BEFORE FORCE (the doc: "never arrest with zero prior
//     signals"): when a case first reaches INVESTIGATING ->
//     PlayerSignal{Investigated}; at WANTED -> PlayerSignal{Wanted} +
//     Poster (the poster record rides the JusticeStore for 7.3b's panel +
//     the UI notice). notifyCaseStatusChanged is the case machine's hook
//     (cases.cpp calls it on every status transition); attemptArrest
//     REFUSES (deterministic no-op, the return value names it) when no
//     signal was ever recorded for the culprit — the invariant test pins
//     it in both build flavors.
//
//   * Arrest resolution: an NPC culprit auto-submits -> the warrant's
//     evidence convicts (4.3's standard sentence); a player culprit
//     stands their engagement — the combat pipeline's warrant-backed
//     consent applies (damage.cpp's warrantCovers: the guard's damage is
//     lawful, the culprit's defense damage vs guards is assault unless a
//     lethal warrant exists), and the player ends it by surrender (4.3),
//     escape, or capture (4.7's bounty rules).
//
//   * EVACUATION — civilians (non-guard roles) of the territory take the
//     fleeing intent toward home/safe zone until `untilWorldTime` (the
//     manhunt's duration); MID/FAR civilians are abstractly marked by the
//     same NpcState flag. EvacuateTerritory (5.1's action) calls the same
//     mechanic — the engine's trigger, 4.6's implementation.
//
//   * LAWLESS — a territory with zero guard-role NPCs and no safe flag
//     sits lawless after LAWLESS_AFTER_DAYS (A-96 = 3 game-days):
//     lawsAt resolves DEFAULT_LAWS (WorldStore's own rule — 4.6 drives
//     the watch, the world owns the flag), case ticks skip the territory,
//     and enforcement never responds there. Exits: clearLawless — the
//     reconquest event (5.5) or the player claim (6.3) calls it.
//
//   * The spawner rides the IEventSpawner seam (world_context.hpp) — no
//     events/ include anywhere in this file (D-SINK-1; the Done-when).

#include <cstdint>
#include <set>

#include "core/scheduler.hpp" // IStepSystem (the step-6 driver)
#include "core/types.hpp"     // characterId, territoryId

namespace harrowmere
{

struct WorldContext; // core/world_context.hpp — reference only here

} // namespace harrowmere

namespace harrowmere::justice
{

struct Evidence; // justice/evidence.hpp — reference only here
struct Warrant;  // justice/warrants.hpp — reference only here

// localGuardPower — the card's sum: current power of the territory's
// guard/captain/militia_veteran home garrison. Unbound context members read
// 0 (the D-SINK-1 recovery).
std::uint64_t localGuardPower(territoryId t, WorldContext &ctx);

// The culprit's current power (getCurrentPower through the registry + the
// instance registry — a thin read the tests can also call).
std::uint64_t culpritPower(characterId culprit, WorldContext &ctx);

// respondToWarrant — design reference verbatim (see the header doc). The
// manhunt spawn enqueues into the NEXT tick's step 5 through the scheduler;
// this call itself never re-enters the event engine.
enum class EnforcementDecision
{
    Arrested,       // the odds held: the arrest attempt resolved (or engaged)
    Evacuated,      // the culprit overmatched the garrison: civilians flee + manhunt enqueued
    LawlessRefused, // no one to enforce (the lawless rule)
    NoSignal        // the doc invariant: never arrest with zero prior signals
};

EnforcementDecision respondToWarrant(const Warrant &w, WorldContext &ctx);

// The signals half. The case machine calls this on every status transition
// (INVESTIGATING first reach -> Investigated; WANTED -> Wanted + Poster).
// Signals publish through the ISimEventSink::onPlayerSignal seam.
void notifyCaseStatusChanged(const Evidence &e, int oldStatusOrdinal, WorldContext &ctx);

// attemptArrest — the odds held: resolve the arrest. NPC culprit: auto-submit
// -> convict (4.3). Player culprit: the engagement stands (the warrant-backed
// consent is combat's own check) — the function records the engagement and
// returns Arrested. Refused with NoSignal when no prior signal exists for the
// culprit (the doc invariant — a return value, not an assert, so the
// invariant holds in release builds too).
EnforcementDecision attemptArrest(const Warrant &w, WorldContext &ctx);

// evacuateTerritoryCivilians — the manhunt's civilian flight: every
// non-guard-role NPC whose home is the territory takes the fleeing intent
// until `untilWorldTime` (NEAR moves, MID/FAR abstractly marked). The
// 5.1 EvacuateTerritory action shares this mechanic.
void evacuateTerritoryCivilians(territoryId territory, std::uint64_t untilWorldTime, WorldContext &ctx);

// ---- lawlessness (the driver + the read) ----------------------------------------

// updateLawlessness — the step-6 driver: per territory (id ascending, R7),
// stamp the garrison-less watch, promote to lawless after A-96, and clear
// both the moment guards exist again. Safe territories never watch.
void updateLawlessness(WorldContext &ctx);

// ---- the step system ------------------------------------------------------------

// Step 6 (Justice), registered AFTER the case ticks + the suspicion ticks:
// warrant response (new/refreshed warrants only — the responded set is
// this object's own memory) + the lawlessness update. Registration order
// within the step = execution order (§4.4).
class EnforcementStepSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;

    // ---- save seam (task 7.1): the responded set is the signal-before-force
    // memory across the load boundary — without it a resumed world re-responds
    // to warrants the guards already engaged. Rides the save's "justice"
    // block (the dedup set is justice-state-shaped; the README documents it).
    [[nodiscard]] const std::set<std::uint64_t> &responded() const noexcept
    {
        return responded_;
    }
    void restoreResponded(
        std::set<std::uint64_t> rows
    )
    {
        responded_ = std::move(rows);
    }

private:
    std::set<std::uint64_t> responded_; // warrant ids already responded to

    // The lawlessness driver's day gate (8.3's soak found the per-tick scan
    // at 87% of the tick budget): the promotion threshold is DAY-granular
    // (LAWLESS_AFTER_DAYS, A-96), so the full-population garrison scan runs
    // once per game-day (+ the armed first call), not 14,400x per day. The
    // pinned updateLawlessness function keeps its per-call semantics — the
    // gate lives on the step system alone.
    std::uint64_t lastLawlessDay_ = 0;
    bool lawlessArmed_            = false;
};

} // namespace harrowmere::justice

#endif // HARROWMERE_CORE_JUSTICE_ENFORCEMENT_HPP
