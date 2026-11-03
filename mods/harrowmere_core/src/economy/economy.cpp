// economy/economy.cpp — Task 3.7 implementation (D-ECON-1).
//
// Prices are PUBLISHED per game-hour at step 7 and trades transact at the published
// price — per-transaction live pricing is the pump exploit the 3.7 pitfalls call out
// (players repricing their own visit). Capacity-0 regions take the pinned R2 fallback
// (price = baseValue) BEFORE any division. Every delta is signed (R1); every product
// of SCALE-grid values goes through the 128-bit helper (R3).
#include "economy/economy.hpp"

#include <algorithm>

#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "core/math.hpp"
#include "core/world_context.hpp"

namespace harrowmere
{

// ---------------------------------------------------------------- goods table --

void EconomyStore::registerGood(
    goodId id, const char *key, std::uint64_t baseValue, std::uint64_t perCapitaNeedScale
)
{
    GoodRow row;
    row.baseValue          = baseValue;
    row.perCapitaNeedScale = perCapitaNeedScale;
    goods_[id]             = row;
    goodKeys_[key]         = id;
}

goodId EconomyStore::goodIdForKey(
    const char *key
) const
{
    const auto it = goodKeys_.find(key);
    return it == goodKeys_.end() ? static_cast<goodId>(0) : it->second;
}

std::uint64_t EconomyStore::baseValueOf(
    goodId good
) const
{
    const auto it = goods_.find(good);
    return it == goods_.end() ? 0u : it->second.baseValue;
}

std::uint64_t EconomyStore::perCapitaNeedScaleOf(
    goodId good
) const
{
    const auto it = goods_.find(good);
    return it == goods_.end() ? 0u : it->second.perCapitaNeedScale;
}

// ------------------------------------------------------------ stocks & demand --

RegionStock &EconomyStore::stock(
    regionId region, goodId good
)
{
    return stocks_[region][good]; // creates on access (demand 0 until the daily cycle)
}

const RegionStock *EconomyStore::findStock(
    regionId region, goodId good
) const
{
    const auto rit = stocks_.find(region);
    if (rit == stocks_.end())
        return nullptr;
    const auto git = rit->second.find(good);
    return git == rit->second.end() ? nullptr : &git->second;
}

void EconomyStore::addSupply(
    regionId region, goodId good, std::uint64_t units
)
{
    stocks_[region][good].supply += static_cast<std::int64_t>(units);
}

void EconomyStore::setDemand(
    regionId region, goodId good, std::uint64_t unitsPerDay
)
{
    stocks_[region][good].demand = static_cast<std::int64_t>(unitsPerDay);
}

void EconomyStore::setPopulation(
    regionId region, std::uint32_t population
)
{
    population_[region] = population;
}

std::uint32_t EconomyStore::populationOf(
    regionId region
) const
{
    const auto it = population_.find(region);
    return it == population_.end() ? 0u : it->second;
}

void EconomyStore::applyDailyConsumption(
    regionId region, WorldContext & /*ctx*/
)
{
    const auto rit = stocks_.find(region);
    if (rit == stocks_.end())
        return;
    for (auto &[good, s] : rit->second)
    {
        (void)good;
        if (s.demand <= 0)
            continue;
        s.supply -= s.demand; // the population eats its need (R1: signed stock)
        if (s.supply < 0)
            s.supply = 0; // scarcity is the price signal, never negative stock
    }
}

void EconomyStore::applyDailyCycle(
    WorldContext &ctx
)
{
    // 1. Population snapshot per stocked region (A-49's capacity input).
    for (const auto &[region, goodsMap] : stocks_)
    {
        (void)goodsMap;
        population_[region] = ctx.world->regionPopulation(region);
    }
    // 2. Daily demand = population x perCapitaNeed (consumption per NPC per game-day).
    for (auto &[region, goodsMap] : stocks_)
    {
        const std::uint32_t pop = populationOf(region);
        for (auto &[good, s] : goodsMap)
        {
            (void)s;
            setDemand(region, good, mulDivU64(pop, perCapitaNeedScaleOf(good), SCALE));
        }
    }
    // 3. Consumption applies against the fresh demand.
    for (const auto &[region, goodsMap] : stocks_)
    {
        (void)goodsMap;
        applyDailyConsumption(region, ctx);
    }
}

// ------------------------------------------------------- D-ECON-1 price math --

std::uint64_t EconomyStore::priceFor(
    regionId region, goodId good
) const
{
    const std::uint64_t base = baseValueOf(good);
    if (base == 0)
        return 0; // no priced good (R2: nothing to fall back to)

    // capacity = population * perCapitaNeed (A-49); perCapitaNeed is on the SCALE grid.
    const std::uint64_t capacity = mulDivU64(populationOf(region), perCapitaNeedScaleOf(good), SCALE);
    if (capacity == 0)
        return base; // pinned R2 fallback BEFORE dividing: empty region -> baseValue

    const RegionStock *s      = findStock(region, good);
    const std::int64_t supply = (s != nullptr) ? s->supply : 0;
    const std::int64_t demand = (s != nullptr) ? s->demand : 0;

    std::int64_t ratio      = demand - supply; // signed delta (R1)
    const std::int64_t band = 4 * static_cast<std::int64_t>(capacity);
    if (ratio > band)
        ratio = band;
    if (ratio < -band)
        ratio = -band;

    // price = base * (SCALE + ratio * PRICE_SENSITIVITY / capacity) / SCALE
    const std::int64_t pressure
        = ratio * static_cast<std::int64_t>(PRICE_SENSITIVITY) / static_cast<std::int64_t>(capacity);
    std::int64_t price = static_cast<std::int64_t>(base) * (static_cast<std::int64_t>(SCALE) + pressure)
                       / static_cast<std::int64_t>(SCALE);

    const std::int64_t lo = static_cast<std::int64_t>(base / 4); // the 4x band (D-ECON-1)
    const std::int64_t hi = static_cast<std::int64_t>(base * 4);
    if (price < lo)
        price = lo;
    if (price > hi)
        price = hi;
    return static_cast<std::uint64_t>(price);
}

std::uint64_t EconomyStore::publishedPrice(
    regionId region, goodId good
) const
{
    const auto rit = prices_.find(region);
    if (rit == prices_.end())
        return 0;
    const auto git = rit->second.find(good);
    return git == rit->second.end() ? 0u : git->second;
}

std::uint64_t EconomyStore::transactionPrice(
    regionId region, goodId good
) const
{
    const std::uint64_t published = publishedPrice(region, good);
    return published != 0 ? published : priceFor(region, good); // fallback before the first publish
}

void EconomyStore::recomputePrices(
    WorldContext &ctx
)
{
    for (const auto &[region, goodsMap] : stocks_)
    {
        for (const auto &[good, s] : goodsMap)
        {
            (void)s;
            std::uint64_t fresh = priceFor(region, good);
            // 5.5's wiring of 5.1's PriceBias record (the D-ECON-1 band
            // stays the world invariant — the biased price clamps into the
            // same [base/4, base*4] as every other price).
            if (ctx.eventEffects != nullptr)
            {
                const std::uint64_t bias = ctx.eventEffects->priceBiasFor(region, good, ctx.now);
                if (bias != SCALE)
                {
                    const std::uint64_t base   = baseValueOf(good);
                    const std::uint64_t biased = base > 0 ? fresh * bias / SCALE : fresh;
                    const std::uint64_t lo     = base / 4;
                    const std::uint64_t hi     = base * 4;
                    fresh                      = std::min<std::uint64_t>(std::max<std::uint64_t>(biased, lo), hi);
                }
            }
            const std::uint64_t old = publishedPrice(region, good);
            prices_[region][good]   = fresh;
            lastRecompute_[region]  = ctx.now;

            // Publish PriceChanged on a >= 10% move (task 3.7; SCALE grid).
            bool moved = false;
            if (old == 0)
            {
                moved = fresh != 0; // first publication is a change
            } else
            {
                const std::uint64_t diff = fresh > old ? fresh - old : old - fresh;
                moved                    = diff * SCALE >= old * PRICE_CHANGED_MIN_MOVE;
            }
            if (moved)
                ctx.events->onPriceChanged(region, good, old, fresh);
        }
    }
}

// ------------------------------------------------------------------- trades --

TradeResult EconomyStore::buy(
    Character &buyer, regionId region, goodId good, std::uint32_t qty, WorldContext &ctx
)
{
    if (qty == 0)
        return TradeResult::err(EconomyError::InvalidTarget);
    RegionStock &s = stocks_[region][good];
    if (s.supply < static_cast<std::int64_t>(qty))
        return TradeResult::err(EconomyError::OutOfStock);

    const std::uint64_t unit = transactionPrice(region, good);
    const std::uint64_t cost = unit * static_cast<std::uint64_t>(qty);
    // NO overdraft outside conviction fines (D-ECON-1): the buy path refuses short wallets.
    if (buyer.progression.currency < static_cast<std::int64_t>(cost))
        return TradeResult::err(EconomyError::InsufficientFunds);

    buyer.progression.currency -= static_cast<std::int64_t>(cost);
    s.supply -= static_cast<std::int64_t>(qty);
    addGoodsHeld(buyer.id, good, static_cast<std::int64_t>(qty));

    const territoryId terr = ctx.world->territoryOfRegion(region);
    accrueTax(terr, cost * TAX_SLICE_PER_TRANSACTION / SCALE); // noble duty collects (3.6)
    return TradeResult::ok();
}

TradeResult EconomyStore::sell(
    Character &seller, regionId region, goodId good, std::uint32_t qty, WorldContext &ctx
)
{
    if (qty == 0)
        return TradeResult::err(EconomyError::InvalidTarget);
    if (goodsHeld(seller.id, good) < qty)
        return TradeResult::err(EconomyError::InvalidTarget);

    const std::uint64_t unit = transactionPrice(region, good);
    const territoryId terr   = ctx.world->territoryOfRegion(region);
    // Suspicion tier of the seller applies (4.5 hook: SHUNNED -20% receipts); null = SCALE.
    const std::uint64_t modifier = ctx.priceModifier->sellReceiptModifier(seller.id, terr);
    const std::uint64_t gross    = mulDivU64(unit * static_cast<std::uint64_t>(qty), modifier, SCALE);

    addGoodsHeld(seller.id, good, -static_cast<std::int64_t>(qty));
    stocks_[region][good].supply += static_cast<std::int64_t>(qty);
    seller.progression.currency += static_cast<std::int64_t>(gross);
    accrueTax(terr, gross * TAX_SLICE_PER_TRANSACTION / SCALE);
    return TradeResult::ok();
}

// ------------------------------------------------- territory wealth & taxes --

void EconomyStore::accrueTax(
    territoryId territory, std::uint64_t amount
)
{
    if (amount == 0)
        return;
    pendingTaxes_[territory] += amount;
}

void EconomyStore::collectTaxes(
    territoryId territory
)
{
    const auto it = pendingTaxes_.find(territory);
    if (it == pendingTaxes_.end())
        return;
    territoryWealth_[territory] += it->second;
    pendingTaxes_.erase(it);
}

std::uint64_t EconomyStore::takeWealth(
    territoryId territory, std::uint64_t maxAmount
)
{
    std::uint64_t &w          = territoryWealth_[territory];
    const std::uint64_t taken = w < maxAmount ? w : maxAmount;
    w -= taken;
    return taken;
}

std::uint64_t EconomyStore::wealth(
    territoryId territory
) const
{
    const auto it = territoryWealth_.find(territory);
    return it == territoryWealth_.end() ? 0u : it->second;
}

std::uint64_t EconomyStore::pendingTaxes(
    territoryId territory
) const
{
    const auto it = pendingTaxes_.find(territory);
    return it == pendingTaxes_.end() ? 0u : it->second;
}

// -------------------------------------------------- goods held by characters --

std::uint64_t EconomyStore::goodsHeld(
    characterId who, goodId good
) const
{
    const auto pit = goodsHeld_.find(who);
    if (pit == goodsHeld_.end())
        return 0;
    const auto git = pit->second.find(good);
    return git == pit->second.end() ? 0u : git->second;
}

void EconomyStore::addGoodsHeld(
    characterId who, goodId good, std::int64_t delta
)
{
    std::uint64_t &held = goodsHeld_[who][good];
    if (delta >= 0)
    {
        held += static_cast<std::uint64_t>(delta);
    } else
    {
        const std::uint64_t take = static_cast<std::uint64_t>(-delta);
        held                     = held > take ? held - take : 0u; // saturates; callers check first
    }
}

std::vector<goodId> EconomyStore::producedGoods(
    regionId region
) const
{
    // The extraction ledger read (6.2's ResourceConflict cb): the region's
    // stock rows carrying supply. R7: the map's own good-id ascending order.
    std::vector<goodId> out;
    const auto rit = stocks_.find(region);
    if (rit == stocks_.end())
        return out;
    for (const auto &[good, stock] : rit->second)
    {
        if (stock.supply > 0)
        {
            out.push_back(good);
        }
    }
    return out;
}

// ---------------------------------------------------------------- step 7 system --

void EconomySystem::run(
    WorldContext &ctx
)
{
    const std::uint64_t hour = ctx.now / 3600;
    const std::uint64_t day  = ctx.now / core::DAY_LENGTH_SECONDS;

    if (!started_)
    {
        started_  = true;
        lastHour_ = hour;
        lastDay_  = day;
        ctx.economy->recomputePrices(ctx); // seed published prices; no consumption at boot
        return;
    }

    if (day != lastDay_)
    {
        lastDay_ = day;
        ctx.economy->applyDailyCycle(ctx); // population snapshot -> demand -> consumption
    }
    if (hour != lastHour_)
    {
        lastHour_ = hour;
        ctx.economy->recomputePrices(ctx); // hourly publication (3.7 pitfall: never per transaction)
    }
}

// ---- save seam (task 7.1) -------------------------------------------------------

EconomyStore::Snapshot EconomyStore::snapshot() const
{
    Snapshot out;
    out.stocks          = stocks_;
    out.prices          = prices_;
    out.pendingTaxes    = pendingTaxes_;
    out.territoryWealth = territoryWealth_;
    out.goodsHeld       = goodsHeld_;
    out.lastRecompute   = lastRecompute_;
    out.population      = population_;
    return out;
}

void EconomyStore::restore(
    const Snapshot &saved
)
{
    stocks_          = saved.stocks;
    prices_          = saved.prices;
    pendingTaxes_    = saved.pendingTaxes;
    territoryWealth_ = saved.territoryWealth;
    goodsHeld_       = saved.goodsHeld;
    lastRecompute_   = saved.lastRecompute;
    population_      = saved.population;
}

} // namespace harrowmere
