#ifndef HARROWMERE_CORE_JUSTICE_CITIZEN_JUSTICE_HPP
#define HARROWMERE_CORE_JUSTICE_CITIZEN_JUSTICE_HPP

// citizen_justice.hpp — task 4.7: civilian NPCs and militias punish
// criminals through information and economy — under the doc's strict
// precondition gate.
//
// Pinned here (design reference verbatim + the card's readings):
//
//   * THE GATE — "NPC violence strictly requires witnessed active crime,
//     active warrant, or conviction — never suspicion": npcMayViolate =
//     witnessedActiveCrime (an in-progress assault the NPC PERSONALLY
//     witnesses — the 3.2 assault accumulator holds the in-progress state,
//     the spatial seam answers "personally") || hasWarrantIn(target, here)
//     || convictedRecord(target, here) (a recent compact record — within
//     the A-71 case window). Suspicion/trust tiers appear NOWHERE in this
//     function — TestGate_SuspicionNeverAuthorizesViolence fills a memory
//     to SHUNNED and pins the refusal.
//
//   * MILITIAS — "veteran militias engage only at favorable odds":
//     militia_veteran NPCs + willing citizens (affinity to the victim
//     >= A-54's 2000, integrity >= MILITIA_CITIZEN_INTEGRITY) form a posse
//     when a warrant is active locally and the guards cannot handle it;
//     engage only when the summed posse power reaches MILITIA_ODDS_RATIO
//     (A-97, 2.0x) of the target's power — else shadow & report (the
//     decision returns; the callers act on the branch).
//
//   * CAPTURE & BOUNTIES — "alive capture pays full; dead pays partial.
//     Warrants pay once; bounty total stays below conviction penalty":
//     claimBounty routes through a guard/captain/militia_veteran present in
//     the warrant's territory (the claim's processor), pays
//     bounty (alive) or bounty * DEAD_BOUNTY_RATIO (A-98, 0.6x, dead) —
//     each alive capture within CAPTURE_FATIGUE_WINDOW_DAYS (A-99) of the
//     last compounds the payout by CAPTURE_FATIGUE_FACTOR (A-100, 0.5x) —
//     then payWarrant (the `paid` flag is on the WARRANT; a second claim
//     finds the warrant gone — the pay-once rule's store-level form).
//
//   * LETHALITY — lethal warrants permit lawful execution (3.3's
//     isCriminalAct already exonerates through hasLethalWarrantIn);
//     killing under a NON-lethal warrant routes to murder evidence — the
//     death path's own rule, pinned here by the test.
//
//   * SMUGGLING — "smuggling a culprit grants accomplice evidence instead
//     of event rewards": a character sharing an event with a culprit who is
//     that event's TARGET-SIDE participant and aids them (4.1's trigger
//     list) earns SMUGGLING evidence and is EXCLUDED from the event's
//     reward distribution (the IEventExclusionSink seam — 5.3 consumes
//     excludedFromRewards; the interface was provided here).
//
//   * VILLAGE SLAUGHTER — "grants zero economic gain and gives neighboring
//     territories a casus belli": >= MASSACRE_THRESHOLD (A-101 = 5) sapient
//     NPC deaths by one culprit in one territory within MASSACRE_WINDOW_DAYS
//     (A-102 = 2) zero the victims' corpse loot currency (loot items
//     remain) and register the casus belli for the neighboring territories
//     (the parent's children + the parent) through the IWarRegistry stub —
//     6.2 implements against the frozen signature (D-SINK-1).

#include <cstdint>
#include <map>
#include <vector>

#include "core/types.hpp" // characterId, territoryId

namespace harrowmere
{

struct WorldContext; // core/world_context.hpp

namespace justice
{

struct Warrant; // warrants.hpp — reference only here

// ---- the gate ------------------------------------------------------------------

// npcMayViolate — the doc's gate, verbatim (see the header doc). `npc` and
// `target` are ids (the callers hold no pointers — §4.5).
bool npcMayViolate(characterId npc, characterId target, WorldContext &ctx);

// witnessedActiveCrime — the first arm: an in-progress unconsented assault
// (the 3.2 accumulator holds one for the target) the NPC personally
// witnesses (the target within WITNESS_RADIUS of the NPC).
bool witnessedActiveCrime(characterId npc, characterId target, WorldContext &ctx);

// convictedRecord — the third arm: a compact record in the territory's
// archive naming the target within the A-71 case window ("recent").
bool convictedRecord(characterId target, territoryId territory, WorldContext &ctx);

// ---- militias ------------------------------------------------------------------

struct Posse
{
    std::vector<characterId> members; // militia veterans first, then citizens (id-ascending, R7)
    std::uint64_t totalPower = 0;
    bool engages             = false; // the A-97 odds gate
};

// formPosse — the card's rule: the veteran+citizen posse for a warrant
// active locally; engages only at the favorable odds, else the decision is
// shadow-&-report (engages == false; the callers act on the branch).
Posse formPosse(const Warrant &warrant, WorldContext &ctx);

// ---- capture & bounties ----------------------------------------------------------

enum class BountyClaim
{
    Paid,          // the payout landed (the warrant left the active set)
    NoProcessor,   // no guard/captain/militia_veteran in the warrant's territory
    UnknownWarrant // the warrant id is not live (already paid/expired — pay-once)
};

// claimBounty — the capture/kill claim (see the header doc). `capturedAlive`
// selects the full bounty vs the A-98 partial; alive captures stamp the
// capture-fatigue window. The payout lands on the claimant's purse (i64).
BountyClaim claimBounty(std::uint64_t warrantId, characterId claimant, bool capturedAlive, WorldContext &ctx);

// The fatigue-adjusted payout read (the invariant test's oracle): the bounty
// after the compounding factor for the culprit's capture history.
std::uint64_t bountyPayoutFor(const Warrant &warrant, bool capturedAlive, WorldContext &ctx);

// ---- smuggling -------------------------------------------------------------------

// recordSmuggling — the aid trigger for an event's target-side culprit: the
// helper (already an event participant with the culprit) earns the SMUGGLING
// evidence at the culprit's position and lands in the event's
// excludedFromRewards. No shared event or no target-side membership = no-op
// ("watching is never punished" — mere co-presence is not aiding).
void recordSmuggling(characterId helper, characterId aidedCulprit, WorldContext &ctx);

// ---- village slaughter -----------------------------------------------------------

// noteNpcKilled — the death path's hook (JusticeSink::onNpcKilled): prunes
// the kill tally to the A-102 window, raises the massacre marker at the
// threshold, and registers the casus belli once per crossing.
void noteNpcKilled(characterId victim, characterId killer, WorldContext &ctx);

// massacreLootCurrency — the loot-currency read the death path stages: 0
// while the (killer, territory) marker stands, the purse otherwise.
std::int64_t massacreLootCurrency(characterId killer, territoryId territory, std::int64_t purse, WorldContext &ctx);

} // namespace justice

} // namespace harrowmere

#endif // HARROWMERE_CORE_JUSTICE_CITIZEN_JUSTICE_HPP
