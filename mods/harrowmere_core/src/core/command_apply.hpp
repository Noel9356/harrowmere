#ifndef HARROWMERE_CORE_CORE_COMMAND_APPLY_HPP
#define HARROWMERE_CORE_CORE_COMMAND_APPLY_HPP

// command_apply - the step-9 application of the published command set
// (tasks 7.3/7.4; common.md §4.2 rule 2: "submit queues; step 9 applies").
//
// The four task-3.10 intents (Move / Attack / Interact / Emote) keep their
// pinned home: player::applyIntents drains them (motion state, the interact
// gate, the action log). The Phase-2 commands (card 7.3's list) route HERE,
// through applyQueuedCommands(), into the owner systems that exist today:
//
//   BuyStat / BuySkill / BuyLevel  -> progression::buyStat/buySkill/buyLevel (2.3)
//   RespecStat / RespecSkill       -> the doc respec (getRefund literal, 2.3)
//   Equip / Unequip                -> gear::equip/unequip (2.4)
//   Consume                        -> gear::consumeItem (2.4)
//   Craft                          -> crafting::craft (2.4)
//   ChangeAppearance               -> appearance::changeAppearance (2.6)
//   RequestDuel / YieldDuel        -> consent::requestDuel/yieldDuel (2.7)
//
// Everything whose owner system has not landed (Buy/Sell -> the economy's
// runtime wiring; ReportCrime/Bribe/Surrender/JoinEvent/LeaveEvent/
// ServeSentence/AttemptEscape -> Phase 3; ClaimTerritory..Donate -> Phase 4)
// is REJECTED with a logged reason that surfaces through the UI signal sink
// (§4.2 rule 2; the phase file's "You leave these seams" #4 pins exactly
// this shape: "justice/event commands no-op with a logged rejection until
// their systems exist").
//
// SimCommandKind mirrors the capability envelope's SimCommand::Kind with a
// u8 underlying type - core/world_context.hpp forward-declares exactly that
// spelling for IUiSignalSink::onCommandRejected. The mapping is a switch
// over the published enum; the closed-list static_asserts catch a silent
// drift when either list grows.

#include <cstdint>

// The store first (it pulls worldgen.hpp), THEN world_context.hpp - whose
// interface spells the store/data types unqualified via its own
// using-declarations (the folded-back world_context_aliases.hpp fix).
#include "character/store.hpp"   // CharacterStore (2.0; built)
#include "factions/factions.hpp" // FactionStore (6.1; the Found/Dissolve/SetRelation owner)

#include "combat/consent.hpp"        // ConsentRegistry (2.7; built)
#include "core/scheduler.hpp"        // IStepSystem (3.1's registration surface)
#include "core/world_context.hpp"    // WorldContext, IUiSignalSink (the rejection seam)
#include "items/crafting.hpp"        // RecipeTable (2.4; built)
#include "items/inventory.hpp"       // InventoryStore, ItemInstanceRegistry (2.4; built)
#include "player/controller.hpp"     // IntentQueue (the queue submit() fills)
#include "harrowmere_capability.hpp" // SimCommand (the published vocabulary)

namespace harrowmere
{

namespace events
{
class EventEngine; // events/engine.hpp (5.1) — pointer member only (the Join/Leave owner)
}

namespace world
{
class SpatialGrid; // world/spatial_grid.hpp (1.6; the duel paths' radius gather)
}

// The step-9 mirror of SimCommand::Kind (u8; the sink's parameter type):
// card 7.3's pinned list with 3.10's Emote slotted after Interact (32
// values; see the capability header's block comment).
enum class SimCommandKind : std::uint8_t
{
    Move,
    Attack,
    Interact,
    Emote,
    Buy,
    Sell,
    Craft,
    Equip,
    Unequip,
    Consume,
    BuyStat,
    BuySkill,
    BuyLevel,
    RespecStat,
    RespecSkill,
    ChangeAppearance,
    RequestDuel,
    YieldDuel,
    JoinEvent,
    LeaveEvent,
    ReportCrime,
    Bribe,
    Surrender,
    ServeSentence,
    AttemptEscape,
    ClaimTerritory,
    InstallLaws,
    FoundFaction,
    DissolveFaction,
    SetRelation,
    DeclareWar,
    Donate
};

// ---- the runtime services the command paths need ----------------------------

// WorldContext carries the stores; the few singletons that live in the
// runtime composition (the duel registry, the player's bag store, the item
// instance registry, the recipe table, the player controller + queue) ride
// this bundle. Pointers default null: a missing service degrades its
// commands to logged rejections, never a crash (§4.0's degrade-if-absent
// contract).
struct CommandServices
{
    player::IntentQueue *queue             = nullptr; // the queue submit() fills (step 9 drains it here)
    player::PlayerController *player       = nullptr; // the sim-side player (3.10; the craft counter)
    character::CharacterStore *characters  = nullptr; // shared with WorldContext; re-pointed for convenience
    items::InventoryStore *bags            = nullptr; // 2.4's player-side bag store
    items::ItemInstanceRegistry *instances = nullptr; // 2.4's instance-id space
    RecipeTable *recipes                   = nullptr; // 2.4's recipe table (recipes.json)
    combat::ConsentRegistry *duels         = nullptr; // 2.7's consent registry
    const world::SpatialGrid *grid         = nullptr; // 1.6's index (the duel paths' radius gather)
    const core::WorldClock *clock          = nullptr; // the AFK/integration clock (3.10; read-only here)
    double fixedStepSeconds                = 0.1;     // the engine fixed step (1/tickRate; the plugin sets it)
    events::EventEngine *eventEngine       = nullptr; // 5.1's engine (Join/Leave); null = rejections
    factions::FactionStore *factions       = nullptr; // 6.1's store (Found/Dissolve/SetRelation); null = rejections
    // D-JAIL-1's fast-forward request (7.3b): ServeSentence stamps the
    // arrested player's sentence end; the PLUGIN's fixed-update drain runs
    // the pinned pipeline at max speed until it (never inside the tick —
    // the §4.4 reentrancy rule). 0 = no request pending.
    std::uint64_t fastForwardUntil = 0;
};

// Drain `services.queue` once: Phase-2 commands apply through their owner
// systems; the four 3.10 intents are re-queued into `intentsOut` (FIFO order
// preserved) for player::applyIntents - the pre-existing step-9 consumer
// stays the sole owner of the intent semantics (controller.hpp's drain
// rules), and this function never duplicates them.
//
// Returns the number of commands consumed in total (applied + rejected +
// forwarded). Rejections log one WARN each (the per-tick hot-loop rule keeps
// them off the TRACE-only path: command traffic is player-paced, not
// per-NPC-per-tick) and surface through WorldContext::signals'
// onCommandRejected (§4.2 rule 2).
std::size_t applyQueuedCommands(WorldContext &ctx, CommandServices &services, player::IntentQueue &intentsOut);

// Apply ONE command directly (the queue drain above is a thin loop over
// this). Returns true when the command was consumed here (applied or
// rejected); false when it is one of the four 3.10 intents and belongs to
// player::applyIntents (the caller forwards it untouched).
bool applyCommand(WorldContext &ctx, CommandServices &services, const SimCommand &command);

// ---- the step-9 system wrapper (3.1's registration surface) ------------------
//
// Registers into TickStep::Player BEFORE the plugin's legacy direct
// applyIntents call is retired (3.1's re-registration note): one run() = one
// fixed update = drain the submit queue (Phase-2 commands apply here) then
// hand the four 3.10 intents to player::applyIntents - the pinned intent
// semantics stay in their original owner, and the command paths join the
// same FIFO slice. The resolver/handlers are the plugin's registered
// interaction table (3.10's owner wiring); defaults keep the null shape.
class PlayerCommandSystem final : public IStepSystem
{
public:
    explicit PlayerCommandSystem(
        CommandServices services,
        const player::InteractionResolver &resolver = player::nullInteractionResolver(),
        const player::InteractionHandlers &handlers = player::InteractionHandlers::empty()
    )
        : services_(services), resolver_(resolver), handlers_(handlers)
    {}

    void run(WorldContext &ctx) override;

    // The services accessor (the runtime's one door to the step-9 state): the
    // plugin reads the D-JAIL-1 fast-forward stamp ServeSentence set and the
    // tests drain against the same surface. The copy this system owns IS the
    // live wiring — mutate through this accessor, never a stale copy.
    CommandServices &services()
    {
        return services_;
    }
    const CommandServices &services() const
    {
        return services_;
    }

private:
    CommandServices services_;
    player::IntentQueue forwarded_; // the 3.10 intents of this slice (scratch)
    const player::InteractionResolver &resolver_;
    const player::InteractionHandlers &handlers_;
};

} // namespace harrowmere

#endif // HARROWMERE_CORE_CORE_COMMAND_APPLY_HPP
