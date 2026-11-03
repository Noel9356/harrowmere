// gear.cpp — item scoring, equip/unequip, consumption, and the guard-gear
// theft hook (task 2.4). See gear.hpp for the pinned semantics.

#include "items/gear.hpp"

#include <cassert>
#include <algorithm>

#include "character/character.hpp"
#include "combat/justice_sink.hpp"
#include "data/items.hpp"
#include "data/registry.hpp"
#include "world/world.hpp" // WorldStore::socialClassOf (D-JUR-1)

namespace harrowmere
{

std::uint64_t getItemScore(
    const data::ItemData &item
)
{
    // design reference §Gear Score, verbatim (A-17/A-18/A-19):
    //   getItemScore(item):
    //       return BASE_ITEM_SCORE + (W_RARITY * item.rarity / SCALE)
    //            + (W_LEVEL * item.level / SCALE)
    // Multiply before divide; each term truncates separately (R2) — the
    // doc's own form, never "improved" into a combined fraction.
    return BASE_ITEM_SCORE + (W_RARITY * static_cast<std::uint64_t>(item.rarity) / SCALE)
         + (W_LEVEL * static_cast<std::uint64_t>(item.level) / SCALE);
}

std::uint64_t getGearScore(
    const data::DataRegistry &data, const items::ItemInstanceRegistry &instances, const Character &c
)
{
    // design reference verbatim: the sum over c.equippedItems — instance ids
    // (two iron swords score twice), resolved instance -> definition -> row.
    std::uint64_t total = 0;
    for (const std::uint64_t instanceId : c.equippedItems)
    {
        if (!instances.exists(instanceId))
        {
            assert(false && "getGearScore: equipped instance id missing from the registry (programmer error)");
            continue; // deterministic release recovery: skip the entry
        }
        const data::ItemData *item = data.itemById(instances.itemDefOf(instanceId));
        if (item == nullptr)
        {
            assert(false && "getGearScore: instance's item definition missing from the registry (programmer error)");
            continue; // deterministic release recovery: skip the entry
        }
        total += getItemScore(*item);
    }
    return total;
}

GearResult equip(
    Character &c,
    items::Inventory &bag,
    const items::ItemInstanceRegistry &instances,
    const data::DataRegistry &data,
    std::uint64_t itemInstanceId
)
{
    if (!instances.exists(itemInstanceId))
        return GearResult::err(GearError::UnknownInstance);

    const data::ItemData *item = data.itemById(instances.itemDefOf(itemInstanceId));
    if (item == nullptr)
        return GearResult::err(GearError::UnknownItem);
    if (!item->equippable)
        return GearResult::err(GearError::NotEquippable);

    // The instance must sit loose in THIS bag (equipping from someone else's
    // bag is not a path — loot moves the instance first, 3.3's corpse loot).
    if (!items::takeLooseInstance(bag, itemInstanceId))
        return GearResult::err(GearError::NotOwned);

    // Body first (the doc field), then the bag mirror; both stay in lockstep.
    c.equippedItems.push_back(itemInstanceId);
    bag.equipped.push_back(itemInstanceId);

    // 2.5's audit: equipping enters the power formula.
    markPowerDirty(c);
    return GearResult::ok();
}

GearResult unequip(
    Character &c,
    items::Inventory &bag,
    const items::ItemInstanceRegistry &instances,
    const data::DataRegistry &data,
    std::uint64_t itemInstanceId
)
{
    if (!instances.exists(itemInstanceId))
        return GearResult::err(GearError::UnknownInstance);

    const data::ItemData *item = data.itemById(instances.itemDefOf(itemInstanceId));
    if (item == nullptr)
        return GearResult::err(GearError::UnknownItem);

    // Remove from the body (the doc field) — must be equipped to unequip.
    bool removed = false;
    for (std::size_t i = 0; i < c.equippedItems.size(); ++i)
    {
        if (c.equippedItems[i] == itemInstanceId)
        {
            c.equippedItems.erase(c.equippedItems.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
            break;
        }
    }
    if (!removed)
        return GearResult::err(GearError::NotEquipped);

    // The bag mirror, in lockstep (the invariant inventory.hpp pins).
    bool removedMirror = false;
    for (std::size_t i = 0; i < bag.equipped.size(); ++i)
    {
        if (bag.equipped[i] == itemInstanceId)
        {
            bag.equipped.erase(bag.equipped.begin() + static_cast<std::ptrdiff_t>(i));
            removedMirror = true;
            break;
        }
    }
    assert(removedMirror && "unequip: Character.equippedItems and the bag mirror diverged (programmer error)");
    (void)removedMirror; // NDEBUG (release) drops the assert; keep the variable used (DoD: -Werror)

    // Back to the bag as a loose instance.
    items::addLooseInstance(bag, itemInstanceId);

    // 2.5's audit: unequipping enters the power formula (and 2.5's
    // TestPower_PeakNeverDecreases runs on exactly this path).
    markPowerDirty(c);
    return GearResult::ok();
}

ConsumeResult consumeItem(
    Character &c, items::Inventory &bag, const data::DataRegistry &data, const std::string &itemKey
)
{
    const data::ItemData *item = data.itemByKey(itemKey);
    if (item == nullptr)
        return ConsumeResult::err(ConsumeError::UnknownItem);
    if (!item->hasConsumable)
        return ConsumeResult::err(ConsumeError::NotConsumable);

    // Consume one unit first — a refused take leaves no side effect.
    if (!items::takeStacked(bag, item->id, 1))
        return ConsumeResult::err(ConsumeError::NotOwned);

    // Heal, clamped to maxHp (gain never borrows the damage pipeline's
    // clamp: currentHp stops AT maxHp, never wraps, never exceeds).
    if (item->consumable.healHp > 0)
    {
        const std::uint64_t healed = std::min<std::uint64_t>(item->consumable.healHp, c.body.maxHp - c.body.currentHp);
        c.body.currentHp += healed;
    }

    // XP grant (design reference: "XP is obtained from ... consuming some
    // items"). totalXp accumulates (u64, never decreases); availableXp goes
    // through applyXpDelta so the -MAX_XP floor stays the one clamp (R4) —
    // the card's pitfall, honored exactly.
    if (item->consumable.xpGain > 0)
    {
        const std::uint64_t gain = item->consumable.xpGain;
        c.progression.totalXp += gain; // never decreases (design reference)
        c.progression.availableXp = applyXpDelta(c.progression.availableXp, static_cast<std::int64_t>(gain));
        // 2.5's audit: the XP grant enters the power formula (skills/stats
        // bought from it will dirty again — the grant itself is the site).
        markPowerDirty(c);
    }

    return ConsumeResult::ok();
}

bool isGuardMarked(
    const data::ItemData &item
)
{
    return item.guardMarked;
}

bool guardGearEntitled(
    const Character &holder, const world::WorldStore *world, territoryId atTerritory, bool npcRoleEntitled
)
{
    // Guard-role NPCs are entitled by their role (the flag the caller passes
    // until NpcState lands — 3.4/3.6 own the role read).
    if (npcRoleEntitled)
        return true;

    // The guard/royal title half: the territory tree's social class at the
    // selling point (D-JUR-1's single walk; NOBLE covers the titled gentry
    // that legally fields guards, ROYAL the crown).
    if (world != nullptr)
    {
        const SocialClassId cls = world->socialClassOf(holder.id, atTerritory);
        if (cls == SocialClassId::NOBLE || cls == SocialClassId::ROYAL)
            return true;
    }
    return false;
}

void onGuardGearSold(
    const Character &seller,
    const data::ItemData &item,
    const world::WorldStore *world,
    territoryId atTerritory,
    JusticeSink *justice
)
{
    if (!item.guardMarked)
        return; // only territory-marked guard gear flags anything

    // Entitled holders sell in peace; everyone else flags the theft. The
    // evidence itself (CrimeType::THEFT at `where`) is 4.1's; the null sink
    // keeps the call a deterministic no-op until then (D-SINK-1).
    if (guardGearEntitled(seller, world, atTerritory, /*npcRoleEntitled=*/false))
        return;

    if (justice != nullptr)
        justice->holdsGuardGearTheft(seller.id, seller.body.position);
}

} // namespace harrowmere
