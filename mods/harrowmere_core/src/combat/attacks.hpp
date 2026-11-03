// combat/attacks.hpp — Task 3.2: attack resolution v1 (pinned A-28..A-31) and the
// step-9 -> step-10 combat queue (§4.6's trace: an Attack intent consumed at step 9
// resolves at step 10 through the one damage pipeline).
#pragma once
#include <cstdint>
#include <vector>

#include "core/result.hpp" // Result<void, ...> (task 0.2)
#include "core/scheduler.hpp"
#include "core/types.hpp"

namespace harrowmere::data
{
struct ItemData;    // items.hpp (1.1) — the weapon-factor row
class DataRegistry; // registry.hpp (task 1.3)
} // namespace harrowmere::data

namespace harrowmere
{

struct WorldContext;
struct Character;

struct AttackRequest
{
    characterId attacker       = 0;
    characterId target         = 0;
    std::uint64_t weaponItemId = 0;     // equipped item instance id; 0 = unarmed
    bool isSpell               = false; // v1: the single "spark" spell (D-LORE-2)
};

// Scratch store between step 9 (intents/commands) and step 10 (combat resolution).
// The D-LORE-2 casting-cadence cooldown lives here too: the pinned CooldownType
// list (task 1.1, COUNT == 3) has no ARCANA slot and must not grow, so the
// A-31 gate rides this store's per-character map (R7) instead of inventing an
// undocumented Character field or an enum value.
class CombatQueue
{
public:
    // A-31: is `who` inside the ARCANA casting cadence at `now`?
    bool castOnCooldown(
        characterId who, std::uint64_t now
    ) const
    {
        const auto it = castCooldowns_.find(who);
        return it != castCooldowns_.end() && it->second > now;
    }

    // Arm (or re-arm) `who`'s casting cooldown until `expiresAt` (worldTime).
    void markCast(
        characterId who, std::uint64_t expiresAt
    )
    {
        castCooldowns_[who] = expiresAt;
    }

    void push(
        const AttackRequest &r
    )
    {
        requests_.push_back(r);
    }
    std::size_t size() const
    {
        return requests_.size();
    }
    std::vector<AttackRequest> drain()
    {
        std::vector<AttackRequest> out = std::move(requests_);
        requests_.clear();
        return out;
    }

    // ---- save seam (task 7.1): the A-31 casting cadence book persists; the
    // pending request vector does NOT (same-tick scratch between steps 9 and
    // 10 — §4.6, empty at every save point).
    [[nodiscard]] const std::map<characterId, std::uint64_t> &castCooldowns() const noexcept
    {
        return castCooldowns_;
    }
    void restoreCastCooldowns(
        std::map<characterId, std::uint64_t> rows
    )
    {
        castCooldowns_ = std::move(rows);
    }

private:
    std::vector<AttackRequest> requests_;                // insertion order is the resolution order (R7)
    std::map<characterId, std::uint64_t> castCooldowns_; // A-31 expiry per caster (R7)
};

// Rule-level outcomes of the attack-resolution layer (§3.4): a miss is a
// RESULT, not damage; the 0.2 Result<void, E> template is the tree-wide carrier.
enum class AttackError
{
    Missed,            // the "combat" stream roll beat the A-28 hit chance
    OnCooldown,        // D-LORE-2's ARCANA casting cadence (A-31)
    RequirementNotMet, // the spell's ARCANA gate (3.2: {"spark": ARCANA 10})
    Rejected,          // the damage pipeline refused (invalid target / invulnerable)
    COUNT
};
using AttackResult = Result<void, AttackError>;

// hitChance = clamp(SCALE/2 + (agiA - agiD)*100/SCALE, 1000, 9000)   (A-28, R1 diff)
std::uint64_t hitChance(const data::DataRegistry &data, const Character &attacker, const Character &defender);

// rawDamage = max(MIN_DAMAGE, attEff(STR)*WF/GRID - defEff(DEF)*DEFENCE_FACTOR/GRID)
// (A-29/A-30; the factors live on the per-mille grid — see constants_phase2.hpp).
std::uint64_t rawDamage(
    const data::DataRegistry &data, const Character &attacker, const Character &defender, const data::ItemData *weapon
);

// Rolls hit on the "combat" stream, then funnels through applyDamage. A miss
// produces no DamageEvent at all (nothing to record, nothing to contribute).
AttackResult resolveAttack(Character &attacker, Character &defender, const data::ItemData *weapon, WorldContext &ctx);

// The one v1 spell: {"spark": ARCANA 10} — rawDamage = ARCANA effectiveness (no
// weapon factor); casting cost is the ARCANA cooldown (A-31; D-LORE-2 — no mana
// pool field exists, and none may be invented on Character).
AttackResult castSpark(Character &caster, Character &target, WorldContext &ctx);

// Step 10 — drains the queue in insertion order (§4.4's Combat step).
class CombatResolutionSystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;
};

} // namespace harrowmere
