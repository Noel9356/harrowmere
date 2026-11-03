// combat/justice_sink.hpp — Task 3.3 (D-SINK-1).
//
// Cross-layer calls out of combat/death go through these interfaces; the null
// implementations below ship with Phase 2 and task 4.1 swaps the real justice
// sink in via the scheduler wiring. Combat/death code never includes justice
// internals (Done-when, 3.3).
//
// The perception seam ("You leave these seams" #1) lives here too: the crime
// pipeline calls `IPerceptionSink::perceiveWithin` and hands the witness list
// to the sink; Phase 3's task 3.9 implements perception, task 4.1 consumes it.
#pragma once
#include <cstdint>
#include <vector>

#include "core/enums.hpp" // CrimeType (the CrimeContext payload)
#include "core/types.hpp"

namespace harrowmere
{

// Everything the future evidence store needs at commit time. The witness list is
// gathered through the perception seam; the stub returns no witnesses (Phase 2
// keeps no records — nobody in the world writes evidence down yet).
struct CrimeContext
{
    CrimeType type      = CrimeType::MURDER;
    characterId culprit = 0;
    characterId victim  = 0;
    Position position;
    std::uint64_t worldTime = 0;
    std::vector<characterId> witnesses; // perceived via IPerceptionSink (3.9); empty until then
};

// D-SINK-1: recordCrime/noteAssault/hasWarrantIn/hasLethalWarrantIn/holdsGuardGearTheft.
// isArrested is 2.1's pinned WorldContext justice query (task 4.3's jailUntil map).
class JusticeSink
{
public:
    virtual ~JusticeSink() = default;

    virtual void recordCrime(
        const CrimeContext & /*crime*/
    )
    {}
    virtual void noteAssault(
        characterId /*culprit*/, characterId /*victim*/, std::uint64_t /*appliedDamage*/
    )
    {}

    // Warrant queries are jurisdiction-bound and null-false until 4.2 lands.
    virtual bool hasWarrantIn(
        characterId /*suspect*/, territoryId /*territory*/
    ) const
    {
        return false;
    }
    virtual bool hasLethalWarrantIn(
        characterId /*suspect*/, territoryId /*territory*/
    ) const
    {
        return false;
    }

    // 2.4's guard-gear theft hook lands here (task 4.1 fulfils it).
    virtual void holdsGuardGearTheft(
        characterId /*culprit*/, const Position & /*where*/
    )
    {}

    virtual bool isArrested(
        characterId /*who*/
    ) const
    {
        return false;
    }

    // 4.1's delayed-report trigger: a corpse's `discovered` flag flipped
    // (3.3's AftermathSystem scan). `discoverer` is the living character whose
    // presence flipped it (0 = unknown — an anonymous discovery still reports,
    // the doc's source-0 convention). Null default keeps Phase-2 behavior.
    virtual void onBodyDiscovered(
        characterId /*victim*/, characterId /*discoverer*/
    )
    {}

    // 4.7's village-slaughter tally: a sapient NPC died with a known killer
    // (the death already applied — step 10's D-TICK-1 rule; the loot staging
    // reads the marker AFTER this fires). Null default keeps 3.3 behavior.
    virtual void onNpcKilled(
        characterId /*victim*/, characterId /*killer*/
    )
    {}

    // 4.7's massacre loot rule: the loot currency a corpse may carry for this
    // killer at this position — 0 when the village-slaughter marker is active
    // for the (culprit, territory) pair, the victim's purse otherwise. The
    // default returns the purse unchanged (no justice layer = no marker).
    virtual std::int64_t massacreLootCurrency(
        characterId /*killer*/, const Position & /*at*/, std::int64_t purse
    )
    {
        return purse;
    }
};

class NullJusticeSink final : public JusticeSink
{};

// Seam #1: perceive → report. Task 3.9 implements; the stub returns empty.
//
// 4.1 extends the seam with the identity half: the crime pipeline qualifies
// witnesses through 3.9's relationships API and asks THIS interface who
// actually knows the culprit (the sighting/affinity rule — no justice-side
// reimplementation). The stub answers keep the Phase-2 readings (nobody
// perceived, nobody knows).
class IPerceptionSink
{
public:
    virtual ~IPerceptionSink() = default;
    virtual std::vector<characterId> perceiveWithin(
        const Position & /*position*/, double /*radiusMeters*/
    )
    {
        return {};
    }

    // w.knows(culpritId) (3.9): true iff the observer is an acquaintance
    // (affinity >= A-54) or holds a fresh, appearance-matched sighting of the
    // culprit. Handles both observer kinds (NPC with a NpcState; the player —
    // sighting rows only). Stub: false.
    virtual bool knowsIdentity(
        characterId /*observer*/, characterId /*culprit*/
    )
    {
        return false;
    }

    // The freshest sighting stamp of `who` across every observer — 4.1's
    // missing-persons sweep input ("no sighting for A-64"). 0 = no sighting
    // recorded anywhere (indistinguishable from a sighting AT the epoch — a
    // documented approximation). Stub: 0 (nobody has been seen).
    virtual std::uint64_t lastSightingOf(
        characterId /*who*/
    )
    {
        return 0;
    }
};

} // namespace harrowmere
