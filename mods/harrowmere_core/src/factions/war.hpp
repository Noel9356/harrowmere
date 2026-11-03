#ifndef HARROWMERE_CORE_FACTIONS_WAR_HPP
#define HARROWMERE_CORE_FACTIONS_WAR_HPP

// war.hpp — task 6.2: declared wars with casus belli, notice, treaty and
// outcome effects on regions and territorial control — NPC-driven by default,
// player-declarable against NPC factions (the doc's war paragraph, sentence
// by sentence testable: the mapping lives in src/factions/README.md).
//
// The doc structs are verbatim (the card's Spec block):
//   War        { id, attacker, defender, cb, declaredAt, noticeEndsAt,
//                endedAt, outcome, active }
//   CasusBelliKind { BorderDispute, ResourceConflict, Reputation, Massacre,
//                PowerRatio }
// CasusBelli carries the two origins the kinds name (the massacre's village /
// the resource-conflict region) — the card pins the kinds, not the record's
// fields; both are provenance reads, never rule inputs beyond validation.
//
// The pinned declaration rules (doc list, constants.hpp A-109..A-112):
//   * BorderDispute    — adjacent controlled territories (the anchors' region
//                        grid adjacency; control = a titled or owning member).
//   * ResourceConflict — both factions extract the same good from a contested
//                        region (3.7 data: the region's produced stock is the
//                        extraction; co-resident members of both factions are
//                        the extractors — DEVIATIONS.md carries the reading).
//   * Reputation       — the defender's leader reputation in the attacker's
//                        capital < WAR_REPUTATION_CB_FLOOR (-4000).
//   * Massacre         — 4.7's hook: a village-slaughter marker registered by
//                        the IWarRegistry seam for a territory the DECLARING
//                        side controls, naming the defender's faction.
//   * PowerRatio       — attacker aggregate member power >= WAR_POWER_RATIO
//                        (1.5x) of the defender's; zero-member factions never
//                        pass (R2's guard).
// declareWar sets noticeEndsAt = now + WAR_NOTICE_GAME_S; the defender is
// auto-notified (a PlayerSignal{War} when the defender side is the player's —
// D-WAR-SIGNAL-1; the NPC court reads the war record itself). During the
// notice IN_WAR (2.1) is NOT set and 3.3's war exemption does not apply —
// kills during notice are still crimes (the pinned test). From noticeEndsAt
// the ONE gate below flips both:
//
//   warEffectivelyActive(war, now)  — active && now >= noticeEndsAt
//
// (the card pitfall: implement one function, use it in IN_WAR AND the
// exemption, or they drift). D-SP-WAR-1: the defender's player-owned
// territory protections (6.3) are suspended from noticeEndsAt while the war
// is active — claims.cpp reads isRegionContestedActive through this store.
//
// Resolution (the doc's war-progress sentence): wars progress by battle
// events (the regional BATTLE template, both factions' NPCs joined as targets
// of each other via the D-WAR-EVT-1 dual-targetGroups extension) and by power
// attrition; the war ends when (a) one side's aggregate member power falls
// below WAR_COLLAPSE_RATIO (30%) of the other's, or (b) WAR_MAX_DURATION_DAYS
// (14 game-days) timeout — the attacker loses on timeout.
//
// Outcomes (doc: "Outcomes modify region flags and territorial control"):
// the winner sets the region war-outcome bit (territory_templates.json's
// regionFlags "war_torn" — flags are data-defined, the core stores the bits)
// on the contested regions; the losing side's PLAYER-OWNED territories in
// contested regions transfer to the winner's leading member (the claim
// rewiring rides 6.3's ClaimStore); NPC-ruled territories change titles (the
// loser's noble/royal title-holders are replaced by the winner's appointees —
// socialClassOf reads the new titles naturally, D-JUR-1's walk is untouched).
//
// Peace treaty (doc: "The same factions cannot immediately re-declare: a
// mandatory one-week peace treaty applies"): endWar stamps
// relations[a][b].treatyUntil = now + PEACE_TREATY_DAYS (both directions,
// the mirror-row rule); declareWar refuses while now < treatyUntil.
//
// Determinism (R7): the war book is std::map keyed by war id; every scan
// (contested regions, members, territories) walks id-ascending maps. The
// monthly NPC auto-declaration sweep is lazy-gated like 6.1's drift (the
// first tick of a new game-month applies the month once) and drives only
// rival pairs — wars arise from casus belli, never from a bare timer.

#include <cstdint>
#include <map>
#include <vector>

#include "core/result.hpp"        // Result<void, E> (0.2's carrier)
#include "core/scheduler.hpp"     // IStepSystem (the step-8 tick)
#include "core/types.hpp"         // factionId, characterId, territoryId, regionId
#include "core/world_context.hpp" // IWarRegistry (the interface face) + WorldContext
#include "world/region.hpp"       // kNoRegion (the cb's unnamed-region sentinel)

namespace harrowmere::factions
{

class FactionStore; // factions.hpp — the membership + power reads (pointer-only here)

enum class CasusBelliKind : std::uint8_t
{
    BorderDispute,
    ResourceConflict,
    Reputation,
    Massacre,
    PowerRatio
};

struct CasusBelli
{
    CasusBelliKind kind = CasusBelliKind::PowerRatio;
    regionId region
        = world::kNoRegion; // the contested region the cb names (ResourceConflict / the Massacre origin's region)
    territoryId origin = 0; // the territory-level origin (the Massacre's village; 0 = none)
};

enum class WarOutcome : std::uint8_t
{
    None, // the war still stands (or never resolved)
    AttackerWon,
    DefenderWon
};

struct War
{
    std::uint64_t id   = 0;
    factionId attacker = 0;
    factionId defender = 0;
    CasusBelli cb{};
    std::uint64_t declaredAt   = 0;
    std::uint64_t noticeEndsAt = 0;
    std::uint64_t endedAt      = 0;
    WarOutcome outcome         = WarOutcome::None;
    bool active                = false;
};

// The ONE flip instant (the card pitfall): IN_WAR (status.cpp reads it
// through IWarRegistry::isCombatant) and 3.3's isCriminalAct war exemption
// (FactionStore::isAtWar) both ask exactly this.
bool warEffectivelyActive(const War &war, std::uint64_t now) noexcept;

// The rule-level failures of declaration (§3.4: return values, never
// exceptions).
enum class WarError
{
    UnknownFaction,
    SameFaction,     // a faction holds no war against itself
    AlreadyAtWar,    // an active war (notice included) already stands between the pair
    TreatyUntil,     // the mandatory peace treaty blocks the re-declaration (both directions)
    NoCasusBelli,    // none of the five casus belli validated (incl. the PowerRatio ratio)
    NoFactionBinding // the store's faction binding is missing (programmer error, deterministic refusal)
};
using WarResult = Result<void, WarError>;

class WarStore final : public IWarRegistry
{
public:
    WarStore() = default;

    // The faction binding (membership + aggregate-power reads). The plugin
    // and the tests bind once, before any tick — declaration refuses
    // without it (NoFactionBinding: the PowerRatio read needs the members).
    void bindFactions(
        FactionStore *store
    )
    {
        factionStore_ = store;
    }

    // The context binding (the EventEngine::bindContext pattern): the
    // frozen-signature reads (isCombatant, isAtWarPair) answer against the
    // bound context's worldTime — the store holds no clock of its own (R8).
    // The first tick binds too (the plugin's context is the run's one
    // context); the context-free reads answer false until then.
    void bindContext(
        WorldContext *ctx
    )
    {
        ctx_ = ctx;
    }

    // ---- declaration ------------------------------------------------------------

    // declareWar — the doc's declaration rules, all five casus belli
    // validated against live data; the notice rides A-110; the defender is
    // auto-notified (PlayerSignal{War} when player-side, D-WAR-SIGNAL-1);
    // TerritoryChanged{War} publishes after the mutation (0.7 rule 1). The
    // cb is IN-OUT: the validators fill the provenance fields they read (the
    // ResourceConflict's region, the Massacre's origin) and the PowerRatio
    // fallback rewrites the kind — the caller reads back what resolved.
    WarResult declareWar(factionId attacker, factionId defender, CasusBelli &cb, WorldContext &ctx);

    // ---- resolution (the step-8 tick) --------------------------------------------

    // tick — the war progression: per active war (id-ascending), the notice
    // window's battle spawns (contested regions, at most one live BATTLE per
    // region — the engine's scale-uniqueness gate is the throttle), then the
    // resolution checks (collapse at A-111, timeout at WAR_MAX_DURATION_DAYS)
    // and the outcome application. Also the lazy monthly NPC auto-declaration
    // sweep (rival pairs, first valid cb wins — "NPC-driven by default").
    void tick(WorldContext &ctx);

    // endWar — the outcome application (the collapse/timeout paths and the
    // tests' direct resolution entry): region war-outcome bits, the claim
    // rewiring (6.3's rules), the title transfers, the peace treaty.
    void endWar(std::uint64_t warId, WarOutcome outcome, WorldContext &ctx);

    // ---- reads ---------------------------------------------------------------------

    const War *find(std::uint64_t id) const;
    const std::map<std::uint64_t, War> &all() const
    {
        return wars_;
    }

    // An active (declared, notice included) war between the pair, or nullptr.
    const War *activeWarBetween(factionId a, factionId b) const;

    // The at-war read (3.3's exemption + IN_WAR share it): effectively
    // active only — a notice-pending war is not yet war.
    bool isAtWarPair(factionId a, factionId b) const;

    // The contested regions of `war` (computed read, region-ascending, R7):
    // the cb's named region plus every region anchored to a territory either
    // belligerent controls. `active`-wars only callers pass warEffectivelyActive
    // themselves; the read is total (an ended war answers its regions still —
    // the outcome already applied).
    std::vector<regionId> contestedRegionsOf(const War &war, WorldContext &ctx) const;

    // Any DECLARED war (active == true, notice included) contesting `region`
    // — the claim gate's "no active war involving it" (6.3) and the
    // protection suspension's war-involvement reads. `requireNoticeEnded`
    // upgrades to the effectively-active read (the D-SP-WAR-1 suspension
    // starts at noticeEndsAt, not at declaration).
    bool isRegionContested(regionId region, bool requireNoticeEnded) const;

    // IWarRegistry — IN_WAR's derived-status face (status.cpp): a faction is
    // a combatant exactly while it belligerents an effectively-active war.
    bool isCombatant(factionId faction) const override;

    // 4.7's casus belli hook (the frozen seam signature): a village-slaughter
    // marker for `origin`'s defender-side claimants vs `culpritFaction`. The
    // marker persists until consumed by a Massacre declaration (no window is
    // pinned for the cb's validity; the marker IS the record).
    void registerCasusBelli(territoryId origin, characterId culprit, factionId culpritFaction) override;

    // ---- save seam (task 7.1) ---------------------------------------------------
    //
    // The war book as one plain-data snapshot: the wars (ended ones included
    // — active=false rows carry the outcome the treaty stamp came from), the
    // 4.7's massacre markers (the marker IS the record until a Massacre cb
    // consumes it), the id counter and the lazy month gate. MassacreMarker is
    // a private nested type, so the snapshot carries it unpacked.
    struct Snapshot
    {
        std::map<std::uint64_t, War> wars;
        std::map<territoryId, std::pair<factionId, std::uint64_t>> massacreMarkers;
        std::uint64_t nextWarId            = 1;
        std::uint64_t lastAutoDeclareMonth = 0;
        std::uint64_t nextProgressCheckAt  = 0; // the progression sweep's cadence stamp (the 8.3 as-built)
    };

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot &saved); // replaces the war book wholesale

private:
    // Aggregate member power of `faction` (living members, the cached power
    // read — cache freshness is the LOD tick's business, the 6.1 precedent).
    std::uint64_t aggregatePower(factionId faction, WorldContext &ctx) const;

    // The five validators (declareWar runs them in the pinned enum order).
    bool cbBorderDisputeValid(factionId attacker, factionId defender, WorldContext &ctx) const;
    bool cbResourceConflictValid(factionId attacker, factionId defender, CasusBelli &cb, WorldContext &ctx) const;
    bool cbReputationValid(factionId attacker, factionId defender, WorldContext &ctx) const;
    bool cbMassacreValid(factionId attacker, factionId defender, CasusBelli &cb) const;
    bool cbPowerRatioValid(factionId attacker, factionId defender, WorldContext &ctx) const;

    // A territory is under `faction`'s control: a member holds a title on it,
    // or its D-TERR-OWNER-1 owner is a member.
    bool controlsTerritory(territoryId t, factionId faction, WorldContext &ctx) const;

    // The faction's leader (rank 9 first, then the highest rank; ties by
    // lower id — the 6.1 ladder). 0 = memberless.
    characterId leadingMember(factionId faction) const;

    // The monthly NPC auto-declaration sweep (lazy month gate, R7 pair scan).
    void monthlyAutoDeclare(WorldContext &ctx);

    std::map<std::uint64_t, War> wars_; // R7: war-id ascending; ended wars stay (active=false)
    std::uint64_t nextWarId_ = 1;       // R10: ids are opaque, never reused

    // 4.7's markers: territory -> (culprit faction, worldTime). R7 order.
    struct MassacreMarker
    {
        factionId culpritFaction = 0;
        std::uint64_t at         = 0;
    };
    std::map<territoryId, MassacreMarker> massacreMarkers_;

    FactionStore *factionStore_         = nullptr; // the binding (power/membership reads)
    WorldContext *ctx_                  = nullptr; // the binding (the frozen-signature reads' clock)
    std::uint64_t lastAutoDeclareMonth_ = 0;       // the lazy month gate (0 = never swept)
    std::uint64_t nextProgressCheckAt_  = 0;       // the progression sweep's cadence gate (0 = due)
};

// Step 8 (Macro): the war progression (§4.4's macro step — the kingdom-level
// politics the per-character steps never see; the economy owns step 7, the
// war tick lands here).
class WarTickSystem final : public IStepSystem
{
public:
    WarStore *store = nullptr;

    void run(
        WorldContext &ctx
    ) override
    {
        if (store != nullptr)
        {
            store->tick(ctx);
        }
    }
};

} // namespace harrowmere::factions

#endif // HARROWMERE_CORE_FACTIONS_WAR_HPP
