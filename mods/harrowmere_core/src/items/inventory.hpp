#ifndef HARROWMERE_CORE_ITEMS_INVENTORY_HPP
#define HARROWMERE_CORE_ITEMS_INVENTORY_HPP

// inventory.hpp — bags, item instances, and the inventory store (task 2.4).
//
// The card's Inventory record — "struct Inventory { std::map<std::uint64_t,
// std::uint32_t> stacks; std::vector<std::uint64_t> equipped; }" — carried
// verbatim, plus ONE additive member the card's own pitfall forces: loose
// item INSTANCES. `equippedItems` stores item instance ids, not stack keys —
// "two iron swords score twice" (card 2.4 pitfalls) — so an unequipped
// iron sword must live somewhere with its identity: `instances` below. The
// stack map stays the fungible side: stackable units (materials, bandages,
// rations, mana tonics) count by item definition id; instanced equippables
// move between `instances` (bag) and `equipped` (body) as whole instances.
// DEVIATIONS.md carries the one-line note.
//
// Ownership (D-STORE-1 boundaries): the Inventory record is NpcState-owned
// for NPCs ("NpcState-owned; players same shape", card 2.4) — task 3.4
// embeds one per NPC; the SP player's bag lives in InventoryStore below
// until the player mirror lands (7.3's Inventory panel reads it through the
// capability views). Both owners share exactly this shape, so every function
// in items/gear.hpp and items/crafting.hpp takes `Inventory &` and never
// cares who holds it.
//
// ItemInstanceRegistry — the instance-id space (card 2.4: "instance registry
// (itemIdCounter) must serialize in saves (7.1)"). Instance ids are drawn
// from ONE monotonic counter (the same never-reused discipline as
// core/types.hpp's IdGenerator, R10); the mapping instance -> item
// definition makes getGearScore well-defined (resolve the id, read the
// row's rarity/level) and is what a save round-trips (7.1's schema extends
// with the counter + the map in the same commit).
//
// InventoryStore — the player-side bag store (§4.5: all simulation state
// lives in stores owned by harrowmere_core). Auto-inserts an empty record on
// first touch: every character HAS a bag from birth — an empty bag is the
// default, not an error. std::map keyed by characterId (R7); no unordered
// container anywhere.

#include <cstdint>
#include <map>
#include <vector>

#include "core/types.hpp" // characterId (task 0.2, R10)

namespace harrowmere::items
{

// One loot row (card 3.3's Corpse::loot vocabulary): fungible units of one
// item definition. The bag's stacks map flattens into these when a corpse is
// staged (3.3's collectLoot); 7.1's save schema serializes the same shape.
struct ItemStack
{
    std::uint64_t itemKey = 0; // the item definition id (stacks' key)
    std::uint32_t count   = 0; // units in this row
};

// The bag record (card 2.4, verbatim members + the loose-instance side).
struct Inventory
{
    // Fungible stacks: item definition id -> unit count (stackable items;
    // count 0 entries are erased, never stored).
    std::map<std::uint64_t, std::uint32_t> stacks;

    // Item INSTANCE ids currently equipped (worn gear has left the bag).
    // Kept in lockstep with Character.equippedItems — that doc field is the
    // scoring truth (getGearScore sums it); this mirror is the bag-side view
    // the Inventory panel reads. A debug assertion pins the lockstep.
    std::vector<std::uint64_t> equipped;

    // Item INSTANCE ids sitting loose in the bag (unequipped equippables —
    // crafted/bought/looted swords, armor, masks). Craft/spawn appends;
    // equip consumes from here; unequip returns here.
    std::vector<std::uint64_t> instances;
};

// The item-instance id space (card 2.4's itemIdCounter). Ids start at 1
// (0 is reserved in every id space, core/types.hpp's FIRST_ID rule).
class ItemInstanceRegistry
{
public:
    static constexpr std::uint64_t FIRST_INSTANCE_ID = 1;

    ItemInstanceRegistry() = default;

    // Draw a fresh instance id for one unit of item definition `itemDefId`.
    // Monotonic, never reused (R10); ids ascend across the whole run.
    std::uint64_t spawn(std::uint32_t itemDefId);

    // The definition an instance was spawned for; asserts existence
    // (programmer error — every caller got the id from spawn() or a loaded
    // save). Release: deterministic 0 (the zero item definition is never a
    // real row, so callers' lookups miss cleanly).
    [[nodiscard]] std::uint32_t itemDefOf(std::uint64_t instanceId) const;

    [[nodiscard]] bool exists(std::uint64_t instanceId) const noexcept;

    std::size_t size() const noexcept
    {
        return instances_.size();
    }

    // ---- save seams (task 7.1 serializes both; load merges never rewinds) ----

    // The counter's next-to-issue value + the full instance map (R7:
    // std::map, ascending key order).
    [[nodiscard]] std::uint64_t counter() const noexcept
    {
        return itemIdCounter_;
    }
    const std::map<std::uint64_t, std::uint32_t> &all() const noexcept
    {
        return instances_;
    }

    // Adopt a saved counter (a fresh-world registry keeps its default).
    // Values below FIRST_INSTANCE_ID clamp up; never rewinds a live counter.
    void adoptCounter(std::uint64_t next);

    // Restore one saved instance (load path; a duplicate id is a programmer
    // error — assert in debug, keep the existing mapping in release).
    void restore(std::uint64_t instanceId, std::uint32_t itemDefId);

private:
    std::uint64_t itemIdCounter_ = FIRST_INSTANCE_ID;
    std::map<std::uint64_t, std::uint32_t> instances_; // instance id -> item definition id (R7)
};

// The player-side bag store (§4.5). NPC bags join NpcState (task 3.4) with
// the same record shape — the store exists so the SP player's bag has its
// storage truth before that lands.
class InventoryStore
{
public:
    // The character's bag; inserts an empty record on first touch (an empty
    // bag is every character's birth state — never an error).
    Inventory &bag(characterId id);
    [[nodiscard]] const Inventory &bag(characterId id) const;

    [[nodiscard]] bool has(
        characterId id
    ) const noexcept
    {
        return bags_.find(id) != bags_.end();
    }

    // Deterministic iteration (R7).
    const std::map<characterId, Inventory> &all() const noexcept
    {
        return bags_;
    }

private:
    std::map<characterId, Inventory> bags_; // characterId -> bag (R7)
};

// ---- stack helpers (the one write path for the fungible side) ----------------

// Add `count` units of `itemDefId`. Count 0 is a no-op (never inserts).
void addStacked(Inventory &bag, std::uint64_t itemDefId, std::uint32_t count);

// Take `count` units of `itemDefId`; returns false (and changes nothing)
// when the bag holds fewer. Erases the entry at zero — a stack map never
// stores a 0.
[[nodiscard]] bool takeStacked(Inventory &bag, std::uint64_t itemDefId, std::uint32_t count);

[[nodiscard]] std::uint32_t countStacked(const Inventory &bag, std::uint64_t itemDefId);

// ---- loose-instance helpers --------------------------------------------------

// Append an instance id to the bag's loose list (spawn paths).
void addLooseInstance(Inventory &bag, std::uint64_t instanceId);

// Remove one occurrence of `instanceId` from the loose list; false when it
// is not there (the caller decides whether that is a rule error).
[[nodiscard]] bool takeLooseInstance(Inventory &bag, std::uint64_t instanceId);

// True when the instance sits in the bag (loose OR equipped — ownership
// check for equip/consume paths).
[[nodiscard]] bool holdsInstance(const Inventory &bag, std::uint64_t instanceId);

} // namespace harrowmere::items

#endif // HARROWMERE_CORE_ITEMS_INVENTORY_HPP
