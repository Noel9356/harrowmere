#ifndef HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP
#define HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP

// The published capability surface of the harrowmere_core native mod - the
// ONLY header harrowmere_ui (and any third-party native mod) compiles against.
// The provider owns its vocabulary, engine-style: like every published
// capability header this stays implementation-, platform- and vendor-agnostic
// (plain data + virtuals; the purity guard in the mod's CMakeLists enforces it
// mechanically). See mods/README.md, section "Portability".
//
// v0.1 (task 0.1): the surface below is the CONTRACT (common.md §4.2). The
// views began as inert stubs - and that is deliberate: the header's shape is
// what later phases plug into. The Phase-1 runtime bootstrap (the exit
// checklist's debug-overlay requirement) filled the Phase-1 slice of
// WorldView/PlayerView with real snapshot data and wired the command queue
// into the fixed-update lane; Phase 2 fills the rest (rebuilt at tick step 13
// each frame) and owns the full command surface. The surface's names and
// shapes only ever extend. The write-path VOCABULARY (the SimCommand intent
// envelope) landed with task 3.10 - see the write-path block below.
//
// Phase 2 (tasks 7.2-7.4): the full command set is PINNED here (the phase
// file's "You leave these seams" #4 - 7.3's list, verbatim), the views grow
// their Phase-2 fields (justice/event/signal views filled for real, §4.2),
// and the published stat/skill name tables give the panels closed labels
// without a core-internal include (§4.2 rule 5). Every change is additive:
// Phase-1 field names, orders and defaults are untouched, so the Phase-1
// debug panel and the P1 tests keep compiling unchanged.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace harrowmere
{

// ---- read views (Phase-1 slice; Phase 2 fills the rest) ----------------------
//
// Views are SNAPSHOTS, not live references: rebuilt once per frame (tick step
// 13), read by a UI frame between ticks. No view ever exposes a store
// iterator or a raw Character pointer - ids and copies only (R10, §4.5).
// Fog-of-war is structural: views expose exactly what a player can observe
// (justice: reputation, warrants against you, signals - never caseScore or
// hidden evidence; §4.6); TestUi_NoHiddenEvidenceExposed (7.3) pins the
// whitelist.
//
// Phase 1 fills the fields the phase's own exit checklist names (the F1 debug
// overlay: region, territory, law set, safe-zone flag, season, weather, tick
// number): WorldView carries the world-underfoot summary and the tick, and
// PlayerView carries the player transform + the two clocks. The Phase-2-only
// surfaces (power, justice, events) exist as fields with zero/empty values so
// the published shape does not move when 2.x/3.x fill them.

// The published view vocabulary lives in its own namespace: the capability
// surface's snapshot types must never collide with the sim-internal
// core/... names (core/types.hpp's harrowmere::Position is the doc-pinned
// sim-side struct; views::Position is the wire copy the UI reads).
namespace views
{

struct Position
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Published vocabulary for the fields below. These are the capability
// surface's own enums (plain data, no includes): they mirror the sim-internal
// closed lists (core/enums.hpp, seasons.hpp) by value - the provider maps
// between them when it rebuilds the views, and the ordinals are pinned in the
// sim so the mapping is a cast-checked switch, never a re-derivation.
enum class SeasonId : std::uint32_t
{
    Spring,
    Summer,
    Autumn,
    Winter
};

enum class WeatherKind : std::uint32_t
{
    Clear,
    Rain,
    Storm,
    Fog,
    Snow
};

// The legal hierarchy type (sim-internal core/enums.hpp has the same closed
// list with the docs' SCREAMING spellings; the published surface uses the
// doc's display names).
enum class TerritoryType : std::uint32_t
{
    Village,
    City,
    Kingdom,
    Empire
};

// The LOD simulation tier (sim-internal npc/state.hpp's LodTier; published as
// a plain u8 ordinal in CharacterSummary below - 0 = NEAR, 1 = MID, 2 = FAR).
enum class LodTierId : std::uint32_t
{
    Near = 0,
    Mid  = 1,
    Far  = 2
};

// The 4.5 trust ladder (sim-internal core/enums.hpp TrustTier; published as a
// plain u8 ordinal in CharacterSummary below - 0 = STRANGER .. 3 = SHUNNED).
// Phase 3 fills it; v1 reads zero (STRANGER) everywhere.
enum class TrustTierId : std::uint32_t
{
    Stranger   = 0,
    Watched    = 1,
    Distrusted = 2,
    Shunned    = 3
};

// ---- published closed-list name tables --------------------------------------
//
// The sim's stat/skill enums are closed lists (core/enums.hpp, task 1.1); the
// panels need their display names and the capability header cannot include
// core internals (§4.2 rule 5). The tables below publish the names with the
// ordinals mirroring core/enums.hpp's declaration order; a static_assert in
// the core's command_apply.cpp pins the two against each other so a silent
// drift is a compile error, not a mislabeled button.

inline constexpr std::uint32_t kStatCount  = 6; // StatType::COUNT (core/enums.hpp)
inline constexpr std::uint32_t kSkillCount = 8; // SkillType::COUNT (core/enums.hpp)

inline constexpr std::array<const char *, kStatCount> kStatNames{
    "STRENGTH", "DEFENCE", "AGILITY", "INTELLIGENCE", "ENDURANCE", "PERCEPTION"
};

inline constexpr std::array<const char *, kSkillCount> kSkillNames{
    "MELEE", "ARCHERY", "STEALTH", "ARCANA", "SURVIVAL", "PERSUASION", "CRAFTING", "ESCAPE"
};

// The sim-internal CooldownType closed list, published as ordinals (the
// CharacterView/PlayerView cooldown arrays index by these; core/enums.hpp's
// declaration order: APPEARANCE, RESPEC_STAT, RESPEC_SKILL).
inline constexpr std::uint32_t kCooldownCount = 3;

// The region underfoot (the player's region cell).
struct RegionSummary
{
    std::uint32_t id           = 0; // regionId (u16 space, u32 on the wire)
    std::uint32_t manaRichness = 0; // 0..SCALE (D-LORE-3 band 9500..10000 at worldgen)
};

// One node of the legal hierarchy (WorldView carries the full tree for the
// 7.3 Map panel; the territory underfoot is currentTerritory).
struct TerritorySummary
{
    std::uint64_t id     = 0; // territoryId; 0 = none (unclaimed frontier)
    std::uint64_t parent = 0; // the parent territory id (0 = root level)
    TerritoryType type   = TerritoryType::Village;
    std::string lawSetKey;    // the effective law set's key (lawsAt walk result)
    bool safe        = false; // safe-zone flag
    bool lawless     = false; // 4.6: garrison-less unconquered territory
    bool claimedByMe = false; // 7.3b's Map facet: one of MY claims (6.3 fills; false until then)
};

struct SeasonSummary
{
    SeasonId season           = SeasonId::Spring;
    std::uint64_t dayOfSeason = 0; // 0-based day within the current season (A-51 length 30)
};

struct WeatherSummary
{
    WeatherKind kind = WeatherKind::Clear;
};

struct WorldView
{
    RegionSummary currentRegion{};
    TerritorySummary currentTerritory{};
    SeasonSummary season{};
    WeatherSummary weather{};
    std::uint64_t tickNumber = 0; // completed sim ticks since session start

    // Phase 2 (7.3 Map panel): the whole territory tree in id-ascending order
    // (R7; the panel renders the parent/child nesting from `parent`). Wars
    // (6.2) and claims (6.3) overlay later - the shape carries them through
    // TerritorySummary's facets without a breaking change.
    std::vector<TerritorySummary> territories;
};

// One item row as the player's bag/equipment presents it (2.4's data through
// the 0.4 loaders; keys are items.json keys, names are the rows' display
// names). `consumable`/`equippable` mirror the row's blocks so the Inventory
// panel's buttons enable themselves from the view alone.
struct BagEntry
{
    std::string itemKey;     // items.json key (Consume/Craft intents name keys)
    std::string name;        // display name
    std::uint32_t count = 0; // stack count (fungible side)
    bool consumable     = false;
    bool equippable     = false;
};

// One equipped item INSTANCE (2.4: equippedItems stores instance ids, not
// stack keys - two iron swords score twice).
struct EquippedEntry
{
    std::uint64_t itemInstanceId = 0;
    std::string itemKey;
    std::string name;
};

// One recipe row as the Inventory panel's craft list presents it (2.4's
// recipes.json; `craftableNow` is the core's own gate verdict - inputs in the
// bag AND the CRAFTING skill requirement met - so the panel never re-derives
// a game rule).
struct RecipeInputEntry
{
    std::string itemKey;
    std::uint32_t count = 0;
    std::uint32_t inBag = 0; // what the player currently holds (button sub-label)
};

struct RecipeEntry
{
    std::string recipeKey; // recipes.json key (Craft intents name keys)
    std::string outputKey;
    std::string outputName;
    std::vector<RecipeInputEntry> inputs;
    std::string reqSkillName; // "" = none (v1 recipes carry at most one)
    std::uint32_t reqSkillValue = 0;
    bool craftableNow           = false;
};

struct PlayerView
{
    Position position{};               // the sim-side player transform (3.10)
    std::uint64_t worldTime   = 0;     // game seconds since world epoch (R8)
    std::uint64_t realTime    = 0;     // real seconds since session start (D-TIME-1)
    std::uint64_t cachedPower = 0;     // Phase 2 fills (2.5); zero in Phase 1
    std::uint64_t peakPower   = 0;     // Phase 2 fills; zero in Phase 1
    std::uint32_t level       = 0;     // Phase 2 fills; zero in Phase 1
    std::int64_t availableXp  = 0;     // Phase 2 fills; zero in Phase 1
    bool afk                  = false; // D-AFK-1 (3.10)

    // ---- Phase 2 (7.3 panels): the local player's full observable state ----
    std::string name;                                            // the creation record's name (2.0)
    std::uint64_t currentHp     = 0;                             // Body.currentHp
    std::uint64_t maxHp         = 0;                             // Body.maxHp
    std::int64_t currency       = 0;                             // Progression.currency (A-41 purse)
    std::uint64_t totalXp       = 0;                             // lifetime XP (never decreases)
    std::uint64_t nextLevelCost = 0;                             // levelCosts[level] (A-7/A-8; 0 at the cap)
    std::array<std::uint64_t, kStatCount> stats{};               // raw points
    std::array<std::uint64_t, kSkillCount> skills{};             // raw points
    std::array<std::uint64_t, kStatCount> statEffectiveness{};   // getStatEffectiveness (2.2)
    std::array<std::uint64_t, kSkillCount> skillEffectiveness{}; // getSkillEffectiveness (2.3)
    std::uint64_t statCap  = 0;                                  // statCaps[level] (A-13/A-14)
    std::uint64_t skillCap = 0;                                  // skillCaps[level] (A-15/A-16)
    // Cooldown expiry worldTimes by kCooldownCount ordinals (APPEARANCE,
    // RESPEC_STAT, RESPEC_SKILL - 2.1's slots); 0 = expired. The panels
    // compare against `worldTime` for the countdown display.
    std::array<std::uint64_t, kCooldownCount> cooldowns{};
    std::uint64_t appearanceCostNext = 0; // A-22/A-23: 50 + 10*level, paid from availableXp
    // Derived statuses (D-CHAR-1: computed core-side, never stored; the HUD's
    // status icons read these).
    bool inCombat              = false; // A-4 window from lastCombatTime
    bool inEvent               = false; // eventId != 0
    bool inWar                 = false; // 6.2 fills; false until then
    bool arrested              = false; // 4.3's jailUntil map
    bool inDuel                = false; // duelOpponent != 0 (2.7)
    std::uint64_t duelOpponent = 0;     // the opponent's characterId (0 = none)
    std::uint64_t arenaId      = 0;     // shared-id arena consent (0 = none)

    // The player's bag + equipment + known recipes (2.4; the Inventory panel).
    // The bag is the fungible side collapsed per item key; equipped carries
    // the instances; recipes carry the craft gate verdicts.
    std::vector<BagEntry> bag;
    std::vector<EquippedEntry> equipped;
    std::vector<RecipeEntry> recipes;
};

// Fog-of-war whitelist (§4.2 rule 3): reputation/notoriety/warrants-against-
// you/jail only. Phase 1 has no justice activity - the values stay zeroed.
//
// Phase 2 (7.3 Justice panel) extends the SAME whitelist structurally: the
// per-territory reputation walk (displayed flattened) and the warrants
// against the player (the accused-side view: territory, bounty, expiry,
// lethality - exactly what 4.6's signals + poster text may say). NO case
// score, NO evidence list, NO report weights: the fields below are the
// complete whitelist and TestUi_NoHiddenEvidenceExposed pins that shape.
struct TerritoryReputation
{
    std::uint64_t territoryId = 0;
    std::int64_t reputation   = 0; // ±A-87 MAX_REPUTATION
};

struct WarrantAgainstMe
{
    std::uint64_t territoryId      = 0;     // the issuing territory (jurisdiction root)
    std::uint64_t bounty           = 0;     // currency (A-73 cap)
    std::uint64_t expiresWorldTime = 0;     // A-74 duration
    bool lethal                    = false; // copied from crime data at issuance
};

struct JusticeView
{
    std::int64_t reputation                = 0; // the CURRENT territory's reputation
    std::uint64_t notoriety                = 0; // the global record
    std::uint32_t activeWarrantsAgainstYou = 0; // live warrant count (purge-on-read applied)
    bool isJailed                          = false;
    std::uint64_t jailUntil                = 0; // sentence end worldTime (4.3)

    // Phase 2 (7.3): the same whitelist, flattened for the panel.
    std::vector<TerritoryReputation> perTerritory; // the walk's origin first, root last (4.4)
    std::vector<WarrantAgainstMe> warrants;        // live warrants against the player
};

// One UI-visible signal (the capability-side twin of game_events.hpp's
// PlayerSignal event - §4.2 rule 2: rejections surface back through these).
enum class PlayerSignalKind : std::uint32_t
{
    Investigated,
    Wanted,
    Poster,
    Greeting,
    War // 6.2 (D-WAR-SIGNAL-1): the war-notice notification — the same
        // deviation row as core/game_events.hpp's PlayerSignal::Kind.
};

struct PlayerSignalEntry
{
    PlayerSignalKind kind = PlayerSignalKind::Investigated;
    std::string text;
};

struct UiSignals
{
    std::vector<PlayerSignalEntry> entries; // Phase 3 fills; empty in Phase 1
};

// One active event instance as the player may see it (Phase 1: none).
struct EventSummary
{
    std::uint64_t eventId = 0;
    std::string templateKey;
    std::uint64_t stateId = 0;

    // Phase 2 (7.3 Events panel): participation + contribution as visible.
    bool joinable                 = false; // the template's join policy admits the player
    bool amParticipant            = false; // the player is in `participants`
    std::uint64_t myContribution  = 0;     // getContribution raw total (5.3)
    std::uint32_t mySharePermille = 0;     // estimated reward share, 0..1000 (getRewardShare)
    std::uint32_t difficulty      = 0;     // the current state's difficulty (display only)
};

struct EventsView
{
    std::vector<EventSummary> active; // Phase 2 fills (5.x); empty in Phase 1
};

// One present character as a UI frame may observe it (§4.2: "presence,
// species, role, power, status, needs (as visible)"). "Present" = within the
// presentation radius (A-118, 300 m) of the player - the same set the 7.2
// bridge renders, so the World inspector's list and the world's markers
// agree. Ordinals: tier (LodTierId), trust (TrustTierId).
struct CharacterSummary
{
    std::uint64_t id = 0;        // characterId (R10)
    std::string name;            // NpcState.name; the player's row carries the creation name
    std::uint64_t speciesId = 0; // speciesId (u16 space, u64 on the wire)
    std::string speciesKey;      // species.json key ("human", "elf", ...)
    std::string roleKey;         // roles.json key ("" = none)
    std::uint32_t level       = 0;
    std::uint64_t cachedPower = 0;
    Position position{};
    std::uint64_t currentHp   = 0;
    std::uint64_t maxHp       = 0;
    std::uint32_t ageYears    = 0; // 3.4's A-38 read
    LodTierId tier            = LodTierId::Far;
    TrustTierId trustTowardMe = TrustTierId::Stranger; // 4.5 fills; v1 STRANGER
    std::array<std::uint64_t, 4> needs{};              // hunger, rest, wealth, social (0..SCALE)
    bool sapient  = false;
    bool isPlayer = false;
    // Derived statuses (D-CHAR-1), core-side computed.
    bool inCombat = false;
    bool inEvent  = false;
    bool arrested = false;
};

// presence, species, role, power, status, needs-as-visible - Phase 2 fills
// (2.x); the published shape exists now so consumers compile against it.
struct CharactersView
{
    std::vector<CharacterSummary> present; // A-118 window, id-ascending (R7)
};

} // namespace views

// ---- write path ---------------------------------------------------------------
//
// Commands are the ONLY write path from outside harrowmere_core: submit
// queues, tick step 9 applies (or rejects with a logged reason that surfaces
// as a PlayerSignal).
//
// v0.1 (task 0.1) shipped an empty command envelope. Task 3.10 (player
// control & interaction intents) pins the intent vocabulary the input layer
// produces (card 3.10 Spec: "Input layer (7.4, UI mod) produces intents
// (Move{dir}, Attack{target}, Interact{target, action}, Emote{act})") - it
// lives HERE because this header is the only vocabulary the input mod may
// compile against (DEVIATIONS.md carries the row; the shapes stay plain
// data, no new includes, purity guard untouched).
//
// Phase 2 (tasks 7.3/7.4) pins the FULL command set - the phase file's "You
// leave these seams" #4 and card 7.3's list, verbatim and in order:
//
//   Move, Attack, Interact, Buy, Sell, Craft, Equip, Unequip, Consume,
//   BuyStat, BuySkill, BuyLevel, RespecStat, RespecSkill, ChangeAppearance,
//   RequestDuel, YieldDuel, JoinEvent, LeaveEvent, ReportCrime, Bribe,
//   Surrender, ServeSentence, AttemptEscape, ClaimTerritory, InstallLaws,
//   FoundFaction, DissolveFaction, SetRelation, DeclareWar, Donate
//
// with task 3.10's pinned Emote intent slotted after Interact (the Phase-1
// published vocabulary "only ever extends"; the P1 tests submit Emote and
// the step-9 drain routes it - the card's list is a subsequence of the
// Kind enum, 32 values total). Justice/event/faction commands
// (ReportCrime .. Donate) no-op at step 9 with a logged rejection (surfaced
// as a PlayerSignal) until their systems exist - Phase 3/Phase 4 owners swap
// the rejections for real work without touching this header again.
//
// The envelope carries every variant side by side (no std::variant): `kind`
// names the active one. That keeps the header POD-simple and lets later
// phases extend the set the card's own way - "consent-gated intents extend
// the intent set (2.7) - the queue must take new variants without breaking
// the contract": a new Kind value plus a new payload field is source-
// compatible for producers and consumers alike.

// Move{dir} - the polled motion state. The input layer re-submits its
// camera-relative direction every frame (card 7.4P1), so a 10 Hz fixed
// update receives bursts; the sim consumes a Move as MOTION STATE (the
// latest intent in the slice wins, one kinematic integration per fixed
// update - frame-rate independent; see controller.hpp). dirX/dirZ are a
// world-space direction (the input layer owns camera-relative mapping);
// the controller normalizes. sprint: A-57's "sprint x2" - free in Phase 1,
// priced with the rest need when 2.1's player mirror lands.
struct MoveIntent
{
    double dirX = 0.0;
    double dirZ = 0.0;
    bool sprint = false;
};

// Attack{target} - a combat action against the named character (0 = none).
// Step 9 consumes the intent; the application is step 10's applyDamage
// (common.md §4.6 worked trace) - the consumer lands with 3.2/3.3 (Phase 2);
// Phase 1 drains the intent and leaves the state untouched.
struct AttackIntent
{
    std::uint64_t target = 0; // characterId space (R10); 0 = none
};

// Interact{target, action} - a context interaction with a nearby character.
// The card pins the action list; COUNT exists because the handler registry
// is array-indexed by it (R9). The per-action owners: talk -> trade 3.7 /
// title dialog 6.3, loot -> corpses 3.3, report_crime + bribe -> 4.1,
// surrender -> 4.3, pick_up/drop -> inventory 2.4. The range gate and the
// handler registry are task 3.10's (src/player/interactions.hpp).
struct InteractIntent
{
    enum class Action : std::uint32_t
    {
        TALK,
        LOOT,
        REPORT_CRIME,
        BRIBE,
        SURRENDER,
        PICK_UP,
        DROP,
        COUNT
    };

    std::uint64_t target = 0; // characterId space (R10); 0 = none
    Action action        = Action::TALK;
};

// Emote{act} - a self-state toggle the world can observe (card 3.10:
// crouch toggle, light_torch - UNLIT's counter-act, mask toggle - the
// MASKED act; the item-ownership check activates with 2.4's inventory, the
// Phase-1 toggle is free but the act is logged). The suspicion acts these
// expose are core/enums.hpp's SuspicionAct; the mapping lives on the
// player-side action log (player::PlayerActions::exposes).
struct EmoteIntent
{
    enum class Act : std::uint32_t
    {
        CROUCH,
        LIGHT_TORCH,
        MASK
    };

    Act act = Act::CROUCH;
};

// ---- Phase-2 command payloads (plain data; one per new Kind) ----------------

// Buy/Sell{good, qty} - the regional trade (3.7): quantity of a goods.json
// good at the player's current region's live price. u32 carries the goodId
// space (the economy's goods table is 8 v1 rows).
struct TradeIntent
{
    std::uint32_t good = 0; // goods.json id (R10: opaque in the good space)
    std::uint32_t qty  = 1;
};

// Craft{recipeKey} - recipes.json key (2.4; the Inventory panel's craft list).
// Equip/Unequip{itemInstanceId} - the instance id space (2.4's registry).
// Consume{itemKey} - items.json key.
// ChangeAppearance{components, componentCount} - the new look's component ids
// (2.6; v1 ships preset looks, so the list is short and fixed-capacity - the
// envelope stays copy-cheap for the per-frame Move traffic).

struct CraftIntent
{
    std::string recipeKey;
};

struct ItemInstanceIntent
{
    std::uint64_t itemInstanceId = 0;
};

struct ConsumeIntent
{
    std::string itemKey;
};

inline constexpr std::uint32_t kMaxAppearanceComponents = 8;

struct ChangeAppearanceIntent
{
    std::array<std::uint64_t, kMaxAppearanceComponents> components{};
    std::uint32_t componentCount = 0;
};

// BuyStat{stat}/BuySkill{skill} - the closed enum ordinals (kStatCount /
// kSkillCount spaces; core/enums.hpp order, mirrored by kStatNames/kSkillNames).
struct StatIntent
{
    std::uint32_t stat = 0;
};

struct SkillIntent
{
    std::uint32_t skill = 0;
};

// RequestDuel{target} / YieldDuel{} - 2.7's consent channels (the acceptance
// side is the NPC's 6.5 policy, not a command).
struct DuelIntent
{
    std::uint64_t target = 0;
};

// JoinEvent{eventId} / LeaveEvent{eventId} - 5.x's event engine.
struct EventIntent
{
    std::uint64_t eventId = 0;
};

// ReportCrime{target} / Bribe{target} - 4.1's report/bribery paths (the
// targeted form; the keyboard's G/B bindings ride the Interact intents,
// these are the panel-path equivalents).
struct TargetIntent
{
    std::uint64_t target = 0;
};

// ServeSentence{} / AttemptEscape{} - 4.3 (D-JAIL-1: the fast-forward runs
// the pinned pipeline until jailUntil; escape is the skill check).

// ClaimTerritory{territory} / InstallLaws{territory, lawSetKey} - 6.3.
struct ClaimIntent
{
    std::uint64_t territory = 0;
    std::string lawSetKey; // InstallLaws only
};

// FoundFaction{name} / DissolveFaction{} - 6.1.
struct FoundFactionIntent
{
    std::string name;
};

// SetRelation{faction, relation} / DeclareWar{faction} - 6.1/6.2 (relation on
// the SCALE grid: -SCALE..SCALE).
struct FactionIntent
{
    std::uint32_t faction = 0;
    std::int32_t relation = 0;
};

// Donate{target, amount} - 4.5's donation relief (-1000 score, A-92).
struct DonateIntent
{
    std::uint64_t target = 0;
    std::int64_t amount  = 0;
};

struct SimCommand
{
    // The pinned set: card 7.3's list with 3.10's Emote slotted after
    // Interact (see the block comment above). Extending it requires a
    // deviation row (§1.4: the pinned enum lists are closed).
    enum class Kind : std::uint32_t
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

    Kind kind = Kind::Move;
    MoveIntent move{};                   // kind == Move
    AttackIntent attack{};               // kind == Attack
    InteractIntent interact{};           // kind == Interact
    EmoteIntent emote{};                 // kind == Emote
    TradeIntent trade{};                 // kind == Buy / Sell
    CraftIntent craft{};                 // kind == Craft
    ItemInstanceIntent itemInstance{};   // kind == Equip / Unequip
    ConsumeIntent consume{};             // kind == Consume
    ChangeAppearanceIntent appearance{}; // kind == ChangeAppearance
    StatIntent stat{};                   // kind == BuyStat
    SkillIntent skill{};                 // kind == BuySkill
    DuelIntent duel{};                   // kind == RequestDuel
    EventIntent event{};                 // kind == JoinEvent / LeaveEvent
    TargetIntent target{};               // kind == ReportCrime / Bribe
    ClaimIntent claim{};                 // kind == ClaimTerritory / InstallLaws
    FoundFactionIntent foundFaction{};   // kind == FoundFaction
    FactionIntent faction{};             // kind == SetRelation / DeclareWar
    DonateIntent donate{};               // kind == Donate
};

// ---- presentation pacing (7.3's Time-controls panel) ------------------------
//
// pause / 1x / 8x / 60x - "scales accumulator only - worldTime mapping
// unchanged" (card 7.3). This is PACING state, not simulation state: it
// changes how many runTick() calls each engine fixed-update dispatch consumes
// (pause consumes none) and nothing else. Every game rule, the tick math and
// the worldTime-per-tick mapping are untouched (D-TICK-1 holds: at 1x one
// dispatch is exactly one runTick); headless tests drive runTick directly and
// never read this; determinism's contract (same tick count + seed + commands
// = same state) is unaffected because pacing only selects HOW MANY ticks
// elapse in real time, which the player controls.
//
// Decision note for the lead (DEVIATIONS.md candidate): the card pins no
// transport for the time controls and the pinned SimCommand list has no time
// command, so this is a separate control surface on the capability rather
// than a 32nd command (extending the pinned command list needs §1.4's row;
// this surface does not). Rule 2's "submit is the only write path" governs
// SIM state - a rejection or acceptance of a game action - and the pacing
// surface writes no sim state; the comment block exists so the review sees
// the reasoning where the code is.
namespace views
{

struct TimeControls
{
    bool paused                    = false;
    std::uint32_t ticksPerDispatch = 1; // 1 / 8 / 60 at runtime; other values clamp to [1, 60]
};

} // namespace views

// The pinned speed stops the Time-controls panel offers.
inline constexpr std::uint32_t kTimeScaleMax = 60;

// Capability "harrowmere": the game's authoritative simulation. Consumers
// acquire through the family name; the descriptor declares the
// implementation-bound member ("harrowmere.core") exactly as an engine module
// does. Mods consume it through the family ("harrowmere"); the app acquires
// it as a hard requirement (a run without its sim is not Harrowmere).
class SimCapability
{
public:
    // The family name a mod acquires through: the manifest requirement
    // spelling, and the grant the kernel derives from it
    // ("capability.harrowmere"). The kernel answers a family request with the
    // single active member's pointer (harrowmere.core) - engine_kernel.cpp,
    // resolveCapabilityPointer: "exact declarations first, then - for a
    // family request - the single active member".
    static constexpr const char *capabilityFamily = "harrowmere";
    // The concrete member the core mod's descriptor declares. The app (the
    // trusted composition root) may acquire it directly; a mod that demands
    // only the family has no grant for the member name and must not.
    static constexpr const char *capabilityName = "harrowmere.core";
    virtual ~SimCapability()                    = default;

    // ---- reads: stable, cheap view snapshots (rebuilt at step 13 each frame) ----
    virtual const views::WorldView &world() const; // regions, territories, laws-at, safe zones, season/weather
    virtual const views::CharactersView &
    characters() const;                                // presence, species, role, power, status, needs (as visible)
    virtual const views::PlayerView &player() const;   // the local player's full state
    virtual const views::JusticeView &justice() const; // reputation, notoriety, warrants against you, jail status
    virtual const views::EventsView &events() const;   // joinable/active events, own contribution
    virtual const views::UiSignals &signals() const; // PlayerSignals queue (investigated / wanted / poster / greeting)

    // ---- writes: commands, never direct state mutation ----
    // Queued; applied at step 9 next tick (§4.4) or rejected with a logged
    // reason that surfaces back through a PlayerSignal.
    virtual void submit(SimCommand cmd);

    // ---- presentation pacing (see TimeControls above) ----
    virtual views::TimeControls timeControls() const;
    virtual void setTimeControls(const views::TimeControls &controls);

private:
    // v0.1 stub storage: one inert snapshot per view. Phase 2 replaces these
    // with rebuilt-per-frame view data; the accessor signatures do not move.
    views::WorldView world_{};
    views::CharactersView characters_{};
    views::PlayerView player_{};
    views::JusticeView justice_{};
    views::EventsView events_{};
    views::UiSignals signals_{};
    views::TimeControls timeControls_{};
};

// ---- inline definitions (v0.1 inert stubs) ----------------------------------

inline const views::WorldView &SimCapability::world() const
{
    return world_;
}

inline const views::CharactersView &SimCapability::characters() const
{
    return characters_;
}

inline const views::PlayerView &SimCapability::player() const
{
    return player_;
}

inline const views::JusticeView &SimCapability::justice() const
{
    return justice_;
}

inline const views::EventsView &SimCapability::events() const
{
    return events_;
}

inline const views::UiSignals &SimCapability::signals() const
{
    return signals_;
}

inline void SimCapability::submit(
    SimCommand
)
{
    // v0.1: no command vocabulary exists yet; nothing to queue. Phase 2 fills
    // the queue here and tick step 9 drains it (§4.4).
}

inline views::TimeControls SimCapability::timeControls() const
{
    return timeControls_;
}

inline void SimCapability::setTimeControls(
    const views::TimeControls &controls
)
{
    // Clamp to the pinned band so a stray panel value can never ask the
    // fixed-update handler for a negative or absurd burst.
    views::TimeControls clamped = controls;
    if (clamped.ticksPerDispatch < 1u)
    {
        clamped.ticksPerDispatch = 1u;
    }
    if (clamped.ticksPerDispatch > kTimeScaleMax)
    {
        clamped.ticksPerDispatch = kTimeScaleMax;
    }
    timeControls_ = clamped;
}

} // namespace harrowmere

#endif // HARROWMERE_CORE_BRIDGE_HARROWMERE_CAPABILITY_HPP
