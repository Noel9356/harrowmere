// command_apply.cpp - the step-9 application of the Phase-2 command set.
//
// See command_apply.hpp for the routing contract. Every rejection is a
// rule-level failure: a return value + one WARN log + the UI signal sink
// (§4.2 rule 2) - never an exception (§3.4), never silent.

#include "core/command_apply.hpp"

#include <string>
#include <vector>

#include "character/appearance.hpp"    // changeAppearance (2.6)
#include "combat/consent.hpp"          // requestDuel / yieldDuel (2.7)
#include "core/log.hpp"                // HARROWMERE_LOG_* (0.5's categories)
#include "events/engine.hpp"           // the EventEngine (5.1; Join/Leave owners)
#include "factions/factions.hpp"       // FactionStore (6.1; Found/Dissolve/SetRelation)
#include "items/crafting.hpp"          // craft (2.4)
#include "items/gear.hpp"              // equip / unequip / consumeItem (2.4)
#include "justice/conviction.hpp"      // isArrested / attemptEscape / surrender (4.3)
#include "justice/evidence.hpp"        // attemptBribery / selfReportVictim (4.1)
#include "npc/state.hpp"               // NpcStore (the nearest-guard/witness scans' role + home reads)
#include "progression/matchmaking.hpp" // the unnamed-duel SP policy (6.5 consumer 1)
#include "progression/power.hpp"       // getMatchmakingScore (6.5's A-116 input)
#include "progression/progression.hpp" // buy*/respec* (2.3)
#include "world/spatial_grid.hpp"      // SpatialGrid (the duel paths' radius gather)

namespace harrowmere
{

namespace
{

// ---- the published-vocabulary mirror ----------------------------------------

// SimCommandKind mirrors SimCommand::Kind one-to-one (command_apply.hpp).
// The switch below is the mapping; the static_assert pins the closed-list
// size so a new Kind without a mirror arm is a compile error, not a
// misrouted command.
static_assert(
    static_cast<int>(SimCommand::Kind::Donate) == 31,
    "SimCommand::Kind is the pinned 32-kind list (card 7.3 + 3.10's Emote); "
    "extend kindOf() and SimCommandKind together"
);

SimCommandKind kindOf(
    const SimCommand &command
)
{
    switch (command.kind)
    {
    case SimCommand::Kind::Move:
        return SimCommandKind::Move;
    case SimCommand::Kind::Attack:
        return SimCommandKind::Attack;
    case SimCommand::Kind::Interact:
        return SimCommandKind::Interact;
    case SimCommand::Kind::Emote:
        return SimCommandKind::Emote;
    case SimCommand::Kind::Buy:
        return SimCommandKind::Buy;
    case SimCommand::Kind::Sell:
        return SimCommandKind::Sell;
    case SimCommand::Kind::Craft:
        return SimCommandKind::Craft;
    case SimCommand::Kind::Equip:
        return SimCommandKind::Equip;
    case SimCommand::Kind::Unequip:
        return SimCommandKind::Unequip;
    case SimCommand::Kind::Consume:
        return SimCommandKind::Consume;
    case SimCommand::Kind::BuyStat:
        return SimCommandKind::BuyStat;
    case SimCommand::Kind::BuySkill:
        return SimCommandKind::BuySkill;
    case SimCommand::Kind::BuyLevel:
        return SimCommandKind::BuyLevel;
    case SimCommand::Kind::RespecStat:
        return SimCommandKind::RespecStat;
    case SimCommand::Kind::RespecSkill:
        return SimCommandKind::RespecSkill;
    case SimCommand::Kind::ChangeAppearance:
        return SimCommandKind::ChangeAppearance;
    case SimCommand::Kind::RequestDuel:
        return SimCommandKind::RequestDuel;
    case SimCommand::Kind::YieldDuel:
        return SimCommandKind::YieldDuel;
    case SimCommand::Kind::JoinEvent:
        return SimCommandKind::JoinEvent;
    case SimCommand::Kind::LeaveEvent:
        return SimCommandKind::LeaveEvent;
    case SimCommand::Kind::ReportCrime:
        return SimCommandKind::ReportCrime;
    case SimCommand::Kind::Bribe:
        return SimCommandKind::Bribe;
    case SimCommand::Kind::Surrender:
        return SimCommandKind::Surrender;
    case SimCommand::Kind::ServeSentence:
        return SimCommandKind::ServeSentence;
    case SimCommand::Kind::AttemptEscape:
        return SimCommandKind::AttemptEscape;
    case SimCommand::Kind::ClaimTerritory:
        return SimCommandKind::ClaimTerritory;
    case SimCommand::Kind::InstallLaws:
        return SimCommandKind::InstallLaws;
    case SimCommand::Kind::FoundFaction:
        return SimCommandKind::FoundFaction;
    case SimCommand::Kind::DissolveFaction:
        return SimCommandKind::DissolveFaction;
    case SimCommand::Kind::SetRelation:
        return SimCommandKind::SetRelation;
    case SimCommand::Kind::DeclareWar:
        return SimCommandKind::DeclareWar;
    case SimCommand::Kind::Donate:
        return SimCommandKind::Donate;
    }
    return SimCommandKind::Move; // unreachable over the closed list (deterministic recovery, §3.4)
}
// The rejection path: one WARN, one signal-sink call. `reason` is a literal
// (rule-level failure; no formatting on the hot path). Category `ui`: the
// write path of the UI surface (0.5's pinned token list).
void reject(
    WorldContext &ctx, SimCommandKind kind, const char *reason
)
{
    HARROWMERE_LOG_WARN(ui, "command %d rejected: %s", static_cast<int>(kind), reason);
    if (ctx.signals != nullptr)
        ctx.signals->onCommandRejected(kind, reason);
}

// PurchaseError -> the literal reason string (rule-level failures are return
// values, §3.4; the strings name the gate, not the internals).
const char *purchaseReason(
    PurchaseError e
)
{
    switch (e)
    {
    case PurchaseError::InsufficientXp:
        return "purchase refused (insufficient XP)";
    case PurchaseError::AtCap:
        return "purchase refused (stat/skill cap reached)";
    case PurchaseError::RequirementUnmet:
        return "purchase refused (skill requirements unmet)";
    case PurchaseError::ManaBacklash:
        return "purchase failed (mana backlash - the enhancement's pain)";
    case PurchaseError::NotPeaceful:
        return "respec refused (not peaceful)";
    case PurchaseError::CooldownActive:
        return "respec refused (cooldown active)";
    case PurchaseError::AtLevelCap:
        return "purchase refused (level cap)";
    case PurchaseError::BadIndex:
        return "purchase refused (bad stat/skill index)";
    }
    return "purchase refused";
}

// The stat/skill intent ordinal -> enum cast with a bounds check (the
// published arrays are the closed lists; a bad ordinal is a programmer
// error in debug, a logged rejection in release - the same split
// PurchaseError::BadIndex pins).
bool statInBounds(
    std::uint32_t ordinal
)
{
    return ordinal < static_cast<std::uint32_t>(StatType::COUNT);
}

bool skillInBounds(
    std::uint32_t ordinal
)
{
    return ordinal < static_cast<std::uint32_t>(SkillType::COUNT);
}

// The two scores' magnitude — 6.5's A-116 acceptance input (both sides read
// the 2.5 formula; the delta is a magnitude, never signed).
std::uint64_t matchmakingScoreDelta(
    characterId a, characterId b, WorldContext &ctx
)
{
    if (ctx.characters == nullptr || ctx.data == nullptr)
        return 0;
    const std::uint64_t sa = getMatchmakingScore(ctx.characters->get(a), *ctx.data);
    const std::uint64_t sb = getMatchmakingScore(ctx.characters->get(b), *ctx.data);
    return sa > sb ? sa - sb : sb - sa;
}

// FactionError -> the literal reason string (rule-level failures name the
// gate, §3.4).
const char *factionReason(
    factions::FactionError e
)
{
    switch (e)
    {
    case factions::FactionError::UnknownFaction:
        return "faction refused (unknown faction)";
    case factions::FactionError::UnknownCharacter:
        return "faction refused (unknown/invalid character)";
    case factions::FactionError::NotLeader:
        return "faction refused (management is the leader's alone)";
    case factions::FactionError::AlreadyMember:
        return "faction refused (already a member)";
    case factions::FactionError::PlayerFactionExists:
        return "faction refused (the SP player faction already exists)";
    case factions::FactionError::InsufficientCurrency:
        return "faction refused (the founding cost is not covered)";
    case factions::FactionError::NoTerritoryPresence:
        return "faction refused (no territory presence)";
    case factions::FactionError::NotMember:
        return "faction refused (not a member)";
    case factions::FactionError::BadRank:
        return "faction refused (rank outside 0..9)";
    }
    return "faction refused";
}

// The nearest living sapient adult NPC to `from` (the Bribe/ReportCrime
// default target — the nearest qualifying witness): (distance, id) order,
// R7-deterministic without a sort (the strict < keeps the first minimum over
// the NpcStore's id-ascending scan).
characterId nearestSapientNpc(
    WorldContext &ctx, const Position &from
)
{
    if (ctx.npcs == nullptr || ctx.characters == nullptr || ctx.data == nullptr)
        return 0;
    characterId best = 0;
    double bestD     = 0.0;
    for (const auto &[id, state] : ctx.npcs->all())
    {
        if (!ctx.characters->exists(id) || ctx.characters->isPlayer(id))
            continue;
        const Character &c = ctx.characters->get(id);
        if (!ctx.data->species(c.species).sapient || state.ageYears(ctx.now) < ADULT_YEARS)
            continue;
        const double dx = c.body.position.x - from.x;
        const double dz = c.body.position.z - from.z;
        const double d  = dx * dx + dz * dz;
        if (best == 0 || d < bestD) // ties keep the lower id (the scan order)
        {
            best  = id;
            bestD = d;
        }
    }
    return best;
}

// The nearest guard-role NPC (the Surrender default target — "surrender to
// any guard of the issuing territory"): the garrison roles (4.6's set), the
// same (distance, id) scan.
characterId nearestGuard(
    WorldContext &ctx, const Position &from
)
{
    if (ctx.npcs == nullptr || ctx.characters == nullptr)
        return 0;
    characterId best = 0;
    double bestD     = 0.0;
    for (const auto &[id, state] : ctx.npcs->all())
    {
        if (!ctx.characters->exists(id))
            continue;
        const bool guard = state.roleKey == "guard" || state.roleKey == "captain" || state.roleKey == "militia_veteran";
        if (!guard)
            continue;
        const Character &c = ctx.characters->get(id);
        const double dx    = c.body.position.x - from.x;
        const double dz    = c.body.position.z - from.z;
        const double d     = dx * dx + dz * dz;
        if (best == 0 || d < bestD)
        {
            best  = id;
            bestD = d;
        }
    }
    return best;
}

} // anonymous namespace

// ---- the one-command application --------------------------------------------

bool applyCommand(
    WorldContext &ctx, CommandServices &services, const SimCommand &command
)
{
    // The four task-3.10 intents: forwarded to player::applyIntents (their
    // pinned consumer). applyCommand's contract says false = "not mine".
    switch (command.kind)
    {
    case SimCommand::Kind::Move:
    case SimCommand::Kind::Attack:
    case SimCommand::Kind::Interact:
    case SimCommand::Kind::Emote:
        return false;
    default:
        break;
    }

    // Every Phase-2 path operates on the SP player (the panels and the input
    // layer act for the local player; NPC purchases are 3.4's auto-buy, not
    // commands).
    if (services.characters == nullptr)
    {
        reject(ctx, kindOf(command), "runtime not bound (no store)");
        return true;
    }
    const characterId playerId = ctx.world != nullptr ? ctx.world->playerCharacterId() : 0;
    if (playerId == 0)
    {
        reject(ctx, kindOf(command), "no player in-world");
        return true;
    }
    Character &player = services.characters->get(playerId);

    switch (command.kind)
    {
    // ---- progression (2.3) -------------------------------------------------
    case SimCommand::Kind::BuyLevel: {
        const PurchaseResult r = buyLevel(player, /*automatic=*/false);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::BuyLevel, purchaseReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::BuyStat: {
        if (!statInBounds(command.stat.stat))
        {
            reject(ctx, SimCommandKind::BuyStat, "stat ordinal out of range");
            return true;
        }
        const PurchaseResult r = buyStat(player, static_cast<StatType>(command.stat.stat), ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::BuyStat, purchaseReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::BuySkill: {
        if (!skillInBounds(command.skill.skill))
        {
            reject(ctx, SimCommandKind::BuySkill, "skill ordinal out of range");
            return true;
        }
        const PurchaseResult r = buySkill(player, static_cast<SkillType>(command.skill.skill), ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::BuySkill, purchaseReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::RespecStat: {
        if (!statInBounds(command.stat.stat))
        {
            reject(ctx, SimCommandKind::RespecStat, "stat ordinal out of range");
            return true;
        }
        const PurchaseResult r = respecStat(player, static_cast<StatType>(command.stat.stat), ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::RespecStat, purchaseReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::RespecSkill: {
        if (!skillInBounds(command.skill.skill))
        {
            reject(ctx, SimCommandKind::RespecSkill, "skill ordinal out of range");
            return true;
        }
        const PurchaseResult r = respecSkill(player, static_cast<SkillType>(command.skill.skill), ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::RespecSkill, purchaseReason(r.error()));
        }
        return true;
    }

    // ---- items (2.4) --------------------------------------------------------
    case SimCommand::Kind::Equip:
    case SimCommand::Kind::Unequip: {
        if (services.bags == nullptr || services.instances == nullptr || ctx.data == nullptr)
        {
            reject(ctx, kindOf(command), "bag services not bound");
            return true;
        }
        items::Inventory &bag = services.bags->bag(playerId);
        const GearResult r
            = (command.kind == SimCommand::Kind::Equip)
                ? equip(player, bag, *services.instances, *ctx.data, command.itemInstance.itemInstanceId)
                : unequip(player, bag, *services.instances, *ctx.data, command.itemInstance.itemInstanceId);
        if (r.isErr())
        {
            reject(ctx, kindOf(command), "equip/unequip refused (ownership, slot or item rules)");
        }
        return true;
    }
    case SimCommand::Kind::Consume: {
        if (services.bags == nullptr || ctx.data == nullptr)
        {
            reject(ctx, SimCommandKind::Consume, "bag services not bound");
            return true;
        }
        items::Inventory &bag = services.bags->bag(playerId);
        const ConsumeResult r = consumeItem(player, bag, *ctx.data, command.consume.itemKey);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::Consume, "consume refused (unknown/not consumable/not owned)");
        }
        return true;
    }
    case SimCommand::Kind::Craft: {
        if (services.bags == nullptr || services.instances == nullptr || services.recipes == nullptr
            || ctx.data == nullptr)
        {
            reject(ctx, SimCommandKind::Craft, "craft services not bound");
            return true;
        }
        items::Inventory &bag = services.bags->bag(playerId);
        const CraftResult r   = craft(
            player,
            bag,
            *services.instances,
            *ctx.data,
            *services.recipes,
            command.craft.recipeKey,
            services.player != nullptr ? &services.player->actions : nullptr
        );
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::Craft, "craft refused (inputs, skill or recipe)");
        }
        return true;
    }

    // ---- appearance (2.6) ----------------------------------------------------
    case SimCommand::Kind::ChangeAppearance: {
        const std::uint32_t count = command.appearance.componentCount > kMaxAppearanceComponents
                                      ? kMaxAppearanceComponents
                                      : command.appearance.componentCount;
        const std::vector<std::uint64_t> components(
            command.appearance.components.begin(),
            command.appearance.components.begin() + static_cast<std::ptrdiff_t>(count)
        );
        const AppearanceResult r = changeAppearance(player, components, ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::ChangeAppearance, "appearance change refused (cooldown or XP)");
        }
        return true;
    }

    // ---- duels (2.7; the unnamed request's SP policy is 6.5's) --------------
    case SimCommand::Kind::RequestDuel: {
        if (services.duels == nullptr)
        {
            reject(ctx, SimCommandKind::RequestDuel, "duel registry not bound");
            return true;
        }
        if (command.duel.target == 0)
        {
            // 6.5 consumer 1: the unnamed request proposes the closest-score
            // NEAR+MID NPC and rolls the A-116 acceptance; acceptance records
            // the duel through the same 2.7 pair (request + on-the-spot
            // consent). Rejections are rule-level refusals (§3.4).
            const characterId proposal = progression::proposeDuelOpponent(playerId, ctx);
            if (proposal == 0)
            {
                reject(ctx, SimCommandKind::RequestDuel, "duel refused (no candidate nearby)");
                return true;
            }
            const std::uint64_t delta = matchmakingScoreDelta(playerId, proposal, ctx);
            if (!progression::npcAcceptsDuel(progression::duelAcceptancePct(delta), ctx))
            {
                reject(ctx, SimCommandKind::RequestDuel, "duel refused (the proposal declined)");
                return true;
            }
            const combat::DuelResult request
                = combat::requestDuel(playerId, proposal, *services.duels, *services.characters, ctx.now);
            if (request.isErr())
            {
                reject(ctx, SimCommandKind::RequestDuel, "duel request refused (self/absent/already dueling)");
                return true;
            }
            world::SpatialGrid fallbackGrid{};
            const combat::DuelResult acceptance = combat::acceptDuel(
                proposal,
                playerId,
                *services.duels,
                *services.characters,
                services.grid != nullptr ? *services.grid : fallbackGrid,
                nullptr,
                ctx.now
            );
            if (acceptance.isErr())
            {
                reject(ctx, SimCommandKind::RequestDuel, "duel acceptance refused (request/dueling state)");
            }
            return true;
        }
        if (!services.characters->exists(command.duel.target))
        {
            reject(ctx, SimCommandKind::RequestDuel, "duel target does not exist");
            return true;
        }
        const combat::DuelResult r
            = combat::requestDuel(playerId, command.duel.target, *services.duels, *services.characters, ctx.now);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::RequestDuel, "duel request refused (self/absent/already dueling)");
        }
        return true;
    }
    case SimCommand::Kind::YieldDuel: {
        if (services.duels == nullptr)
        {
            reject(ctx, SimCommandKind::YieldDuel, "duel registry not bound");
            return true;
        }
        // The broadcast sink is Phase 3's justice seam; a null broadcast is
        // the consent module's own null default (2.7's signature). The grid
        // gathers the yield's observers (re-watched at the midpoint); an
        // empty fallback grid degrades to no observers (headless/tests).
        world::SpatialGrid fallbackGrid{};
        const combat::DuelResult r = combat::yieldDuel(
            playerId,
            *services.duels,
            *services.characters,
            services.grid != nullptr ? *services.grid : fallbackGrid,
            nullptr,
            ctx.now
        );
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::YieldDuel, "yield refused (not dueling)");
        }
        return true;
    }

    // ---- owners that have not landed (logged rejections; seams #1-#4) -------
    case SimCommand::Kind::Buy:
    case SimCommand::Kind::Sell:
        // 3.7's EconomyStore is built; its runtime wiring (goods table +
        // vendor positions) lands with the 3.x integration - the same
        // pending-wiring line the plugin carries for every store.
        reject(ctx, kindOf(command), "trade lands with the 3.x runtime wiring");
        return true;

    // ---- justice (4.1/4.3/4.6; the panel paths' targeted forms) -------------
    case SimCommand::Kind::ReportCrime: {
        // 4.1's one player-report path is the victim self-report (the doc's
        // rule: witnesses report automatically; player VICTIMS self-report
        // post-respawn). `target` names the accused and is validated when
        // nonzero; target 0 falls back to the nearest qualifying witness
        // (7.3b's spec line) as the accusation's witness context.
        const characterId target
            = command.target.target != 0 ? command.target.target : nearestSapientNpc(ctx, player.body.position);
        if (target != 0 && !services.characters->exists(target))
        {
            reject(ctx, SimCommandKind::ReportCrime, "report refused (unknown accused)");
            return true;
        }
        if (!justice::selfReportVictim(playerId, ctx))
        {
            reject(ctx, SimCommandKind::ReportCrime, "report refused (no reportable crime of yours)");
        }
        return true;
    }
    case SimCommand::Kind::Bribe: {
        // 4.1's bribery path: the named target, or the nearest qualifying
        // witness when the panel submits the untargeted form (7.3b).
        const characterId witness
            = command.target.target != 0 ? command.target.target : nearestSapientNpc(ctx, player.body.position);
        if (witness == 0 || !services.characters->exists(witness))
        {
            reject(ctx, SimCommandKind::Bribe, "bribe refused (no witness to bribe)");
            return true;
        }
        const justice::BriberyOutcome outcome = justice::attemptBribery(playerId, witness, ctx);
        if (outcome == justice::BriberyOutcome::NoReport)
        {
            reject(ctx, SimCommandKind::Bribe, "bribe refused (the witness holds no report on you)");
        }
        // Bribed and Failed are real outcomes (the halving / the fresh
        // OBSTRUCTION evidence) — they are not rejections.
        return true;
    }
    case SimCommand::Kind::Surrender: {
        // 4.3's surrender: the named guard, or the nearest guard-role NPC
        // (7.3b's spec line — "surrender to any guard of the issuing
        // territory"). The guard's home territory is the issuing territory
        // proxy; a surrender that converts no warrant is a rule refusal.
        const characterId guard
            = command.target.target != 0 ? command.target.target : nearestGuard(ctx, player.body.position);
        if (guard == 0 || !services.characters->exists(guard))
        {
            reject(ctx, SimCommandKind::Surrender, "surrender refused (no guard to surrender to)");
            return true;
        }
        const territoryId guardTerritory
            = ctx.npcs != nullptr && ctx.npcs->contains(guard) ? ctx.npcs->get(guard).homeTerritory : 0;
        if (!justice::surrender(playerId, guardTerritory, ctx))
        {
            reject(ctx, SimCommandKind::Surrender, "surrender refused (no live warrant there)");
        }
        return true;
    }

    // ---- jail (4.3; D-JAIL-1) ------------------------------------------------
    case SimCommand::Kind::ServeSentence: {
        if (ctx.justiceStore == nullptr || !justice::isArrested(*ctx.justiceStore, playerId, ctx.now))
        {
            reject(ctx, SimCommandKind::ServeSentence, "serve refused (not jailed)");
            return true;
        }
        // D-JAIL-1: the fast-forward runs the pinned pipeline at max speed
        // until jailUntil. The stamp rides CommandServices; the PLUGIN's
        // fixed-update drain services it (never inside the tick — the
        // §4.4 reentrancy rule).
        services.fastForwardUntil = ctx.justiceStore->jailUntil[playerId];
        return true;
    }
    case SimCommand::Kind::AttemptEscape: {
        // 4.3's skill check; failure doubles the sentence (the doc rule).
        if (ctx.justiceStore == nullptr || !justice::isArrested(*ctx.justiceStore, playerId, ctx.now))
        {
            reject(ctx, SimCommandKind::AttemptEscape, "escape refused (not jailed)");
            return true;
        }
        justice::attemptEscape(playerId, ctx);
        return true;
    }

    // ---- events (5.1) ----------------------------------------------------------
    case SimCommand::Kind::JoinEvent: {
        if (services.eventEngine == nullptr)
        {
            reject(ctx, SimCommandKind::JoinEvent, "event engine not bound");
            return true;
        }
        const events::Event *ev = services.eventEngine->find(command.event.eventId);
        if (ev == nullptr)
        {
            reject(ctx, SimCommandKind::JoinEvent, "join refused (no such event)");
            return true;
        }
        if (ev->targetIds.contains(playerId))
        {
            reject(ctx, SimCommandKind::JoinEvent, "join refused (you are this event's target)");
            return true; // a hunted player cannot join their own manhunt
        }
        services.eventEngine->join(playerId, command.event.eventId, ctx);
        return true;
    }
    case SimCommand::Kind::LeaveEvent: {
        if (services.eventEngine == nullptr)
        {
            reject(ctx, SimCommandKind::LeaveEvent, "event engine not bound");
            return true;
        }
        if (services.eventEngine->find(command.event.eventId) == nullptr)
        {
            reject(ctx, SimCommandKind::LeaveEvent, "leave refused (no such event)");
            return true;
        }
        services.eventEngine->leaveEarly(playerId, command.event.eventId, ctx);
        return true;
    }

    // ---- factions (6.1; the war arm is Phase 4's) --------------------------------
    case SimCommand::Kind::FoundFaction: {
        if (services.factions == nullptr)
        {
            reject(ctx, SimCommandKind::FoundFaction, "faction store not bound");
            return true;
        }
        const factions::FactionResult r
            = services.factions->foundPlayerFaction(playerId, command.foundFaction.name, ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::FoundFaction, factionReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::DissolveFaction: {
        if (services.factions == nullptr)
        {
            reject(ctx, SimCommandKind::DissolveFaction, "faction store not bound");
            return true;
        }
        const factions::FactionResult r = services.factions->dissolveFaction(playerId, ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::DissolveFaction, factionReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::SetRelation: {
        if (services.factions == nullptr)
        {
            reject(ctx, SimCommandKind::SetRelation, "faction store not bound");
            return true;
        }
        const factionId ownFaction = services.factions->factionOfCharacter(playerId);
        if (ownFaction == 0)
        {
            reject(ctx, SimCommandKind::SetRelation, "relation refused (you hold no faction)");
            return true;
        }
        const factions::FactionResult r
            = services.factions->setRelation(ownFaction, command.faction.faction, command.faction.relation, ctx);
        if (r.isErr())
        {
            reject(ctx, SimCommandKind::SetRelation, factionReason(r.error()));
        }
        return true;
    }
    case SimCommand::Kind::ClaimTerritory:
    case SimCommand::Kind::InstallLaws:
    case SimCommand::Kind::DeclareWar:
    case SimCommand::Kind::Donate:
        reject(ctx, kindOf(command), "claims/war arrive in Phase 4 (6.2/6.3)");
        return true;

    // The four 3.10 intents never reach this switch (returned above); this
    // arm only satisfies -Wswitch over the closed list.
    case SimCommand::Kind::Move:
    case SimCommand::Kind::Attack:
    case SimCommand::Kind::Interact:
    case SimCommand::Kind::Emote:
        return false;
    }
    return false; // unreachable (closed list)
}

// ---- the queue drain -----------------------------------------------------------

std::size_t applyQueuedCommands(
    WorldContext &ctx, CommandServices &services, player::IntentQueue &intentsOut
)
{
    if (services.queue == nullptr)
    {
        return 0; // nothing bound: submit() already warns on the producer side
    }

    std::size_t consumed = 0;
    while (!services.queue->empty())
    {
        const SimCommand command = services.queue->pop();
        ++consumed;
        if (!applyCommand(ctx, services, command))
        {
            intentsOut.push(command); // the 3.10 intents, FIFO order preserved
        }
    }
    return consumed;
}

// ---- the step-9 system wrapper -------------------------------------------------

void PlayerCommandSystem::run(
    WorldContext &ctx
)
{
    forwarded_.clear();
    applyQueuedCommands(ctx, services_, forwarded_);

    // The 3.10 intents of this slice: the pinned consumer (controller.hpp's
    // drain rules) - motion state, the interact gate, the AFK clock. A
    // missing clock/player binding is the pre-3.1 runtime shape: the intents
    // are dropped with one WARN per slice (never re-queued, never re-ordered).
    if (services_.clock == nullptr || services_.player == nullptr)
    {
        if (!forwarded_.empty())
        {
            HARROWMERE_LOG_WARN(
                ui, "intents dropped: runtime clock/player not bound (%zu this slice)", forwarded_.size()
            );
            forwarded_.clear();
        }
        return;
    }
    player::applyIntents(
        *services_.player, forwarded_, *services_.clock, services_.fixedStepSeconds, resolver_, handlers_
    );
}

} // namespace harrowmere
