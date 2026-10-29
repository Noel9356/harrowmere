// items.hpp — typed view over data/items.json.
//
// Loaded by ItemsLoader (built on task 0.4's JSON layer). Items back the
// Character.equippedItems array, vendor stocks, loot tables, and recipe
// outputs (2.4). The runtime form is a flat vector indexed by lookup; the
// hot path uses itemById()/itemByKey() rather than scanning JSON.
//
// Schema (task 1.1 spec):
//   { "id", "key", "name", "rarity": 0..SCALE, "level", "baseValue",
//     "equippable": bool, "slot"?, "guardMarked": bool,
//     "consumable": { "xpGain": n, "healHp": n, "manaRich": bool }?,
//     "stackable": bool, "weaponFactor"?, "enablesAct"? }
//
// `guardMarked` feeds the justice hook (task 2.4/4.6: "Selling territory-
// marked guard gear flags theft evidence"). `manaRich` consumables grant XP
// ("XP is obtained from killing or consuming some items" — design reference).
// `enablesAct` ties an equippable to a SuspicionAct toggle (e.g., the cloth
// mask enables MASKED — task 3.10).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/enums.hpp"  // SuspicionAct
#include "core/types.hpp"  // (task 0.2 id aliases — items use plain u32 ids for now)

namespace harrowmere::data {

struct DataError;  // forward decl (task 0.4)

// ItemSlot — the equip slot vocabulary. Closed list (extend via deviation).
// String keys in items.json ("main_hand", "off_hand", "head", "face", ...)
// map to these enum values at load time.
enum class ItemSlot : std::uint8_t {
    NONE = 0,  // not equippable / no slot
    MAIN_HAND,
    OFF_HAND,
    HEAD,
    FACE,
    BODY,
    HANDS,
    LEGS,
    FEET,
    BACK,
    COUNT
};

// Consumable — optional sub-object. Any field may be absent; absent means
// "this consumable does not do that". A row with no consumable sub-object
// is not a consumable (equippable or pure currency item).
struct Consumable {
    std::uint32_t xpGain  = 0;  // XP granted on consume
    std::uint32_t healHp  = 0;  // HP restored on consume
    bool          manaRich = false;  // marks the item as mana-rich (XP source per doc)
};

// ItemData — runtime form of one items.json row.
struct ItemData {
    std::uint32_t id = 0;
    std::string   key;
    std::string   name;
    std::uint32_t rarity = 0;       // 0..SCALE
    std::uint32_t level = 0;
    std::int64_t  baseValue = 0;    // currency (i64 — may go negative in debt scenarios)
    bool          equippable = false;
    ItemSlot      slot = ItemSlot::NONE;
    bool          guardMarked = false;
    bool          hasConsumable = false;
    Consumable    consumable{};
    bool          stackable = false;

    // Optional combat fields — weaponFactor multiplies strength in the
    // rawDamage formula (A-29 WEAPON_FACTOR = 1500 default for weapons,
    // 800 unarmed; items override per-row). Stored as scaled u32
    // (1.5 → 15000) for fixed-point consistency with §3.2.
    std::uint32_t weaponFactor = 0;  // 0 = not a weapon / unarmed default applies elsewhere

    // Optional suspicion link — equipping this item enables the named act
    // (e.g., cloth_mask → SuspicionAct::MASKED). NONE = no link.
    SuspicionAct enablesAct = static_cast<SuspicionAct>(-1);  // sentinel: "no act"
};

// ItemsLoader — parses + validates data/items.json.
class ItemsLoader {
public:
    ItemsLoader() = default;
    ~ItemsLoader() = default;
    ItemsLoader(const ItemsLoader&) = delete;
    ItemsLoader& operator=(const ItemsLoader&) = delete;

    bool load(const std::string& jsonPath, std::vector<DataError>& errors);

    const ItemData* itemById(std::uint32_t id) const;
    const ItemData* itemByKey(const std::string& key) const;

    bool        empty() const noexcept { return items_.empty(); }
    std::size_t size()  const noexcept { return items_.size(); }
    const std::vector<ItemData>& all() const noexcept { return items_; }

private:
    std::vector<ItemData> items_;
};

} // namespace harrowmere::data
