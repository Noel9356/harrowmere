// rewards.cpp — task 5.4's implementation (see rewards.hpp for the pinned
// single-pass, the pool denomination and the payout timing). The doc's
// distributeRewards body is kept line-for-line; the R7 tie rule is spelled
// as the sort's comparator so the remainder attribution cannot flip
// between runs (the card pitfall).

#include "events/rewards.hpp"

#include <algorithm>
#include <vector>

#include "character/character.hpp"
#include "core/math.hpp" // applyXpDelta (R4's one clamp)
#include "core/world_context.hpp"
#include "events/contribution.hpp" // getContribution + getEffectiveContribution (the single pass)
#include "items/inventory.hpp"     // addStacked (the item leg's bag path)
#include "progression/power.hpp"   // markPowerDirty (2.5's audited call sites)

namespace harrowmere::events
{

std::uint64_t rewardPool(
    const Event &ev, const EventTemplate &tpl
)
{
    std::uint64_t totalRaw = 0;
    for (const characterId p : ev.participants) // sorted (R7)
    {
        if (ev.excludedFromRewards.count(p) != 0)
        {
            continue; // the smuggler's presence never inflates the pool either
        }
        totalRaw += getContribution(ev, p, tpl);
    }
    return totalRaw * poolMultiplier(ev.currentState.difficulty) / SCALE; // A-107
}

void giveReward(
    characterId who, std::uint64_t pay, const EventTemplate &tpl, WorldContext &ctx
)
{
    if (pay == 0 || ctx.characters == nullptr || !ctx.characters->exists(who))
    {
        return;
    }
    Character &c = ctx.characters->get(who);

    // XP leg: totalXp never decreases (R4); availableXp through the one
    // clamp; the grant fires markPowerDirty (the 2.5 call-site audit's
    // XP-granting family — rewards are exactly that family).
    const std::uint64_t xp = pay * tpl.rewards.xpPerContribution;
    if (xp > 0)
    {
        c.progression.totalXp += xp;
        c.progression.availableXp = applyXpDelta(c.progression.availableXp, static_cast<std::int64_t>(xp));
        markPowerDirty(c);
    }

    // Currency leg: the i64 progression field credits directly (the wage
    // path's shape — 3.7 owns the trade paths; a reward is not a trade).
    const std::uint64_t currency = pay * tpl.rewards.currencyPerContribution;
    if (currency > 0)
    {
        c.progression.currency += static_cast<std::int64_t>(currency);
    }

    // Item leg: one unit of each listed item through the bag path (the
    // rewardTable's items are items.json keys — the definition row resolves
    // through the registry). Unknown keys and absent bags skip
    // deterministically; v1 grants at the definition rarity (the A-107
    // rarity bonus lands with per-instance rarity, 6.4).
    if (tpl.rewards.items.empty() || ctx.bags == nullptr || ctx.data == nullptr)
    {
        return;
    }
    items::Inventory *bag = ctx.bags->bagOf(who);
    if (bag == nullptr)
    {
        return;
    }
    for (const std::string &key : tpl.rewards.items)
    {
        const data::ItemData *row = ctx.data->itemByKey(key);
        if (row != nullptr)
        {
            items::addStacked(*bag, row->id, 1);
        }
    }
}

void distributeRewards(
    Event &ev, std::uint64_t pool, const EventTemplate &tpl, WorldContext &ctx
)
{
    // design reference verbatim: total computed ONCE; ineligible excluded
    // from total; flooring remainder to the top contributor.
    struct Share
    {
        characterId who;
        std::uint64_t amount;
    };
    std::vector<Share> shares;
    std::uint64_t total = 0;
    for (const characterId p : ev.participants) // sorted (R7)
    {
        if (ev.excludedFromRewards.count(p) != 0)
        {
            continue; // 4.7's smuggling hook
        }
        const std::uint64_t mine = getEffectiveContribution(ev, p, tpl);
        if (mine > MIN_CONTRIBUTION)
        {
            shares.push_back(Share{p, mine}); // ineligible never shrink others' share
            total += mine;
        }
    }
    if (total == 0 || shares.empty())
    {
        return;
    }
    std::stable_sort(shares.begin(), shares.end(), [](const Share &a, const Share &b) {
        if (a.amount != b.amount)
        {
            return a.amount > b.amount; // amount descending
        }
        return a.who < b.who; // ties: lower characterId first (R7)
    });
    std::uint64_t paid = 0;
    for (const Share &s : shares)
    {
        const std::uint64_t pay = pool * s.amount / total;
        giveReward(s.who, pay, tpl, ctx);
        paid += pay;
    }
    giveReward(shares[0].who, pool - paid, tpl, ctx); // flooring remainder → top contributor
}

void payEventRewards(
    Event &ev, const EventTemplate &tpl, WorldContext &ctx
)
{
    // Rewards are per template, OPTIONAL (the doc: "States may or may not
    // give rewards"): a template without a rewardTable, or with an
    // all-zero one, pays nothing at any payout point — a no-op, not an
    // error.
    if (!tpl.hasRewardTable || tpl.rewards.allZero())
    {
        return;
    }
    distributeRewards(ev, rewardPool(ev, tpl), tpl, ctx);
}

} // namespace harrowmere::events
