// npc/roles.hpp — Task 3.6: role assignment and the doc's replacement flow that
// keeps settlements functional when their guard/vendor/noble dies.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "core/scheduler.hpp"
#include "core/types.hpp"

namespace harrowmere
{

struct WorldContext;
struct NpcState;

// Sim-side role state: open replacement seats (at most one per role per territory —
// the population/time gating rule) and the captain's territory guard-power rollup.
// Role DATA (priority, integrityRange, critical, suitableSpecies) lives in roles.json
// via the DataRegistry — data, not code (task 1.3).
class RoleRegistry
{
public:
    bool hasOpenSeat(
        territoryId territory, const std::string &role
    ) const
    {
        auto it = openSeats_.find(territory);
        return it != openSeats_.end() && it->second.count(role) != 0;
    }
    void markOpenSeat(
        territoryId territory, const std::string &role
    )
    {
        openSeats_[territory].insert(role);
    }
    void clearOpenSeat(
        territoryId territory, const std::string &role
    )
    {
        auto it = openSeats_.find(territory);
        if (it != openSeats_.end())
        {
            it->second.erase(role);
            if (it->second.empty())
                openSeats_.erase(it);
        }
    }
    const std::set<std::string> &openSeatsOf(
        territoryId territory
    ) const
    {
        static const std::set<std::string> kEmpty;
        auto it = openSeats_.find(territory);
        return it == openSeats_.end() ? kEmpty : it->second;
    }

    void setGuardPower(
        territoryId territory, std::uint64_t power
    )
    {
        guardPower_[territory] = power;
    }
    std::uint64_t guardPower(
        territoryId territory
    ) const
    {
        auto it = guardPower_.find(territory);
        return it == guardPower_.end() ? 0u : it->second;
    }

    // ---- save seam (task 7.1): the open replacement seats + the captain's
    // guard-power rollup are sim state; persist/ serializes both maps.
    struct Snapshot
    {
        std::map<territoryId, std::set<std::string>> openSeats;
        std::map<territoryId, std::uint64_t> guardPower;
    };
    [[nodiscard]] Snapshot snapshot() const
    {
        return Snapshot{openSeats_, guardPower_};
    }
    void restore(
        const Snapshot &saved
    )
    {
        openSeats_  = saved.openSeats;
        guardPower_ = saved.guardPower;
    }

private:
    std::map<territoryId, std::set<std::string>> openSeats_; // R7
    std::map<territoryId, std::uint64_t> guardPower_;
};

// Design reference verbatim (A-48: REPLACEMENT_DELAY = 1 game-day): schedule the
// replacement, then promote an eligible NPC or migrate a backup from the frontier.
// Captures the ROLE KEY and territory — never a reference to the dead NPC (pitfall).
void onCriticalNpcDeath(const NpcState &dead, WorldContext &ctx);

// A living, adult (>= ADULT_YEARS), sapient NPC of the role's species Suitability in
// the SAME territory, roleless or in a lower-priority role (roles.json priority).
bool eligibleNpcExists(const std::string &role, territoryId territory, WorldContext &ctx);

// Highest current-role priority, then oldest; deterministic ties by lower id (R7).
characterId selectEligibleNpc(const std::string &role, territoryId territory, WorldContext &ctx);

void assignRole(characterId npcId, const std::string &role, WorldContext &ctx);

// Reuses 1.5's seed path for one NPC, marked migrated = true; only when the
// territory population >= template min (else the seat stays open until births refill).
void spawnBackupNpc(const std::string &role, territoryId territory, WorldContext &ctx);

// Births refill populations; pending seats retry when that happens (3.4 -> 3.6 hand-off).
void retryPendingSeats(territoryId territory, WorldContext &ctx);

// Step 4 (registered after the NPC tick system): hourly role duties for the NEAR/MID
// active set (guard patrol, healer, captain rollup reads, vendor reads) + the daily
// statistical production for the whole population (farmer, noble taxes) — duties v1
// are deliberately mechanical (3.6).
class RoleDutiesSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;
};

// The daily statistical pass (called from 3.4's daily sweep, step 1 — FAR tier works
// only here, never per tick).
void rolesDailySweep(WorldContext &ctx);

} // namespace harrowmere
