// core/world_context.hpp — the cross-store lookup bundle (task 2.1's WorldContext,
// extended by Phase-2 cards 3.1–3.7).
//
// Passed by systems, never stored (2.1). Every cross-layer call goes through the
// interfaces below with null defaults (D-SINK-1 + "You leave these seams" #1/#2):
// task 4.1/4.2/4.3 swap the justice sink, 5.3 the events sink, 3.9 perception,
// 4.5 the price-modifier hook — none of that touches this phase's code.
//
// MERGE NOTE: this header supersedes the Phase-1 (2.1) world_context.hpp — it keeps
// the pinned members (`now`, `wars`, `justice`, ...) and adds the stores/systems the
// Phase-2 cards' pseudocode references (ctx.world, ctx.factions, ctx.events, ...).
// The nested store/data spellings (CharacterStore, DataRegistry, SeedRng, NpcSeed)
// are imported into harrowmere below — the tree-wide home of the stopgap aliases
// (core/world_context_aliases.hpp, deleted when this landed; goodId went to
// core/types.hpp).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "character/store.hpp"
#include "combat/justice_sink.hpp"
#include "core/constants.hpp"
#include "core/rng.hpp"
#include "core/types.hpp"
#include "data/registry.hpp"
#include "world/worldgen.hpp"

namespace harrowmere
{

// The four nested spellings the Phase-2 cards' pseudocode uses unqualified
// (`ctx.data`, `ctx.characters`, `ctx.rng`, worldgen seeds). One import each,
// per the deleted aliases header's own instruction.
using character::CharacterStore;
using core::SeedRng;
using data::DataRegistry;
using world::NpcSeed;

class Scheduler;    // core/scheduler.hpp
class NpcStore;     // npc/state.hpp
class EconomyStore; // economy/economy.hpp
class CorpseStore;  // combat/death.hpp
class LodActiveSet; // npc/lod.hpp
class CombatQueue;  // combat/attacks.hpp
class RoleRegistry; // npc/roles.hpp

namespace character
{
class AppearanceHistory; // appearance.hpp (task 2.6) — pointer member only
}

namespace items
{
struct Inventory;           // items/inventory.hpp (2.4) — the bag record IBagSource serves
class ItemInstanceRegistry; // items/inventory.hpp (2.4) — the instance-id space
} // namespace items

namespace player
{
struct PlayerActions; // controller.hpp (3.10) — the act surface 4.1's markers read; pointer member only
}

namespace world
{
class WorldStore;  // world.hpp (1.4) — the doc walks (D-JUR-1) + territory archives; pointer member only
class SpatialGrid; // spatial_grid.hpp (1.6) — the one spatial-query path; pointer member only
} // namespace world

namespace justice
{
class JusticeStore; // justice/justice_store.hpp (4.x) — evidence/warrants/reputations/jail; pointer member only
}

namespace factions
{
class FactionStore; // factions/factions.hpp (6.1) — the membership/relation reads; pointer member only
class WarStore;     // factions/war.hpp (6.2) — the war registry; pointer member only
class ClaimStore;   // factions/claims.hpp (6.3) — the claim ledger; pointer member only
} // namespace factions

// ---- sim event publication (4.x; the 0.7 struct set is the payload) -----------
//
// The 0.7 game events (CrimeRecorded, WarrantIssued, WarrantResolved, ...) are
// the pinned payload types, but the sim carries no engine bus pointer (the
// WorldContext is kernel-free). This seam is the one door out: null default =
// the publication is dropped deterministically (D-SINK-1), the plugin binds an
// adapter that forwards into the kernel bus channels. Publishing happens AFTER
// the mutation is complete (0.7 rule 1) — the justice functions call these at
// their tails.
struct CrimeRecorded;
struct WarrantIssued;
struct WarrantResolved;
struct EventStarted;
struct EventEnded;
struct EventStateChanged;
struct PlayerSignal;
struct TerritoryChanged; // 0.7's territory fact (Laws/Owner/Safety/War); 6.2/6.3 are its emitters

class ISimEventSink
{
public:
    virtual ~ISimEventSink() = default;
    virtual void onCrimeRecorded(
        const CrimeRecorded & /*ev*/
    )
    {}
    virtual void onWarrantIssued(
        const WarrantIssued & /*ev*/
    )
    {}
    virtual void onWarrantResolved(
        const WarrantResolved & /*ev*/
    )
    {}
    // 5.1's event-engine publications + 4.6's PlayerSignal queue (the 0.7
    // struct set; null defaults keep the D-SINK-1 pattern — the publication
    // drops deterministically until the plugin binds the bus adapter).
    virtual void onEventStarted(
        const EventStarted & /*ev*/
    )
    {}
    virtual void onEventEnded(
        const EventEnded & /*ev*/
    )
    {}
    virtual void onEventStateChanged(
        const EventStateChanged & /*ev*/
    )
    {}
    virtual void onPlayerSignal(
        const PlayerSignal & /*ev*/
    )
    {}
    // The 0.7 territory fact (Laws/Owner/Safety/War): 6.2 (declarations,
    // outcome transfers, title moves) and 6.3 (claims, custom laws,
    // dismantles) publish AFTER the mutation is complete (0.7 rule 1). Null
    // default = dropped deterministically (D-SINK-1).
    virtual void onTerritoryChanged(
        const TerritoryChanged & /*ev*/
    )
    {}
};
class NullSimEventSink final : public ISimEventSink
{};

// ---- world queries (1.4 territories / 1.5 worldgen / 3.8 seasons hooks) ----
struct TerritoryInfo
{
    territoryId id     = 0;
    territoryId parent = 0;
    TerritoryType type = TerritoryType::VILLAGE;
    bool safe          = false;
};

class IWorldQueries
{
public:
    virtual ~IWorldQueries() = default;

    virtual territoryId territoryAt(
        const Position & /*p*/
    ) const
    {
        return 0;
    }
    virtual bool isInSafeZone(
        const Position & /*p*/
    ) const
    {
        return false;
    }
    virtual regionId regionAt(
        const Position & /*p*/
    ) const
    {
        return 0;
    }
    virtual TerritoryInfo territoryById(
        territoryId /*id*/
    ) const
    {
        return TerritoryInfo{};
    }
    virtual territoryId territoryOfRegion(
        regionId /*r*/
    ) const
    {
        return 0;
    }

    virtual std::uint32_t regionPopulation(
        regionId /*r*/
    ) const
    {
        return 0;
    }
    virtual std::uint32_t territoryPopulation(
        territoryId /*t*/
    ) const
    {
        return 0;
    }
    virtual void populationBounds(
        territoryId /*t*/, std::uint32_t &minOut, std::uint32_t &maxOut
    ) const
    {
        minOut = 0;
        maxOut = 0;
    }

    virtual Position settlementCenter(
        territoryId /*t*/
    ) const
    {
        return Position{};
    }
    virtual Position safeZoneSpawnFor(
        const Position &deathPosition
    ) const
    {
        return deathPosition;
    }
    virtual Position beginnerSpawnPosition() const
    {
        return Position{};
    } // D-LOD-1 fallback anchor

    // The local player's id (SP: exactly one); 0 = no player in-world (main menu).
    virtual characterId playerCharacterId() const
    {
        return 0;
    }

    // 3.8's shipped hooks, consumed (never redefined) by 3.4/3.6/3.7:
    virtual std::uint64_t seasonYieldScale(
        regionId /*r*/
    ) const
    {
        return SCALE;
    } // A-52 combined, SCALE grid
    virtual std::int64_t migrationClimatePullFor(
        regionId /*r*/
    ) const
    {
        return 0;
    } // frozen warm-climate winter pull (+1000/0)

    virtual void forEachSettlementTerritory(
        const std::function<void(territoryId)> & /*fn*/
    ) const
    {}

    // 1.5's population plan (D-WGEN-2) — 3.4's bootstrap spawns these as living NPCs.
    virtual void forEachPlannedSeed(
        const std::function<void(NpcSeed &)> & /*fn*/
    ) const
    {}

    // Migration pull reads (3.4): the destination's dominant resident species and
    // its laws' culture bias toward a species (1.4's data through lawsAt).
    virtual speciesId dominantSpeciesOf(
        territoryId /*t*/
    ) const
    {
        return 0;
    }

    // 3.4's bias read into 2.2's speciesBiasToward core: the territory's
    // cultureBias toward `self` (laws.json, negative = xenophobia; 1.4's
    // lawsAt walk in the real wiring, D-JUR-1).
    virtual std::int64_t cultureBiasToward(
        territoryId /*t*/, speciesId /*self*/
    ) const
    {
        return 0;
    }

    // 1.5's single-NPC seed path (3.4 births, 3.6 backup migrants). False = cannot.
    virtual bool makeNpcSeed(
        NpcSeed & /*out*/, territoryId /*home*/, speciesId /*species*/, const char * /*roleKey*/
    )
    {
        return false;
    }
};

class NullWorldQueries final : public IWorldQueries
{};

// ---- factions & war (6.1/6.2 own the real registries; null-false until then) ----
class IFactionQueries
{
public:
    virtual ~IFactionQueries() = default;
    virtual bool isAtWar(
        factionId /*a*/, factionId /*b*/
    ) const
    {
        return false;
    }
};
class NullFactionQueries final : public IFactionQueries
{};

class IWarRegistry
{
public:
    virtual ~IWarRegistry() = default;

    // 6.2's flip instant: true iff `faction` is a belligerent of an ACTIVE
    // war whose notice has ended (warEffectivelyActive) — IN_WAR (2.1) and
    // 3.3's isCriminalAct war exemption share the exact same gate through
    // the real registry (the card's one-function pitfall). Null-false until
    // the plugin binds the real WarStore.
    virtual bool isCombatant(
        factionId /*faction*/
    ) const
    {
        return false;
    }

    // 4.7's casus belli hook (frozen stub — 6.2 in Phase 4 implements against
    // this exact signature, D-SINK-1): a village-slaughter marker registered
    // for `origin`'s neighbors vs the culprit's faction. The null default
    // records nothing (no war registry exists until 6.1/6.2 land).
    virtual void registerCasusBelli(
        territoryId /*origin*/, characterId /*culprit*/, factionId /*culpritFaction*/
    )
    {}
};
class NullWarRegistry final : public IWarRegistry
{};

// ---- event-engine seams (5.1 owns the real engine; null defaults until the
// plugin binds it — D-SINK-1; 4.6's manhunt spawn and 4.7's reward exclusion
// go through these, never through an events/ include) --------------------------

// The warrant-shaped seed a manhunt spawn carries (5.4's targetFromWarrant
// reads it; the engine inserts the culprit into the event's targetIds).
struct ManhuntSeed
{
    std::uint64_t warrantId = 0;
    characterId culprit     = 0;
    territoryId territory   = 0; // the warrant's issuing territory
    regionId region         = 0; // the region the event plays out in
};

// The war-shaped seed of the 6.2 BATTLE spawn (D-WAR-EVT-1): one regional
// BATTLE event whose targets are the two belligerents' members, each side a
// target of the other. The WAR MODULE computes the two id lists (faction
// membership is its data; the event engine stays faction-blind — D-SINK-1
// cuts both ways) and hands them over sorted id-ascending (R7).
struct WarBattleSeed
{
    std::uint64_t warId = 0;
    regionId region     = 0; // the contested region the battle plays out in
    factionId attacker  = 0;
    factionId defender  = 0;
    std::vector<characterId> attackerMembers; // living members, id-ascending (R7)
    std::vector<characterId> defenderMembers; // living members, id-ascending (R7)
};

class IEventSpawner
{
public:
    virtual ~IEventSpawner() = default;

    // Start one instance of the named template (the enforcement path only
    // ever names "MANHUNT"). False = refused (unknown template, scale
    // uniqueness, depth cap). The spawn is ENQUEUED — the caller runs in
    // step 6, the event engine ticks at step 5 of the NEXT tick (the §4.4
    // reentrancy rule; never spawn synchronously inside step 6).
    virtual bool spawnEvent(
        const char * /*templateKey*/, const ManhuntSeed & /*seed*/
    )
    {
        return false;
    }

    // 6.2's battle spawn (D-WAR-EVT-1): one BATTLE event in the contested
    // region with the two factions' members joined as the two target groups
    // (targets of each other). Refused when the template is unknown or the
    // scale-uniqueness gate bites (a live BATTLE in the same region). Null
    // default: refused (D-SINK-1 — no war battles without the engine bound).
    virtual bool spawnWarBattle(
        const WarBattleSeed & /*seed*/
    )
    {
        return false;
    }
};
class NullEventSpawner final : public IEventSpawner
{};

// 4.7's reward-exclusion interface ("5.3 flags excludedFromRewards —
// interface provided here"): the smuggling rule marks a helper excluded from
// the shared event's reward distribution and asks whether a culprit is a
// target-side participant of their shared event. Null defaults keep the
// D-SINK-1 pattern (no exclusion, no target-side membership).
class IEventExclusionSink
{
public:
    virtual ~IEventExclusionSink() = default;
    virtual bool isTargetSideParticipant(
        characterId /*culprit*/, std::uint64_t /*eventId*/
    )
    {
        return false;
    }
    virtual void excludeFromRewards(
        characterId /*who*/, std::uint64_t /*eventId*/
    )
    {}
};
class NullEventExclusionSink final : public IEventExclusionSink
{};

// ---- events (5.3 owns contribution routing; null no-op until then) ----
class IEventsSink
{
public:
    virtual ~IEventsSink() = default;
    virtual void onCombatDamage(
        const struct DamageEvent & /*ev*/
    )
    {}
    virtual void onHeal(
        characterId /*healer*/, characterId /*target*/, std::uint64_t /*amount*/
    )
    {}
    virtual void onPriceChanged(
        regionId /*region*/, goodId /*good*/, std::uint64_t /*oldPrice*/, std::uint64_t /*newPrice*/
    )
    {}
};
class NullEventsSink final : public IEventsSink
{};

// ---- event world-effect reads (5.5; the PriceBias/WeatherBias consumers) ----
// The engine records the biases (5.1's interpreter); the consumers join with
// 5.5's wiring through this seam (D-SINK-1: null default = no bias — the
// economy prices at the supply-demand formula and the weather machine rolls
// freely).
class IEventEffectsSink
{
public:
    virtual ~IEventEffectsSink() = default;

    // The active event price bias for the region+good: a SCALE-grid price
    // multiplier (SCALE = no bias; a timed record past its until-timestamp
    // answers SCALE again).
    virtual std::uint64_t priceBiasFor(
        regionId /*region*/, goodId /*good*/, std::uint64_t /*now*/
    ) const
    {
        return SCALE;
    }

    // The active event weather bias key ("clear"/"rain"/"storm"/"fog"/
    // "snow"), or nullptr when no bias holds for the region at `now`.
    virtual const std::string *weatherBiasFor(
        regionId /*region*/, std::uint64_t /*now*/
    ) const
    {
        return nullptr;
    }
};
class NullEventEffectsSink final : public IEventEffectsSink
{};

// ---- suspicion price hook (4.5; A-93: SHUNNED -20% receipts; null = SCALE) ----
class IPriceModifierSink
{
public:
    virtual ~IPriceModifierSink() = default;
    virtual std::uint64_t sellReceiptModifier(
        characterId /*seller*/, territoryId /*territory*/
    ) const
    {
        return SCALE;
    }
};
class NullPriceModifierSink final : public IPriceModifierSink
{};

// ---- matchmaking seam (6.5; the SP difficulty dial) ----------------------------
//
// The doc's matchmaking score (2.5) drives the SP systems the multiplayer
// brackets would have driven (D-SP-1): duel opponent selection, manhunt
// difficulty normalization, event difficulty calibration. The sim-side
// consumers (the event engine) join through THIS interface — never through
// a progression/ include (D-SINK-1: null default = no calibration, the
// template's own difficulty stands; the command path is core-internal
// runtime wiring and may call the module directly).
class IMatchmakingSink
{
public:
    virtual ~IMatchmakingSink() = default;

    // Event difficulty calibration (6.5 consumer 3): the participants' mean
    // matchmaking score seeds the event's initialDifficulty, clamped to the
    // template's [0.5x, 2x] band. The participant ids ride the seam (the
    // sink reads the scores through its own bound context — the engine never
    // touches the power pipeline). Null default: the template stands.
    virtual std::uint32_t calibratedEventDifficulty(
        const std::vector<characterId> & /*participants*/, std::uint32_t templateDifficulty
    ) const
    {
        return templateDifficulty;
    }

    // Manhunt difficulty normalization (6.5 consumer 2): the spawned hunt's
    // difficulty normalizes toward the culprit (guards always join,
    // volunteers by score proximity). Null default: the template stands.
    virtual std::uint32_t normalizedManhuntDifficulty(
        characterId /*culprit*/, std::uint32_t templateDifficulty
    ) const
    {
        return templateDifficulty;
    }
};
class NullMatchmakingSink final : public IMatchmakingSink
{};

// ---- player signals (§4.2 rule 2: rejections surface through a PlayerSignal) ----
enum class SimCommandKind : std::uint8_t; // pinned list lives in core/command_apply.hpp

class IUiSignalSink
{
public:
    virtual ~IUiSignalSink() = default;
    virtual void onCommandRejected(
        SimCommandKind /*kind*/, const char * /*reason*/
    )
    {}
};
class NullUiSignalSink final : public IUiSignalSink
{};

// ---- player bags (2.4 owns the real home; used by 3.3 corpse loot collection) ----
class IBagSource
{
public:
    virtual ~IBagSource() = default;
    virtual items::Inventory *bagOf(
        characterId /*id*/
    )
    {
        return nullptr;
    }
};
class NullBagSource final : public IBagSource
{};

// ---- suspicion-signal seam (2.6; consumed by 4.5) -----------------------------
// The doc's "frequent [appearance] changes are themselves a suspicion trigger"
// fires THROUGH this interface, never by writing suspicion state. Null in the
// context = the signal is dropped deterministically (D-SINK-1); 4.5's store
// implements it when it lands (A-94 weight).
struct ISuspicionSink
{
    virtual ~ISuspicionSink() = default;

    // A-25 fired: `who` recorded APPEARANCE_FREQUENT_COUNT appearance changes
    // within APPEARANCE_FREQUENT_WINDOW_GAME_S game-seconds; `at` is the
    // worldTime of the change that crossed the threshold.
    virtual void onAppearanceFrequent(characterId who, std::uint64_t at) = 0;
};

// ---- AppearanceChanged publication seam (2.6; the 0.7 game event) -------------
// A change publishes the FACT after the mutation is complete (0.7's post-state
// rule). Consumers (witness perception 3.9, the UI) re-read the character or
// the AppearanceHistory. Null = the publication is dropped (the history still
// records, D-SINK-1).
struct IAppearanceSink
{
    virtual ~IAppearanceSink() = default;

    // The change is fully applied (components replaced, XP paid, cooldown
    // armed) when this fires.
    virtual void onAppearanceChanged(characterId who) = 0;
};

// ---- the bundle ----
//
// The task-2.1 pinned shape, extended in place by the Phase-2 cards (3.1–3.7):
// plain data, default-constructed to the "nothing landed yet" state (every
// pointer null, now = 0), which is exactly the honest state of a fresh
// WorldContext before the scheduler wiring runs. Everything is a raw pointer
// on purpose: the context NEVER owns what it points at (the stores and
// registry outlive every tick), a shallow copy stays cheap, and the D-SINK-1
// "null-false until the owner lands" readings stay expressible — a reference
// member could be neither null-checked nor default-constructed, which is what
// the Phase-2 integration initially rewrote this struct into (every 2.x
// pinned test and every pointer-style consumer broke; see
// world_context_aliases.hpp's MERGE note, now folded back here and into
// core/types.hpp).
struct WorldContext
{
    // -- clocks (0.3, D-TIME-1): step 0 refreshes `now`; realTime serves the three
    //    real-time rules; tickGameSeconds = TIME_SCALE / tickHz from [harrowmere] config.
    std::uint64_t now             = 0;
    std::uint64_t realTimeSeconds = 0;
    std::uint64_t tickGameSeconds = 6; // 10 Hz x TIME_SCALE 60 (D-TICK-1)

    // -- cross-store queries & sinks (null defaults until owners land) --
    IWorldQueries *world              = nullptr;
    IFactionQueries *factions         = nullptr;
    IWarRegistry *wars                = nullptr; // 6.2 — null: no war combatants
    JusticeSink *justice              = nullptr; // D-SINK-1 (3.3); isArrested rides it (2.1/4.3)
    IPerceptionSink *perception       = nullptr; // 3.9 — null: no witnesses perceived
    IEventsSink *events               = nullptr; // 5.3 — null: contributions dropped
    IPriceModifierSink *priceModifier = nullptr; // 4.5 — null: SCALE receipts
    IUiSignalSink *signals            = nullptr; // §4.2 rule 2 — null: rejections only logged
    IBagSource *bags                  = nullptr; // 2.4 — null: no bag lookups

    // -- stores (§4.5: all sim state lives in stores owned by harrowmere_core) --
    CharacterStore *characters                 = nullptr; // 2.0 — player-creation reads (2.3); combat mutations (3.x)
    NpcStore *npcs                             = nullptr; // 3.4 — the NpcState sidecar records
    EconomyStore *economy                      = nullptr; // 3.7 — regional stocks & prices
    CorpseStore *corpses                       = nullptr; // 3.3 — step-11 corpse placement
    RoleRegistry *roles                        = nullptr; // 3.6 — role assignment & duties
    const DataRegistry *data                   = nullptr; // 1.3 — species/classes/goods reads
    items::ItemInstanceRegistry *itemInstances = nullptr; // 2.4 — instance-id lookups (power/equip)

    // -- systems --
    Scheduler *scheduler     = nullptr; // enqueueForNextTick / scheduleAt (3.1)
    LodActiveSet *lod        = nullptr; // step-3 active set (3.5) read by step-4 work
    CombatQueue *combatQueue = nullptr; // step-9 attack intents resolved at step 10 (§4.6)
    SeedRng *rng             = nullptr; // 0.3 — named streams (D-LORE-1 draws "combat")

    // -- task-2.x seams kept verbatim (2.3/2.6 consumers, pointer style) --
    const std::map<std::string, std::uint64_t> *actionCounters = nullptr; // 3.4/3.10 — null: all-zero
    character::AppearanceHistory *appearanceHistory            = nullptr; // 2.6 — null: no A-25 tracking
    IAppearanceSink *appearance                                = nullptr; // 2.6 — null: publication dropped
    ISuspicionSink *suspicion                                  = nullptr; // 4.5 — null: signals dropped (D-SINK-1)

    // -- Phase-3 additions (appended last: the struct is aggregate-initialized
    //    positionally in tests/test_phase2_fixture.hpp — new members go here
    //    so the pinned init list keeps compiling) --
    world::WorldStore *worldStore
        = nullptr; // 1.4/4.x — the doc walks (D-JUR-1: lawsAt/socialClassOf/parents) + territory archives (4.2). Null:
                   // no world (a programmer error on the justice paths that need it).
    justice::JusticeStore *justiceStore
        = nullptr; // 4.x — evidence/warrant/reputation/jail state (§4.5's JusticeStore). Null: no justice state — the
                   // justice free functions debug-assert.
    ISimEventSink *simEvents                   = nullptr; // 4.x — 0.7 event publication (null: dropped, D-SINK-1).
    const player::PlayerActions *playerActions = nullptr; // 4.1 — the player's act surface (scorePhysical's
                                                          // BLOODSTAINED marker read). Null: the act reads false.

    // -- Phase-3 additions, tasks 4.5-4.7/5.1 (appended last: the struct is
    //    aggregate-initialized positionally in tests/test_phase2_fixture.hpp —
    //    new members go here so the pinned init list keeps compiling) --
    world::SpatialGrid *grid = nullptr; // 1.6 — the one spatial-query path; 5.1's SpawnTargets inserts the spawned ids.
                                        // Null: no grid (SpawnTargets asserts).
    IEventSpawner *eventSpawner
        = nullptr; // 5.1 — the real engine adapter once bound. Null: manhunt spawns refused (D-SINK-1).
    IEventExclusionSink *eventExclusion
        = nullptr; // 5.1 — the real engine adapter once bound. Null: no exclusions, no target reads (D-SINK-1).
    IEventEffectsSink *eventEffects
        = nullptr; // 5.5 — the real engine adapter once bound. Null: no price/weather biases (D-SINK-1).
    IMatchmakingSink *matchmaking = nullptr; // 6.5 — the real sink once bound. Null: no calibration/normalization (the
                                             // template difficulty stands, D-SINK-1).

    // -- Phase-4 additions, tasks 6.2/6.3 (appended last: the struct is
    //    aggregate-initialized positionally in tests/test_phase2_fixture.hpp —
    //    new members go here so the pinned init list keeps compiling) --
    factions::FactionStore *factionStore
        = nullptr; // 6.3 — the faction reads (the claim tolerance's court walk + the
                   // relation rows). Null: no kingdom tolerance applies (courtless ground).
    factions::WarStore *warStore
        = nullptr; // 6.2 — the war registry (isAtWarPair / contested-region reads; the IWarRegistry is its interface
                   // face). Null: no wars — isAtWar false, nobody a combatant, nothing contested (D-SINK-1).
    factions::ClaimStore *claims = nullptr; // 6.3 — the claim ledger (presence/abandonment + the destruction-protection
                                            // gate). Null: no claims (6.2's outcome rewiring skips the ledger read).
};

} // namespace harrowmere
