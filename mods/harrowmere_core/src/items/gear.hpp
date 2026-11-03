#ifndef HARROWMERE_CORE_ITEMS_GEAR_HPP
#define HARROWMERE_CORE_ITEMS_GEAR_HPP

// gear.hpp — item scoring, equipping, item consumption, and the guard-gear
// theft hook (task 2.4).
//
// Doc-verbatim math (design reference §Gear Score, A-17..A-19):
//     getItemScore(item) = BASE_ITEM_SCORE + W_RARITY*rarity/SCALE
//                        + W_LEVEL*level/SCALE
//     getGearScore(c)    = Σ getItemScore(item) over c.equippedItems
// Integer division truncates (R2) — each term truncates separately, exactly
// as the doc's form writes it.
//
// Equipping has NO stat requirements in v1 (none in the docs); the single
// exception is ownership legitimacy of guardMarked items, which never blocks
// equipping — it FLAGS (a player acquiring one is illegitimate, and SELLING
// one emits the theft path; design rule: "Selling territory-marked guard
// gear flags theft evidence"). The flag itself lives on NpcState
// (holdsGuardGear, task 3.4) — not on the doc Character, never here.
//
// markPowerDirty fires on equip, unequip, and every XP-granting consume
// (2.5's call-site audit names exactly these three from this file).
//
// Registry injection (the repo's one deliberate signature adaptation, the
// same reading species.hpp pins): the docs' pseudocode reads a global
// itemData[...] table; Harrowmere's data lives in the injected DataRegistry
// and instance identity in the injected ItemInstanceRegistry, so both are
// parameters — the card's `equip(Character&, itemInstanceId)` gains them,
// the character and instance-id arguments keep their pinned positions.

#include <cstdint>
#include <string>

#include "core/result.hpp"       // Result<void, E> (task 0.2)
#include "core/types.hpp"        // characterId, Position (task 0.2)
#include "items/inventory.hpp"   // Inventory, ItemInstanceRegistry
#include "progression/power.hpp" // markPowerDirty (2.5's seam)

namespace harrowmere::data
{
struct ItemData;    // items.hpp (task 1.1)
class DataRegistry; // registry.hpp (task 1.3)
} // namespace harrowmere::data

namespace harrowmere::world
{
class WorldStore; // world.hpp (task 1.4) — the title walk for guard-gear entitlement
} // namespace harrowmere::world

namespace harrowmere
{
class JusticeSink; // combat/justice_sink.hpp (D-SINK-1) — forward-declared (harrowmere scope)
} // namespace harrowmere

namespace harrowmere
{

// ---- item & gear scores (design reference §Gear Score, verbatim) --------------

// getItemScore — BASE_ITEM_SCORE (A-17) + W_RARITY * rarity / SCALE (A-18)
// + W_LEVEL * level / SCALE (A-19). rarity rides the SCALE grid (0..10000,
// items.json); each term truncates separately (R2), the doc's own form.
[[nodiscard]] std::uint64_t getItemScore(const data::ItemData &item);

// getGearScore — the sum over c.equippedItems (design reference verbatim).
// Every instance id resolves through `instances` to its definition row in
// `data`; an unknown id or a definition that misses the registry is a
// programmer error (debug assert; release skips the entry deterministically
// — never a crash, never a fake score).
[[nodiscard]] std::uint64_t
getGearScore(const data::DataRegistry &data, const items::ItemInstanceRegistry &instances, const Character &c);

// ---- equip / unequip ------------------------------------------------------------

enum class GearError
{
    NotEquippable,   // the item definition has no equip slot
    NotOwned,        // the instance is neither loose nor equipped in this bag
    NotEquipped,     // unequip: the instance is in the bag but not on the body
    UnknownInstance, // the instance id does not exist in the registry
    UnknownItem      // the instance's definition id misses the registry
};

using GearResult = Result<void, GearError>;

// equip — move one loose instance from the bag onto the body: appends to
// Character.equippedItems (the doc field, the scoring truth) and the bag's
// equipped mirror, removes it from the loose list, fires markPowerDirty.
// Consumables/stackables are not equippable (rule refusal, no side effect).
GearResult equip(
    Character &c,
    items::Inventory &bag,
    const items::ItemInstanceRegistry &instances,
    const data::DataRegistry &data,
    std::uint64_t itemInstanceId
);

// unequip — the reverse path: removes from both equipped lists, returns the
// instance to the bag's loose list, fires markPowerDirty.
GearResult unequip(
    Character &c,
    items::Inventory &bag,
    const items::ItemInstanceRegistry &instances,
    const data::DataRegistry &data,
    std::uint64_t itemInstanceId
);

// ---- item consumption ------------------------------------------------------------

enum class ConsumeError
{
    UnknownItem,   // no items.json row carries this key
    NotConsumable, // the row has no consumable block
    NotOwned       // the bag holds no unit of the item
};

using ConsumeResult = Result<void, ConsumeError>;

// consumeItem — consume one unit of the keyed item from the bag (design
// reference: "XP is obtained from killing or consuming some items"):
//   * heals body.currentHp by the row's healHp, CLAMPED to maxHp (never
//     above; heals have their own path — the damage pipeline's clamp is
//     never reused for gain),
//   * grants XP: totalXp += xpGain (u64, never decreases) and
//     availableXp = applyXpDelta(availableXp, +xpGain) (R4's clamp — the
//     card's pitfall: consumption never bypasses it),
//   * fires markPowerDirty (the XP grant is 2.5's audited call site).
ConsumeResult
consumeItem(Character &c, items::Inventory &bag, const data::DataRegistry &data, const std::string &itemKey);

// ---- guard gear ------------------------------------------------------------------

// True when the item row is territory-marked guard gear.
[[nodiscard]] bool isGuardMarked(const data::ItemData &item);

// The entitlement check behind the theft rule: a guardMarked item is
// legitimately held by a guard-role NPC (role lives on NpcState — 3.4/3.6
// supply the role read; the v1 caller passes what it knows) or by a
// character carrying a guard/royal title — the doc's "guard/royal title",
// read as the territory tree's NOBLE or ROYAL social class at `atTerritory`
// (WorldStore::socialClassOf, D-JUR-1's single walk).
//
// `npcRoleEntitled` carries the NPC-role half until NpcState lands: guard-
// role NPCs pass true (their role IS the entitlement); 3.4/3.6 replace the
// flag with the real role read without touching this signature.
[[nodiscard]] bool guardGearEntitled(
    const Character &holder, const world::WorldStore *world, territoryId atTerritory, bool npcRoleEntitled
);

// The theft hook (card 2.4's Done-when): the vendor sell path (3.7) calls
// this for every sold stack whose item isGuardMarked. An entitled seller is
// silent; anyone else emits holdsGuardGearTheft on the justice sink — which
// records CrimeType::THEFT evidence (4.1). A null sink compiles and no-ops
// (the null default is the point: the hook's signature never changes when
// 4.1 wires the real sink in, D-SINK-1).
void onGuardGearSold(
    const Character &seller,
    const data::ItemData &item,
    const world::WorldStore *world,
    territoryId atTerritory,
    JusticeSink *justice
);

} // namespace harrowmere

#endif // HARROWMERE_CORE_ITEMS_GEAR_HPP
