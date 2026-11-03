// economy/economy.hpp — Task 3.7: dynamic regional prices (D-ECON-1), NPC vendors
// trading by price signals, player buying/selling, and the territory-wealth
// aggregation other systems read (guard power, war resources, taxes).
//
// Prices recompute PER GAME-HOUR at step 7 — never per transaction (3.7 pitfalls:
// per-transaction recompute lets players pump prices within one visit).
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "core/result.hpp" // Result<void, ...> (task 0.2)
#include "core/scheduler.hpp"
#include "core/types.hpp"

namespace harrowmere
{

struct WorldContext;
struct Character;

// Rule-level refusals of the trade paths (3.7, §3.4). The 0.2 Result<void, E>
// template is the tree-wide carrier.
enum class EconomyError
{
    InvalidTarget,     // qty 0 / goods not held (a rule refusal, no side effect)
    OutOfStock,        // the region cannot supply the requested units
    InsufficientFunds, // D-ECON-1: the buy path refuses short wallets (no overdraft)
    COUNT
};
using TradeResult = Result<void, EconomyError>;

struct RegionStock
{
    std::int64_t supply = 0; // units on hand (production, trades)
    std::int64_t demand = 0; // per-game-day demand units (population needs)
};

class EconomyStore
{
public:
    // ---- goods table (loaded from goods.json by the plugin's 0.4 adapter) ----
    void registerGood(goodId id, const char *key, std::uint64_t baseValue, std::uint64_t perCapitaNeedScale);
    goodId goodIdForKey(const char *key) const;
    std::uint64_t baseValueOf(goodId good) const;
    std::uint64_t perCapitaNeedScaleOf(goodId good) const;

    // ---- stocks & demand ----
    RegionStock &stock(regionId region, goodId good); // creates on access
    const RegionStock *findStock(regionId region, goodId good) const;
    void addSupply(regionId region, goodId good, std::uint64_t units);       // farmer duties, migrant
    void setDemand(regionId region, goodId good, std::uint64_t unitsPerDay); // daily consumption recompute
    void applyDailyConsumption(regionId region, WorldContext &ctx);          // step 7's day boundary

    // ---- population snapshot (A-49's capacity input; refreshed by step 7's daily
    //      cycle; tests may pin it directly) ----
    void setPopulation(regionId region, std::uint32_t population);
    std::uint32_t populationOf(regionId region) const;

    // Step 7's day boundary as ONE deterministic unit: refresh the population
    // snapshot, recompute daily demand from the A-49 per-capita table, apply
    // consumption. (The hourly price recompute is recomputePrices, below.)
    void applyDailyCycle(WorldContext &ctx);

    // ---- D-ECON-1: the pinned price formula (all integer SCALE math) ----
    //   ratio = (demand - supply) clamped to [-4*capacity, +4*capacity]
    //   capacity = population * perCapitaNeed (A-49)
    //   price = baseValue * (SCALE + ratio * PRICE_SENSITIVITY / capacity) / SCALE
    //   clamped [base/4, base*4]; capacity 0 -> baseValue (R2 fallback, before dividing)
    std::uint64_t priceFor(regionId region, goodId good) const;

    // Last PUBLISHED (hourly) price; 0 = never published. Trades transact at the
    // published price — per-transaction live pricing is the pump exploit the 3.7
    // pitfalls call out (players repricing their own visit).
    std::uint64_t publishedPrice(regionId region, goodId good) const;
    std::uint64_t transactionPrice(regionId region, goodId good) const;

    // Step 7 (hourly): recompute published prices; publish PriceChanged on >=10% moves.
    void recomputePrices(WorldContext &ctx);

    // ---- trades (the only currency mutation paths in v1 — 3.7's Done-when) ----
    TradeResult buy(Character &buyer, regionId region, goodId good, std::uint32_t qty, WorldContext &ctx);
    TradeResult sell(Character &seller, regionId region, goodId good, std::uint32_t qty, WorldContext &ctx);

    // ---- territory wealth & taxes (FactionStore resources absorb these in 6.1) ----
    void accrueTax(territoryId territory, std::uint64_t amount);
    void collectTaxes(territoryId territory);                                 // the noble duty
    std::uint64_t takeWealth(territoryId territory, std::uint64_t maxAmount); // the wage path
    std::uint64_t wealth(territoryId territory) const;
    std::uint64_t pendingTaxes(territoryId territory) const;

    // ---- goods held by characters (economy-owned: goods are not items, D-STORE-1) ----
    std::uint64_t goodsHeld(characterId who, goodId good) const;
    void addGoodsHeld(characterId who, goodId good, std::int64_t delta);

    // The goods the region currently holds supply of (production = extraction;
    // 6.2's ResourceConflict casus belli reads the ledger). R7: good-id
    // ascending.
    std::vector<goodId> producedGoods(regionId region) const;

    std::uint64_t lastPriceRecompute(
        regionId region
    ) const
    { // test/hook read
        auto it = lastRecompute_.find(region);
        return it == lastRecompute_.end() ? 0u : it->second;
    }

    // ---- save seam (task 7.1) --------------------------------------------------
    //
    // The store's six maps as one plain-data snapshot; persist/ serializes it
    // and the load path hands it back through restore(). The goods table is
    // NOT in the snapshot: it is boot content (goods.json through the plugin's
    // 0.4 adapter), reloaded before a save applies.
    struct Snapshot
    {
        std::map<regionId, std::map<goodId, RegionStock>> stocks;
        std::map<regionId, std::map<goodId, std::uint64_t>> prices;
        std::map<territoryId, std::uint64_t> pendingTaxes;
        std::map<territoryId, std::uint64_t> territoryWealth;
        std::map<characterId, std::map<goodId, std::uint64_t>> goodsHeld;
        std::map<regionId, std::uint64_t> lastRecompute;
        std::map<regionId, std::uint32_t> population;
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot &saved); // replaces the six maps wholesale

private:
    struct GoodRow
    {
        std::uint64_t baseValue          = 0;
        std::uint64_t perCapitaNeedScale = 0;
    };
    std::map<goodId, GoodRow> goods_;
    std::map<std::string, goodId> goodKeys_;

    std::map<regionId, std::map<goodId, RegionStock>> stocks_; // R7 everywhere
    std::map<regionId, std::map<goodId, std::uint64_t>> prices_;

    std::map<territoryId, std::uint64_t> pendingTaxes_;
    std::map<territoryId, std::uint64_t> territoryWealth_;
    std::map<characterId, std::map<goodId, std::uint64_t>> goodsHeld_;
    std::map<regionId, std::uint64_t> lastRecompute_;
    std::map<regionId, std::uint32_t> population_; // A-49 snapshot, refreshed daily (step 7)
};

// Step 7 (Economy): hourly price recompute + the daily production/consumption apply.
class EconomySystem final : public IStepSystem
{
public:
    void run(WorldContext &ctx) override;

    // ---- save seam (task 7.1): the step's lazy hour/day gates ride the save's
    // "runtime" block so a loaded world resumes the cadence exactly (no extra
    // recompute on the load tick).
    struct RuntimeGates
    {
        bool started           = false;
        std::uint64_t lastHour = 0;
        std::uint64_t lastDay  = 0;
    };
    [[nodiscard]] RuntimeGates runtimeGates() const
    {
        return RuntimeGates{started_, lastHour_, lastDay_};
    }
    void restoreRuntimeGates(
        const RuntimeGates &gates
    )
    {
        started_  = gates.started;
        lastHour_ = gates.lastHour;
        lastDay_  = gates.lastDay;
    }

private:
    bool started_           = false;
    std::uint64_t lastHour_ = 0;
    std::uint64_t lastDay_  = 0;
};

} // namespace harrowmere
