// common.cpp - the panels' shared implementation: the visibility defaults,
// the submit helpers (the panels' single write path), the time-controls
// application and the small formatters.
//
// The submit helpers are deliberately the ONLY place a panel builds a
// SimCommand: buttons call them, the pinned tests call them, and the
// queue-not-apply contract (TestUi_CommandsQueueNotApply) has exactly one
// seam to verify. No helper mutates anything - submit queues; tick step 9
// applies (§4.2 rule 2).

#include "panels/panels.hpp"

#include <cstdio>

namespace harrowmere::ui
{

PanelVisibility defaultPanelVisibility()
{
    PanelVisibility v{};
    v.open.fill(false);
    // HUD: the always-on surface (its `open` mirrors the overlay's
    // visibility; the draw is gated by alwaysOn in the binding table).
    v.open[static_cast<std::size_t>(PanelId::Hud)] = true;
    return v;
}

// ---- the submit helpers ------------------------------------------------------

void submitMove(
    PanelContext &ctx, const harrowmere::MoveIntent &move
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::Move;
    c.move = move;
    ctx.sim->submit(c);
}

void submitAttack(
    PanelContext &ctx, std::uint64_t target
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind          = harrowmere::SimCommand::Kind::Attack;
    c.attack.target = target;
    ctx.sim->submit(c);
}

void submitBuy(
    PanelContext &ctx, std::uint32_t good, std::uint32_t qty
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind       = harrowmere::SimCommand::Kind::Buy;
    c.trade.good = good;
    c.trade.qty  = qty;
    ctx.sim->submit(c);
}

void submitSell(
    PanelContext &ctx, std::uint32_t good, std::uint32_t qty
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind       = harrowmere::SimCommand::Kind::Sell;
    c.trade.good = good;
    c.trade.qty  = qty;
    ctx.sim->submit(c);
}

void submitCraft(
    PanelContext &ctx, const std::string &recipeKey
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind            = harrowmere::SimCommand::Kind::Craft;
    c.craft.recipeKey = recipeKey;
    ctx.sim->submit(c);
}

void submitEquip(
    PanelContext &ctx, std::uint64_t itemInstanceId
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind                        = harrowmere::SimCommand::Kind::Equip;
    c.itemInstance.itemInstanceId = itemInstanceId;
    ctx.sim->submit(c);
}

void submitUnequip(
    PanelContext &ctx, std::uint64_t itemInstanceId
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind                        = harrowmere::SimCommand::Kind::Unequip;
    c.itemInstance.itemInstanceId = itemInstanceId;
    ctx.sim->submit(c);
}

void submitConsume(
    PanelContext &ctx, const std::string &itemKey
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind            = harrowmere::SimCommand::Kind::Consume;
    c.consume.itemKey = itemKey;
    ctx.sim->submit(c);
}

void submitBuyStat(
    PanelContext &ctx, std::uint32_t stat
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind      = harrowmere::SimCommand::Kind::BuyStat;
    c.stat.stat = stat;
    ctx.sim->submit(c);
}

void submitBuySkill(
    PanelContext &ctx, std::uint32_t skill
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind        = harrowmere::SimCommand::Kind::BuySkill;
    c.skill.skill = skill;
    ctx.sim->submit(c);
}

void submitBuyLevel(
    PanelContext &ctx
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::BuyLevel;
    ctx.sim->submit(c);
}

void submitRespecStat(
    PanelContext &ctx, std::uint32_t stat
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind      = harrowmere::SimCommand::Kind::RespecStat;
    c.stat.stat = stat;
    ctx.sim->submit(c);
}

void submitRespecSkill(
    PanelContext &ctx, std::uint32_t skill
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind        = harrowmere::SimCommand::Kind::RespecSkill;
    c.skill.skill = skill;
    ctx.sim->submit(c);
}

void submitChangeAppearance(
    PanelContext &ctx, const std::uint64_t *components, std::uint32_t count
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::ChangeAppearance;
    if (components != nullptr)
    {
        const std::uint32_t n
            = count > harrowmere::kMaxAppearanceComponents ? harrowmere::kMaxAppearanceComponents : count;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            c.appearance.components[i] = components[i];
        }
        c.appearance.componentCount = n;
    }
    ctx.sim->submit(c);
}

void submitRequestDuel(
    PanelContext &ctx, std::uint64_t target
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind        = harrowmere::SimCommand::Kind::RequestDuel;
    c.duel.target = target;
    ctx.sim->submit(c);
}

void submitYieldDuel(
    PanelContext &ctx
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::YieldDuel;
    ctx.sim->submit(c);
}

void submitJoinEvent(
    PanelContext &ctx, std::uint64_t eventId
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind          = harrowmere::SimCommand::Kind::JoinEvent;
    c.event.eventId = eventId;
    ctx.sim->submit(c);
}

void submitLeaveEvent(
    PanelContext &ctx, std::uint64_t eventId
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind          = harrowmere::SimCommand::Kind::LeaveEvent;
    c.event.eventId = eventId;
    ctx.sim->submit(c);
}

void submitReportCrime(
    PanelContext &ctx, std::uint64_t target
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind          = harrowmere::SimCommand::Kind::ReportCrime;
    c.target.target = target;
    ctx.sim->submit(c);
}

void submitBribe(
    PanelContext &ctx, std::uint64_t target
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind          = harrowmere::SimCommand::Kind::Bribe;
    c.target.target = target;
    ctx.sim->submit(c);
}

void submitSurrender(
    PanelContext &ctx
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::Surrender;
    ctx.sim->submit(c);
}

void submitServeSentence(
    PanelContext &ctx
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::ServeSentence;
    ctx.sim->submit(c);
}

void submitAttemptEscape(
    PanelContext &ctx
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::AttemptEscape;
    ctx.sim->submit(c);
}

void submitClaimTerritory(
    PanelContext &ctx, std::uint64_t territory
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind            = harrowmere::SimCommand::Kind::ClaimTerritory;
    c.claim.territory = territory;
    ctx.sim->submit(c);
}

void submitInstallLaws(
    PanelContext &ctx, std::uint64_t territory, const std::string &lawSetKey
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind            = harrowmere::SimCommand::Kind::InstallLaws;
    c.claim.territory = territory;
    c.claim.lawSetKey = lawSetKey;
    ctx.sim->submit(c);
}

void submitFoundFaction(
    PanelContext &ctx, const std::string &name
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind              = harrowmere::SimCommand::Kind::FoundFaction;
    c.foundFaction.name = name;
    ctx.sim->submit(c);
}

void submitDissolveFaction(
    PanelContext &ctx
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind = harrowmere::SimCommand::Kind::DissolveFaction;
    ctx.sim->submit(c);
}

void submitSetRelation(
    PanelContext &ctx, std::uint32_t faction, std::int32_t relation
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind             = harrowmere::SimCommand::Kind::SetRelation;
    c.faction.faction  = faction;
    c.faction.relation = relation;
    ctx.sim->submit(c);
}

void submitDeclareWar(
    PanelContext &ctx, std::uint32_t faction
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind            = harrowmere::SimCommand::Kind::DeclareWar;
    c.faction.faction = faction;
    ctx.sim->submit(c);
}

void submitDonate(
    PanelContext &ctx, std::uint64_t target, std::int64_t amount
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::SimCommand c{};
    c.kind          = harrowmere::SimCommand::Kind::Donate;
    c.donate.target = target;
    c.donate.amount = amount;
    ctx.sim->submit(c);
}

void applyTimeControls(
    PanelContext &ctx, bool paused, std::uint32_t ticksPerDispatch
)
{
    if (ctx.sim == nullptr)
        return;
    harrowmere::views::TimeControls t{};
    t.paused           = paused;
    t.ticksPerDispatch = ticksPerDispatch == 0 ? 1u : ticksPerDispatch;
    ctx.sim->setTimeControls(t);
    ctx.timeControls = t;
}

// ---- formatters ---------------------------------------------------------------

void formatGameClock(
    std::uint64_t worldTime, char *buf, std::size_t bufSize
)
{
    // A-3's day math: the seconds within the day map to a wall clock
    // (0 = midnight). Presentation only (R5's carve-out; integers here).
    const std::uint64_t secondsOfDay = worldTime % 86400ull;
    const std::uint64_t day          = worldTime / 86400ull;
    const std::uint64_t hour         = (secondsOfDay / 3600ull) % 24ull;
    const std::uint64_t minute       = (secondsOfDay % 3600ull) / 60ull;
    std::snprintf(
        buf,
        bufSize,
        "day %llu, %02u:%02u",
        static_cast<unsigned long long>(day),
        static_cast<unsigned>(hour),
        static_cast<unsigned>(minute)
    );
}

} // namespace harrowmere::ui
