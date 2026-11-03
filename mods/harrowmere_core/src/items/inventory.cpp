// inventory.cpp — bags, item instances, and the inventory store (task 2.4).
//
// Error handling (§3.4): a bag that lacks what a caller takes is a RULE
// outcome (takeStacked returns false — callers report it); a registry miss
// on a live instance id is a PROGRAMMER error (assert in debug, recover
// deterministically in release — the same split store.cpp pins).

#include "items/inventory.hpp"

#include <cassert>

namespace harrowmere::items
{

std::uint64_t ItemInstanceRegistry::spawn(
    std::uint32_t itemDefId
)
{
    // Monotonic draw (R10): the counter IS the next id; the map records what
    // the id names so gear scoring can resolve it back.
    const std::uint64_t instanceId = itemIdCounter_++;
    instances_.emplace(instanceId, itemDefId);
    return instanceId;
}

std::uint32_t ItemInstanceRegistry::itemDefOf(
    std::uint64_t instanceId
) const
{
    const auto it = instances_.find(instanceId);
    assert(it != instances_.end() && "itemDefOf: unknown item instance id (programmer error)");
    if (it == instances_.end())
        return 0; // deterministic release recovery: the zero definition never names a real row
    return it->second;
}

bool ItemInstanceRegistry::exists(
    std::uint64_t instanceId
) const noexcept
{
    return instances_.find(instanceId) != instances_.end();
}

void ItemInstanceRegistry::adoptCounter(
    std::uint64_t next
)
{
    if (next > itemIdCounter_)
        itemIdCounter_ = next; // never rewind a live counter (R10: ids are never reused)
}

void ItemInstanceRegistry::restore(
    std::uint64_t instanceId, std::uint32_t itemDefId
)
{
    const auto inserted = instances_.emplace(instanceId, itemDefId);
    assert(
        (inserted.second || inserted.first->second == itemDefId)
        && "restore: duplicate instance id with a different definition (programmer error)"
    );
    (void)inserted; // NDEBUG (release) drops the assert; keep the variable used (DoD: -Werror)
    if (instanceId >= itemIdCounter_)
        itemIdCounter_ = instanceId + 1; // the counter always sits above every restored id
}

Inventory &InventoryStore::bag(
    characterId id
)
{
    return bags_[id]; // auto-insert: an empty bag is the birth default
}

const Inventory &InventoryStore::bag(
    characterId id
) const
{
    static const Inventory empty{};
    const auto it = bags_.find(id);
    return it != bags_.end() ? it->second : empty;
}

void addStacked(
    Inventory &bag, std::uint64_t itemDefId, std::uint32_t count
)
{
    if (count == 0)
        return; // a zero count never inserts (the map stores no 0 entries)
    bag.stacks[itemDefId] += count;
}

bool takeStacked(
    Inventory &bag, std::uint64_t itemDefId, std::uint32_t count
)
{
    if (count == 0)
        return true; // taking nothing always succeeds
    const auto it = bag.stacks.find(itemDefId);
    if (it == bag.stacks.end() || it->second < count)
        return false; // rule outcome: not enough units
    it->second -= count;
    if (it->second == 0)
        bag.stacks.erase(it); // a stack map never stores a 0
    return true;
}

std::uint32_t countStacked(
    const Inventory &bag, std::uint64_t itemDefId
)
{
    const auto it = bag.stacks.find(itemDefId);
    return it != bag.stacks.end() ? it->second : 0;
}

void addLooseInstance(
    Inventory &bag, std::uint64_t instanceId
)
{
    bag.instances.push_back(instanceId);
}

bool takeLooseInstance(
    Inventory &bag, std::uint64_t instanceId
)
{
    for (std::size_t i = 0; i < bag.instances.size(); ++i)
    {
        if (bag.instances[i] == instanceId)
        {
            bag.instances.erase(bag.instances.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool holdsInstance(
    const Inventory &bag, std::uint64_t instanceId
)
{
    for (const std::uint64_t id : bag.instances)
        if (id == instanceId)
            return true;
    for (const std::uint64_t id : bag.equipped)
        if (id == instanceId)
            return true;
    return false;
}

} // namespace harrowmere::items
