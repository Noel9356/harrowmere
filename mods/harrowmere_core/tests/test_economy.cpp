// tests/test_economy.cpp — Task 3.7 pinned tests.
// TestEconomy_ScarcityRaisesPriceBounded, TestEconomy_GlutLowersPriceBounded,
// TestEconomy_TradesAdjustStocks, TestEconomy_PlayerCannotOverdraftWallet,
// TestEconomy_PricePublishesOnChange, TestEconomy_SuspicionHookAppliesWhenPresent.
//
// D-ECON-1: integer SCALE math throughout; capacity-0 regions take the baseValue
// fallback BEFORE dividing (R2); prices publish per game-hour, never per transaction.
#include <doctest.h> // the single implement define lives in main.cpp (task 0.6)

#include "core/constants.hpp"
#include "core/constants_phase2.hpp"
#include "economy/economy.hpp"
#include "test_phase2_fixture.hpp"

namespace
{
// phase2_test already carries `using namespace harrowmere;` (fixture header):
// a second using-directive here makes the qualified name `harrowmere::`
// ambiguous for the TU (transitive lookup reaches the namespace by two paths).
using namespace phase2_test;

// A test good with clean numbers: baseValue 1000, perCapitaNeed 1.0/day on the SCALE
// grid (10000). Population 100 -> capacity exactly 100 units.
constexpr goodId kTestGood = 100;

struct CleanEconomy
{
    Fixture f;
    CleanEconomy()
    {
        f.addTerritory(1, 0.0, 0.0, 1, false);
        f.economy.registerGood(kTestGood, "test_good", 1000, 10000);
        f.economy.setPopulation(1, 100);
    }
};
} // namespace

TEST_CASE(
    "TestEconomy_ScarcityRaisesPriceBounded"
)
{
    CleanEconomy e;
    EconomyStore &eco = e.f.economy;
    eco.setDemand(1, kTestGood, 100); // demand == capacity, supply 0

    // Full pressure at capacity: 1000 * (10000 + 100*4000/100) / 10000 = 1400.
    CHECK(eco.priceFor(1, kTestGood) == 1400);
    CHECK(eco.priceFor(1, kTestGood) <= 1000 * 4); // inside the 4x band

    // Extreme deficit: the ratio clamps at +4*capacity, price at 2.6x — still bounded.
    eco.setDemand(1, kTestGood, 1000);
    CHECK(eco.priceFor(1, kTestGood) == 2600);
    CHECK(eco.priceFor(1, kTestGood) <= 1000 * 4);

    // Capacity 0 (empty region): the pinned R2 fallback fires BEFORE dividing.
    e.f.economy.setPopulation(1, 0);
    CHECK(eco.priceFor(1, kTestGood) == 1000);
}

TEST_CASE(
    "TestEconomy_GlutLowersPriceBounded"
)
{
    CleanEconomy e;
    EconomyStore &eco = e.f.economy;
    eco.setDemand(1, kTestGood, 100);

    eco.addSupply(1, kTestGood, 300);              // 3x capacity: 1000*(10000 - 200*4000/100)/10000
    CHECK(eco.priceFor(1, kTestGood) == 250);      // formula 200 -> clamped at base/4
    CHECK(eco.priceFor(1, kTestGood) >= 1000 / 4); // never below the band floor

    eco.addSupply(1, kTestGood, 700); // ratio -900 clamps to -4*capacity
    CHECK(eco.priceFor(1, kTestGood) == 250);

    // Glut without demand at all: same floor.
    eco.setDemand(1, kTestGood, 0);
    CHECK(eco.priceFor(1, kTestGood) == 250);
}

TEST_CASE(
    "TestEconomy_TradesAdjustStocks"
)
{
    CleanEconomy e;
    EconomyStore &eco = e.f.economy;
    RegionStock &s    = eco.stock(1, kTestGood);
    s.supply          = 50;
    s.demand          = 100;
    eco.recomputePrices(e.f.ctx); // publish: ratio 50 -> 1000*(10000 + 50*40)/10000 = 1200

    const characterId p        = spawnPlayer(e.f);
    Character &buyer           = e.f.characters.get(p);
    buyer.progression.currency = 100000;
    CHECK(eco.publishedPrice(1, kTestGood) == 1200);

    CHECK(eco.buy(buyer, 1, kTestGood, 2, e.f.ctx).isOk());
    CHECK(buyer.progression.currency == 100000 - 2 * 1200);
    CHECK(s.supply == 48); // buy drains the regional stock
    CHECK(eco.goodsHeld(p, kTestGood) == 2);

    CHECK(eco.sell(buyer, 1, kTestGood, 1, e.f.ctx).isOk());
    CHECK(s.supply == 49); // sell refills it
    CHECK(eco.goodsHeld(p, kTestGood) == 1);
    CHECK(buyer.progression.currency == 100000 - 2400 + 1200);

    // The tax slice parked both sides' transaction value for the noble duty (3.6):
    // 5% of (2400 + 1200) — proposed constant, flagged in MERGE_NOTES.md.
    const territoryId terr = e.f.world.territoryOfRegion(1);
    CHECK(eco.pendingTaxes(terr) == (2400 + 1200) * TAX_SLICE_PER_TRANSACTION / SCALE);

    // Selling goods you do not hold is refused.
    CHECK(eco.sell(buyer, 1, kTestGood, 5, e.f.ctx).error() == EconomyError::InvalidTarget);
    // Buying more than the region stocks is refused.
    CHECK(eco.buy(buyer, 1, kTestGood, 1000, e.f.ctx).error() == EconomyError::OutOfStock);
}

TEST_CASE(
    "TestEconomy_PlayerCannotOverdraftWallet"
)
{
    CleanEconomy e;
    EconomyStore &eco              = e.f.economy;
    eco.stock(1, kTestGood).supply = 50;
    eco.setDemand(1, kTestGood, 100);
    eco.recomputePrices(e.f.ctx); // 1400/unit

    const characterId p        = spawnPlayer(e.f);
    Character &buyer           = e.f.characters.get(p);
    buyer.progression.currency = 100;

    // NO overdraft outside conviction fines (D-ECON-1): the buy path refuses.
    CHECK(eco.buy(buyer, 1, kTestGood, 1, e.f.ctx).error() == EconomyError::InsufficientFunds);
    CHECK(buyer.progression.currency == 100);    // wallet untouched
    CHECK(eco.stock(1, kTestGood).supply == 50); // stock untouched
    CHECK(eco.goodsHeld(p, kTestGood) == 0);
}

TEST_CASE(
    "TestEconomy_PricePublishesOnChange"
)
{
    CleanEconomy e;
    EconomyStore &eco = e.f.economy;
    RegionStock &s    = eco.stock(1, kTestGood);
    s.supply          = 0;
    s.demand          = 100;

    // First publication is a change (UIs need a starting price).
    eco.recomputePrices(e.f.ctx);
    REQUIRE(e.f.events.priceMoves.size() == 1);
    CHECK(e.f.events.priceMoves[0].oldPrice == 0);
    CHECK(e.f.events.priceMoves[0].newPrice == 1400);

    // A < 10% move publishes nothing (demand 110 -> 1440: +2.9%).
    eco.setDemand(1, kTestGood, 110);
    eco.recomputePrices(e.f.ctx);
    CHECK(e.f.events.priceMoves.size() == 1);

    // A >= 10% move publishes with old and new (demand 300 -> 2200: +53%).
    eco.setDemand(1, kTestGood, 300);
    eco.recomputePrices(e.f.ctx);
    REQUIRE(e.f.events.priceMoves.size() == 2);
    CHECK(e.f.events.priceMoves[1].oldPrice == 1440);
    CHECK(e.f.events.priceMoves[1].newPrice == 2200);

    // The hourly cadence lives in the step-7 system: no re-transaction pricing.
    EconomySystem system;
    e.f.ctx.now = 3600; // one game-hour after the seed run
    s.demand    = 100;
    system.run(e.f.ctx); // first run seeds published prices
    CHECK(e.f.economy.lastPriceRecompute(1) == 3600);
    e.f.ctx.now += 3599; // same game-hour: no recompute
    system.run(e.f.ctx);
    CHECK(e.f.economy.lastPriceRecompute(1) == 3600);
}

TEST_CASE(
    "TestEconomy_SuspicionHookAppliesWhenPresent"
)
{
    CleanEconomy e;
    EconomyStore &eco              = e.f.economy;
    eco.stock(1, kTestGood).supply = 50;
    eco.setDemand(1, kTestGood, 100);
    eco.recomputePrices(e.f.ctx); // 1400/unit

    const characterId p         = spawnPlayer(e.f);
    Character &seller           = e.f.characters.get(p);
    seller.progression.currency = 0; // zero the A-41 starter purse; receipts only
    eco.addGoodsHeld(p, kTestGood, 10);

    // Null sink (SCALE): full receipts at the published 1200.
    CHECK(eco.sell(seller, 1, kTestGood, 2, e.f.ctx).isOk());
    CHECK(seller.progression.currency == 2 * 1200);

    // A shunned seller (4.5's stub: -20% receipts) nets 80%.
    e.f.priceModifier.modifier = 8000;
    CHECK(eco.sell(seller, 1, kTestGood, 2, e.f.ctx).isOk());
    CHECK(seller.progression.currency == 2 * 1200 + 2 * 1200 * 8000 / SCALE);
    REQUIRE(e.f.priceModifier.calls.size() == 2);
    CHECK(std::get<0>(e.f.priceModifier.calls[1]) == p);
    CHECK(std::get<1>(e.f.priceModifier.calls[1]) == e.f.world.territoryOfRegion(1));
}
