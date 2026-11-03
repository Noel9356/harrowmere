// test_items.cpp — Task 2.4's pinned test suite.
//
// Implements the six tests pinned in phase-2-alive.md card 2.4's Tests
// block (names verbatim — common.md §3.5):
//
//   * TestGearScore_MatchesDocFormula               — getItemScore /
//                                                     getGearScore verbatim
//                                                     (A-17..A-19)
//   * TestEquipUnequip_MarksPowerDirty              — both paths fire
//                                                     markPowerDirty (pairs
//                                                     with 2.5)
//   * TestConsume_HealsAndGrantsXp_TotalXpNeverDecreases — heal clamps,
//                                                     XP flows through
//                                                     applyXpDelta, totalXp
//                                                     only grows
//   * TestCraft_InputsConsumedOutputGained          — the recipe consumes
//                                                     and produces exactly
//   * TestCraft_SkillRequirementBlocks              — reqSkill refuses
//                                                     before any side effect
//   * TestGuardGear_SellEmitsTheftSignal            — the theft hook fires
//                                                     the sink for a
//                                                     non-entitled seller
//
// Supporting tests: guard-gear entitlement through the NOBLE/ROYAL title,
// the equip/unequip bag bookkeeping, and the RecipeTable load + cross-ref
// over the shipped recipes.json.
//
// Test harness: doctest v2.4.12 (vendored per task 0.6); no main here —
// tests/main.cpp owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "character/character.hpp"
#include "combat/justice_sink.hpp"
#include "core/enums.hpp"
#include "core/types.hpp"
#include "data/items.hpp"
#include "data/registry.hpp"
#include "items/crafting.hpp"
#include "items/gear.hpp"
#include "items/inventory.hpp"
#include "player/controller.hpp"
#include "world/world.hpp"

#include <filesystem>

namespace
{

std::string itemsDataDir()
{
    // The same baked path the loader-facing tests use (tests/CMakeLists.txt).
#ifdef HARROWMERE_CORE_DATA_DIR
    return std::string(HARROWMERE_CORE_DATA_DIR);
#else
    return std::string("mods/harrowmere_core/data");
#endif
}

// A committed registry over the shipped data — the same precondition the
// runtime guarantees (tasks 0.4/1.3). Also implicitly re-validates that the
// shipped recipes' inputs/outputs resolve (the test that follows loads
// recipes.json against it).
struct LoadedRegistry
{
    harrowmere::data::DataRegistry registry;

    LoadedRegistry()
    {
        std::vector<harrowmere::data::DataError> errors;
        const bool ok = registry.load(itemsDataDir(), errors);
        REQUIRE(ok);
        REQUIRE(errors.empty());
    }
};

// A recording justice sink: captures the guard-gear theft flag (and refuses
// everything else — this suite never commits crimes). JusticeSink is the
// merged D-SINK-1 face (harrowmere scope): recordCrime carries the CrimeContext
// (3.3's commit-time snapshot) instead of the pre-3.3 loose arguments.
struct RecordingJusticeSink final : harrowmere::JusticeSink
{
    std::size_t theftFlags               = 0;
    harrowmere::characterId theftCulprit = 0;
    harrowmere::Position theftWhere{};

    void recordCrime(
        const harrowmere::CrimeContext &
    ) override
    {
        FAIL("no test in this suite may record a crime through the sink");
    }

    void noteAssault(
        harrowmere::characterId, harrowmere::characterId, std::uint64_t
    ) override
    {
        FAIL("no test in this suite may note an assault through the sink");
    }

    [[nodiscard]] bool hasWarrantIn(
        harrowmere::characterId, harrowmere::territoryId
    ) const override
    {
        return false;
    }

    [[nodiscard]] bool hasLethalWarrantIn(
        harrowmere::characterId, harrowmere::territoryId
    ) const override
    {
        return false;
    }

    void holdsGuardGearTheft(
        harrowmere::characterId culprit, const harrowmere::Position &where
    ) override
    {
        ++theftFlags;
        theftCulprit = culprit;
        theftWhere   = where;
    }
};

harrowmere::Character makeCharacter(
    harrowmere::characterId id
)
{
    harrowmere::Character c{};
    c.id             = id;
    c.body.maxHp     = harrowmere::maxHpForLevel(1);
    c.body.currentHp = c.body.maxHp;
    return c;
}

} // namespace

// ---- pinned: TestGearScore_MatchesDocFormula ------------------------------------

TEST_CASE(
    "TestGearScore_MatchesDocFormula"
)
{
    LoadedRegistry loaded;
    const harrowmere::data::DataRegistry &registry = loaded.registry;

    // getItemScore — the doc formula evaluated BY HAND from the shipped
    // rows (A-17/A-18/A-19: BASE 10 + 40*rarity/SCALE + 60*level/SCALE,
    // each term truncating separately, R2):
    //   iron_sword   (rarity 2000, level 5):  10 + 40*2000/10000 + 60*5/10000   = 10 + 8 + 0 = 18
    //   guard_halberd(rarity 5000, level 12): 10 + 40*5000/10000 + 60*12/10000  = 10 + 20 + 0 = 30
    const harrowmere::data::ItemData *sword   = registry.itemByKey("iron_sword");
    const harrowmere::data::ItemData *halberd = registry.itemByKey("guard_halberd");
    REQUIRE(sword != nullptr);
    REQUIRE(halberd != nullptr);

    CHECK(
        harrowmere::getItemScore(*sword)
        == 10 + (40 * sword->rarity) / harrowmere::SCALE + (60 * sword->level) / harrowmere::SCALE
    );
    CHECK(harrowmere::getItemScore(*sword) == 18);
    CHECK(
        harrowmere::getItemScore(*halberd)
        == 10 + (40 * halberd->rarity) / harrowmere::SCALE + (60 * halberd->level) / harrowmere::SCALE
    );
    CHECK(harrowmere::getItemScore(*halberd) == 30);

    // getGearScore — the sum over c.equippedItems, instance-resolved: TWO
    // iron swords score TWICE (the card's own pitfall example).
    harrowmere::items::ItemInstanceRegistry instances;
    harrowmere::Character c = makeCharacter(1);

    const std::uint64_t swordA = instances.spawn(sword->id);
    const std::uint64_t swordB = instances.spawn(sword->id);
    c.equippedItems            = {swordA, swordB};

    CHECK(harrowmere::getGearScore(registry, instances, c) == 2 * harrowmere::getItemScore(*sword));

    // An empty body scores 0 (the gear term vanishes; the character still
    // has BASE power through stats/skills — 2.5's file).
    harrowmere::Character naked = makeCharacter(2);
    CHECK(harrowmere::getGearScore(registry, instances, naked) == 0);
}

// ---- pinned: TestEquipUnequip_MarksPowerDirty ------------------------------------

TEST_CASE(
    "TestEquipUnequip_MarksPowerDirty"
)
{
    LoadedRegistry loaded;
    const harrowmere::data::DataRegistry &registry = loaded.registry;
    const harrowmere::data::ItemData *sword        = registry.itemByKey("iron_sword");
    REQUIRE(sword != nullptr);

    harrowmere::items::ItemInstanceRegistry instances;
    harrowmere::items::InventoryStore bags;
    harrowmere::Character c           = makeCharacter(1);
    harrowmere::items::Inventory &bag = bags.bag(c.id);

    const std::uint64_t instance = instances.spawn(sword->id);
    harrowmere::items::addLooseInstance(bag, instance);

    // Clean cache: 2.5's getCurrentPower clears the flag...
    c.powerDirty = true;
    (void)harrowmere::getCurrentPower(c, registry, instances);
    CHECK_FALSE(c.powerDirty);

    // ...and EQUIP fires it (2.5's audited call site).
    const auto equipped = harrowmere::equip(c, bag, instances, registry, instance);
    REQUIRE(equipped.isOk());
    CHECK(c.powerDirty);

    // The move itself: off the loose list, onto the body + the bag mirror.
    CHECK(c.equippedItems.size() == 1);
    CHECK(c.equippedItems[0] == instance);
    CHECK(bag.equipped.size() == 1);
    CHECK(bag.instances.empty());

    // Clean again, then UNEQUIP fires it too.
    (void)harrowmere::getCurrentPower(c, registry, instances);
    CHECK_FALSE(c.powerDirty);
    const auto unequipped = harrowmere::unequip(c, bag, instances, registry, instance);
    REQUIRE(unequipped.isOk());
    CHECK(c.powerDirty);
    CHECK(c.equippedItems.empty());
    CHECK(bag.equipped.empty());
    REQUIRE(bag.instances.size() == 1);
    CHECK(bag.instances[0] == instance);

    // Rule refusals, no side effects: a consumable cannot equip; an unknown
    // or non-owned instance cannot either.
    const harrowmere::data::ItemData *bandage = registry.itemByKey("bandage");
    REQUIRE(bandage != nullptr);
    harrowmere::Character c2           = makeCharacter(2);
    harrowmere::items::Inventory &bag2 = bags.bag(c2.id);
    harrowmere::items::addStacked(bag2, bandage->id, 1);

    // The bandage is stackable, not instanced — spawn one to prove the refusal is about the ROW.
    const std::uint64_t bandageInstance = instances.spawn(bandage->id);
    harrowmere::items::addLooseInstance(bag2, bandageInstance);
    auto notEquippable = harrowmere::equip(c2, bag2, instances, registry, bandageInstance);
    REQUIRE(notEquippable.isErr());
    CHECK(notEquippable.error() == harrowmere::GearError::NotEquippable);
    CHECK(c2.equippedItems.empty());

    const auto unknown = harrowmere::equip(c2, bag2, instances, registry, 99999);
    REQUIRE(unknown.isErr());
    CHECK(unknown.error() == harrowmere::GearError::UnknownInstance);
}

// ---- pinned: TestConsume_HealsAndGrantsXp_TotalXpNeverDecreases -------------------

TEST_CASE(
    "TestConsume_HealsAndGrantsXp_TotalXpNeverDecreases"
)
{
    LoadedRegistry loaded;
    const harrowmere::data::DataRegistry &registry = loaded.registry;

    harrowmere::Character c = makeCharacter(1);
    harrowmere::items::InventoryStore bags;
    harrowmere::items::Inventory &bag = bags.bag(c.id);

    const harrowmere::data::ItemData *bandage   = registry.itemByKey("bandage");
    const harrowmere::data::ItemData *manaTonic = registry.itemByKey("mana_tonic");
    REQUIRE(bandage != nullptr);
    REQUIRE(manaTonic != nullptr);

    // Hurt the character: 120 max, 60 current.
    c.body.maxHp     = 120;
    c.body.currentHp = 60;

    harrowmere::items::addStacked(bag, bandage->id, 3);
    harrowmere::items::addStacked(bag, manaTonic->id, 2); // the XP half of the pinned test

    // A bandage heals 40 — BELOW maxHp, so it lands in full.
    REQUIRE(harrowmere::consumeItem(c, bag, registry, "bandage").isOk());
    CHECK(c.body.currentHp == 100);
    CHECK(harrowmere::items::countStacked(bag, bandage->id) == 2);
    CHECK(c.progression.totalXp == 0); // bandages heal, they do not teach

    // A heal that would cross maxHp CLAMPS there (gain never wraps, never
    // exceeds — the damage pipeline's clamp is not reused for heals).
    REQUIRE(harrowmere::consumeItem(c, bag, registry, "bandage").isOk());
    CHECK(c.body.currentHp == 120);
    CHECK(harrowmere::items::countStacked(bag, bandage->id) == 1);

    // The mana tonic grants XP (the doc's "consuming some items"): 200 per
    // tonic. totalXp accumulates and NEVER decreases across consumes;
    // availableXp rides applyXpDelta (R4's one clamp) — checked equal to the
    // sum of gains here, where no debt exists.
    const std::uint64_t totalXpBefore = c.progression.totalXp;
    REQUIRE(harrowmere::consumeItem(c, bag, registry, "mana_tonic").isOk());
    CHECK(c.progression.totalXp == totalXpBefore + manaTonic->consumable.xpGain);
    CHECK(c.progression.availableXp == static_cast<std::int64_t>(manaTonic->consumable.xpGain));
    CHECK(c.powerDirty); // 2.5's audit: the XP grant dirties the cache

    // Consume from deep XP debt: the grant still routes through applyXpDelta
    // (the card's pitfall — never a raw assignment).
    c.progression.availableXp = -harrowmere::MAX_XP; // floored exactly at -MAX_XP
    REQUIRE(harrowmere::consumeItem(c, bag, registry, "mana_tonic").isOk());
    CHECK(c.progression.totalXp == totalXpBefore + manaTonic->consumable.xpGain * 2);
    CHECK(c.progression.availableXp > -harrowmere::MAX_XP); // the clamp moved by exactly the gain

    // Rule refusals: unknown key, non-consumable row, a consumable with an
    // empty stack — and a refused consume leaves NOTHING behind.
    CHECK(harrowmere::consumeItem(c, bag, registry, "no_such_item").isErr());
    CHECK(harrowmere::consumeItem(c, bag, registry, "iron_sword").error() == harrowmere::ConsumeError::NotConsumable);
    CHECK(harrowmere::consumeItem(c, bag, registry, "iron_ration").error() == harrowmere::ConsumeError::NotOwned);
    CHECK(c.progression.totalXp == totalXpBefore + manaTonic->consumable.xpGain * 2);
}

// ---- pinned: TestCraft_InputsConsumedOutputGained ---------------------------------

TEST_CASE(
    "TestCraft_InputsConsumedOutputGained"
)
{
    LoadedRegistry loaded;
    const harrowmere::data::DataRegistry &registry = loaded.registry;

    harrowmere::items::ItemInstanceRegistry instances;
    harrowmere::items::InventoryStore bags;
    harrowmere::Character c           = makeCharacter(1);
    harrowmere::items::Inventory &bag = bags.bag(c.id);
    harrowmere::player::PlayerActions actions{};

    // The shipped table: six v1 recipes (bandage, torch, iron sword, leather
    // armor, rations, mana tonic) — every input/output key resolved against
    // items.json at load (a dangling key would have failed the load).
    harrowmere::RecipeTable recipes;
    std::vector<harrowmere::data::DataError> errors;
    REQUIRE(recipes.load(itemsDataDir() + "/recipes.json", registry, errors));
    REQUIRE(errors.empty());
    CHECK(recipes.size() == 6);

    // A clean cache (the doc-Character defaults powerDirty to true): the
    // no-dirty assertion at the end means "crafting did not dirty it".
    (void)harrowmere::getCurrentPower(c, registry, instances);
    CHECK_FALSE(c.powerDirty);

    // The iron sword: 3 iron + 1 wood + 1 tools, CRAFTING 8 — an instanced
    // output (a fresh instance id lands in the bag's loose list).
    const harrowmere::data::ItemData *iron  = registry.itemByKey("iron");
    const harrowmere::data::ItemData *wood  = registry.itemByKey("wood");
    const harrowmere::data::ItemData *tools = registry.itemByKey("tools");
    const harrowmere::data::ItemData *sword = registry.itemByKey("iron_sword");
    REQUIRE(iron != nullptr);
    REQUIRE(wood != nullptr);
    REQUIRE(tools != nullptr);
    REQUIRE(sword != nullptr);

    c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::CRAFTING)] = 8;
    harrowmere::items::addStacked(bag, iron->id, 3);
    harrowmere::items::addStacked(bag, wood->id, 1);
    harrowmere::items::addStacked(bag, tools->id, 1);
    const std::uint64_t instancesBefore = instances.size();

    REQUIRE(harrowmere::craft(c, bag, instances, registry, recipes, "craft_iron_sword", &actions).isOk());

    // Inputs consumed to zero (the stack map stores no zero entries)...
    CHECK(harrowmere::items::countStacked(bag, iron->id) == 0);
    CHECK_FALSE(bag.stacks.contains(iron->id));
    CHECK_FALSE(bag.stacks.contains(wood->id));
    CHECK_FALSE(bag.stacks.contains(tools->id));

    // ...and the output is one new sword instance in the bag.
    CHECK(instances.size() == instancesBefore + 1);
    REQUIRE(bag.instances.size() == 1);
    CHECK(instances.itemDefOf(bag.instances[0]) == sword->id);
    CHECK(actions.crafts == 1); // 2.3's reqActions counter through the write path

    // The mana tonic: a stackable output joins the stack map (+1), and the
    // multi-skill requirement (CRAFTING 6 + ARCANA 5 — the lore hook) holds.
    const harrowmere::data::ItemData *medicine = registry.itemByKey("medicine");
    const harrowmere::data::ItemData *manaHerb = registry.itemByKey("mana_herb");
    const harrowmere::data::ItemData *tonic    = registry.itemByKey("mana_tonic");
    REQUIRE(medicine != nullptr);
    REQUIRE(manaHerb != nullptr);
    REQUIRE(tonic != nullptr);

    c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::ARCANA)] = 5;
    harrowmere::items::addStacked(bag, medicine->id, 2);
    harrowmere::items::addStacked(bag, manaHerb->id, 1);

    REQUIRE(harrowmere::craft(c, bag, instances, registry, recipes, "craft_mana_tonic", &actions).isOk());
    CHECK(harrowmere::items::countStacked(bag, tonic->id) == 1);
    CHECK_FALSE(bag.stacks.contains(medicine->id));
    CHECK(actions.crafts == 2);

    // No XP from crafting (the docs' only XP sources: kills + consumed
    // items) and no power dirty (bag-only change; 2.5's audited list does
    // not name crafting).
    CHECK(c.progression.totalXp == 0);
    CHECK_FALSE(c.powerDirty);
}

// ---- pinned: TestCraft_SkillRequirementBlocks -------------------------------------

TEST_CASE(
    "TestCraft_SkillRequirementBlocks"
)
{
    LoadedRegistry loaded;
    const harrowmere::data::DataRegistry &registry = loaded.registry;

    harrowmere::items::ItemInstanceRegistry instances;
    harrowmere::items::InventoryStore bags;
    harrowmere::Character c           = makeCharacter(1);
    harrowmere::items::Inventory &bag = bags.bag(c.id);

    harrowmere::RecipeTable recipes;
    std::vector<harrowmere::data::DataError> errors;
    REQUIRE(recipes.load(itemsDataDir() + "/recipes.json", registry, errors));

    const harrowmere::data::ItemData *iron  = registry.itemByKey("iron");
    const harrowmere::data::ItemData *wood  = registry.itemByKey("wood");
    const harrowmere::data::ItemData *tools = registry.itemByKey("tools");
    REQUIRE(iron != nullptr);
    REQUIRE(wood != nullptr);
    REQUIRE(tools != nullptr);

    // CRAFTING 7 < the sword recipe's 8: refused BEFORE any side effect —
    // the inputs survive untouched (a blocked craft never consumes).
    c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::CRAFTING)] = 7;
    harrowmere::items::addStacked(bag, iron->id, 3);
    harrowmere::items::addStacked(bag, wood->id, 1);
    harrowmere::items::addStacked(bag, tools->id, 1);

    auto blocked = harrowmere::craft(c, bag, instances, registry, recipes, "craft_iron_sword");
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == harrowmere::CraftError::SkillRequirementUnmet);
    CHECK(harrowmere::items::countStacked(bag, iron->id) == 3);
    CHECK(harrowmere::items::countStacked(bag, wood->id) == 1);
    CHECK(harrowmere::items::countStacked(bag, tools->id) == 1);
    CHECK(bag.instances.empty());

    // Missing inputs refuse the same way (skill met, bag short).
    c.progression.skills[static_cast<std::size_t>(harrowmere::SkillType::CRAFTING)] = 8;
    blocked = harrowmere::craft(c, bag, instances, registry, recipes, "craft_iron_sword");
    // the bag holds exactly 3 iron + 1 wood + 1 tools — this craft SUCCEEDS;
    // take the inputs away and the refusal comes back.
    REQUIRE(blocked.isOk());
    CHECK(harrowmere::items::countStacked(bag, iron->id) == 0);
    blocked = harrowmere::craft(c, bag, instances, registry, recipes, "craft_iron_sword");
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == harrowmere::CraftError::MissingInputs);

    // Unknown recipe keys refuse too (the UI sent a stale row).
    blocked = harrowmere::craft(c, bag, instances, registry, recipes, "craft_nothing");
    REQUIRE(blocked.isErr());
    CHECK(blocked.error() == harrowmere::CraftError::UnknownRecipe);
}

// ---- pinned: TestGuardGear_SellEmitsTheftSignal ------------------------------------

TEST_CASE(
    "TestGuardGear_SellEmitsTheftSignal"
)
{
    LoadedRegistry loaded;
    const harrowmere::data::DataRegistry &registry = loaded.registry;
    const harrowmere::data::ItemData *halberd      = registry.itemByKey("guard_halberd");
    const harrowmere::data::ItemData *sword        = registry.itemByKey("iron_sword");
    REQUIRE(halberd != nullptr);
    REQUIRE(sword != nullptr);
    CHECK(harrowmere::isGuardMarked(*halberd));
    CHECK_FALSE(harrowmere::isGuardMarked(*sword));

    // A small territory tree: kingdom 1 -> village 2, one resident commoner.
    harrowmere::world::WorldStore world(registry.defaultLaws());
    harrowmere::world::Territory kingdom{};
    kingdom.type = harrowmere::TerritoryType::KINGDOM;
    kingdom.id   = 1;
    world.addTerritory(kingdom);
    harrowmere::world::Territory village{};
    village.type   = harrowmere::TerritoryType::VILLAGE;
    village.id     = 2;
    village.parent = 1;
    world.addTerritory(village);

    harrowmere::Character seller     = makeCharacter(100);
    seller.body.position             = {12.0, 0.0, 34.0};
    const harrowmere::Position where = seller.body.position;

    RecordingJusticeSink sink;

    // A commoner selling guard gear: THE hook fires — culprit + position.
    harrowmere::onGuardGearSold(seller, *halberd, &world, 2, &sink);
    CHECK(sink.theftFlags == 1);
    CHECK(sink.theftCulprit == seller.id);
    CHECK(sink.theftWhere.x == where.x);
    CHECK(sink.theftWhere.z == where.z);

    // Regular gear never flags anything.
    harrowmere::onGuardGearSold(seller, *sword, &world, 2, &sink);
    CHECK(sink.theftFlags == 1);

    // A ROYAL title at the selling territory is legitimate ownership: silent.
    world.territory(2)->titles[seller.id] = harrowmere::SocialClassId::ROYAL;
    harrowmere::onGuardGearSold(seller, *halberd, &world, 2, &sink);
    CHECK(sink.theftFlags == 1);
    // ...so is NOBLE...
    world.territory(2)->titles[seller.id] = harrowmere::SocialClassId::NOBLE;
    harrowmere::onGuardGearSold(seller, *halberd, &world, 2, &sink);
    CHECK(sink.theftFlags == 1);
    // ...and the title only counts AT the selling point in the tree: selling
    // in a sibling tree (an unknown territory walks to COMMONER) flags again.
    world.territory(2)->titles[seller.id] = harrowmere::SocialClassId::COMMONER;
    harrowmere::onGuardGearSold(seller, *halberd, &world, 2, &sink);
    CHECK(sink.theftFlags == 2);

    // The NULL sink compiles and no-ops — the D-SINK-1 default 2.4's
    // Done-when demands (the hook's signature never changes when 4.1 wires
    // the real sink in).
    harrowmere::NullJusticeSink nullSink;
    world.territory(2)->titles[seller.id] = harrowmere::SocialClassId::COMMONER;
    harrowmere::onGuardGearSold(seller, *halberd, &world, 2, &nullSink);
    CHECK(true); // reached: no crash, no state

    // The guard-ROLE half of the entitlement arrives as the caller's flag
    // until NpcState (3.4) carries the role read: a flagged-guard NPC sells
    // legitimately even without a title.
    CHECK(harrowmere::guardGearEntitled(seller, &world, 2, /*npcRoleEntitled=*/true));
}

// ---- supporting: the bag record's stack/instance bookkeeping ------------------------

TEST_CASE(
    "TestInventory_StackAndInstanceHelpers"
)
{
    harrowmere::items::Inventory bag;

    harrowmere::items::addStacked(bag, 7, 2);
    harrowmere::items::addStacked(bag, 7, 3);
    CHECK(harrowmere::items::countStacked(bag, 7) == 5);

    CHECK(harrowmere::items::takeStacked(bag, 7, 4));
    CHECK(harrowmere::items::countStacked(bag, 7) == 1);
    CHECK_FALSE(harrowmere::items::takeStacked(bag, 7, 2)); // short: nothing changes
    CHECK(harrowmere::items::countStacked(bag, 7) == 1);
    CHECK(harrowmere::items::takeStacked(bag, 7, 1));
    CHECK_FALSE(bag.stacks.contains(7)); // zeroed stacks erase, never store a 0

    CHECK(harrowmere::items::takeStacked(bag, 7, 0)); // taking nothing succeeds

    harrowmere::items::addLooseInstance(bag, 11);
    harrowmere::items::addLooseInstance(bag, 12);
    CHECK(harrowmere::items::holdsInstance(bag, 11));
    CHECK(harrowmere::items::holdsInstance(bag, 12));
    CHECK(harrowmere::items::takeLooseInstance(bag, 11));
    CHECK_FALSE(harrowmere::items::holdsInstance(bag, 11));
    CHECK_FALSE(harrowmere::items::takeLooseInstance(bag, 99));

    // The equipped mirror participates in holdsInstance (ownership check).
    bag.equipped.push_back(13);
    CHECK(harrowmere::items::holdsInstance(bag, 13));
}

// ---- supporting: the instance registry's save seams ----------------------------------

TEST_CASE(
    "TestInventory_InstanceRegistryCounterSeams"
)
{
    harrowmere::items::ItemInstanceRegistry registry;
    CHECK(harrowmere::items::ItemInstanceRegistry::FIRST_INSTANCE_ID == 1); // 0 reserved (R10)

    const std::uint64_t a = registry.spawn(5);
    const std::uint64_t b = registry.spawn(6);
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(registry.itemDefOf(a) == 5);
    CHECK(registry.itemDefOf(b) == 6);
    CHECK(registry.counter() == 3);

    // The 7.1 roundtrip seam: a loaded save adopts the counter (never
    // rewinds) and restores its instances.
    harrowmere::items::ItemInstanceRegistry loaded;
    loaded.adoptCounter(2); // below the live counter of a real save; clamps up on restore
    loaded.restore(1, 5);
    CHECK(loaded.counter() == 2);
    CHECK(loaded.itemDefOf(1) == 5);
    loaded.adoptCounter(10);
    CHECK(loaded.spawn(7) == 10); // ids never reuse, never collide with restored ones
}
