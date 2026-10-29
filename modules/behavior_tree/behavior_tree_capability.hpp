#ifndef ENGINE_MODULES_BEHAVIOR_TREE_BEHAVIOR_TREE_CAPABILITY_HPP
#define ENGINE_MODULES_BEHAVIOR_TREE_BEHAVIOR_TREE_CAPABILITY_HPP

// The published capability surface of the behavior_tree module. Every consumer -
// other modules, native mods, the app - includes THIS header, from the
// module that provides the capability: the provider owns and publishes its
// own capability vocabulary, so there is no central contract directory and
// no mirrored copy that could drift. Like every published capability header
// this stays implementation-, platform- and vendor-agnostic; the per-module
// purity guard in the module's CMakeLists enforces that mechanically.
// See mods/README.md, section "Portability".

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine
{

// Tick outcomes, as namespace constants rather than an enum class: the
// statuses cross the Lua bridge as plain numbers (scripts name them the same
// way the native surface does), and plain integers survive any ABI boundary
// unchanged - the same convention the animation module's channel/easing/
// mode vocabularies keep. A leaf callback returns one of the first three;
// behavior_status::invalid exists so tick() can answer a refused tick
// (unknown handle) distinctly from a real failure.
namespace behavior_status
{
inline constexpr std::uint32_t success = 0;
inline constexpr std::uint32_t failure = 1;
inline constexpr std::uint32_t running = 2;
inline constexpr std::uint32_t invalid = 3;
} // namespace behavior_status

// Node kinds. The first two are the stateful composites, the next four the
// decorators, the last four the built-in leaves; action/condition are the
// registry-driven leaves whose callbacks live on the consumer side. The
// numbers are part of the contract (the Lua bridge accepts both this module's
// constants and the same names as strings).
namespace behavior_node_kind
{
inline constexpr std::uint32_t sequence      = 0;  // stateful: resumes at the running child
inline constexpr std::uint32_t selector      = 1;  // stateful: resumes at the running child
inline constexpr std::uint32_t invert        = 2;  // decorator, exactly one child
inline constexpr std::uint32_t force_success = 3;  // decorator, exactly one child
inline constexpr std::uint32_t force_failure = 4;  // decorator, exactly one child
inline constexpr std::uint32_t repeat        = 5;  // decorator, exactly one child, count field
inline constexpr std::uint32_t action        = 6;  // leaf, registry callback by name
inline constexpr std::uint32_t condition     = 7;  // leaf, registry callback by name (reader sugar)
inline constexpr std::uint32_t succeed       = 8;  // leaf, constant success
inline constexpr std::uint32_t fail          = 9;  // leaf, constant failure
inline constexpr std::uint32_t wait          = 10; // leaf, waitSeconds then success
} // namespace behavior_node_kind

// One node of a tree description, recursive by value: a description is a
// plain data tree the consumer builds (or the Lua bridge builds from a
// table) and hands over wholesale - the module copies it at createTree and
// owns the runtime state from then on.
struct BehaviorNodeDescription
{
    std::uint32_t kind = behavior_node_kind::sequence;
    // Composite and decorator kinds: the children in evaluation order.
    std::vector<BehaviorNodeDescription> children;
    // action / condition: the registry name the callback resolves through.
    std::string name;
    // repeat: how many child completions the node needs (one evaluation per
    // tick - the unambiguous reading once wait leaves carry time).
    std::uint32_t count = 1;
    // wait: seconds this leaf runs before returning success.
    float waitSeconds = 0.0f;
};

// One tree description: a name (diagnostics and the finished event reference
// it) and a root node. Trees are validated at createTree - kind/arity rules,
// the depth and node caps, finite wait seconds - so a refused tree is a
// precise startup-time style error, never a mid-tick surprise.
struct BehaviorTreeDescription
{
    std::string name;
    BehaviorNodeDescription root;
};

// Published when a tree's root transitioned from running to a finished
// status. Trees that re-complete later publish again; a root that returns
// the same finished status every tick publishes once per running->done
// transition, not once per tick (the throttle that keeps a permanently
// succeeding root from spamming the bus). Auto-tick keeps ticking finished
// trees - the event is news, never a stop signal; a one-shot consumer
// destroys or resets its tree from the event.
struct BehaviorTreeFinishedEvent
{
    std::uint64_t handle = 0;
    std::string name;
    std::uint32_t status = behavior_status::success;
};

// Capability "behavior_tree": AI decision trees for the whole run.
//
// The engine had scripting and timers but no decision structure: an enemy
// that patrols, chases and attacks would hand-roll the same advance-and-
// branch loop every other agent rolls, each with its own forgotten reset
// and its own "stuck in chase forever" bug. This capability is that one
// structure: trees of stateful composites, decorators and leaves, ticked
// per frame (or per explicit call), with the leaf callbacks living on the
// consumer side through a name registry - the module owns the SHAPE of
// decision making, consumers own the ACTIONS.
//
// The composites are deliberately the stateful kind (a running child is
// resumed where it stopped, and a composite resets its subtree whenever it
// completes or aborts): memory-only-while-running is the semantics game AI
// actually wants, it is what keeps wait leaves honest under re-runs, and
// the alternative (stateless re-evaluation from the first child every tick)
// turns every wait into a livelock. A replacement implementation declares
// its own member of the "behavior_tree" family and wins through
// [providers]; consumers keep demanding the family.
class BehaviorTreeCapability
{
public:
    // The implementation-agnostic family name of this capability. Consumers
    // acquire through it; the module's plugin descriptor carries the
    // implementation-bound member name ("behavior_tree.basic").
    static constexpr const char *capabilityName = "behavior_tree";

    // Trees are addressed by handle; handles are never reused.
    using TreeHandle = std::uint64_t;

    static constexpr TreeHandle invalidTree = 0;

    virtual ~BehaviorTreeCapability() = default;

    // Registers the callback behind an action/condition name. The same
    // signature serves both kinds - the distinction is for the reader, the
    // module cannot know whether "can_see_player" is a question or a step.
    // Re-registering an existing name replaces its callback (a live tree
    // picks the new one up on its next leaf resolution - no invalidation
    // ceremony). Returns false for an empty name (logged).
    virtual bool registerAction(const std::string &name, std::function<std::uint32_t()> callback) = 0;

    // Removes one registry entry. Trees whose leaves name it tick to
    // behavior_status::failure with a logged warning until the name
    // returns. Returns false when the name is unknown.
    virtual bool unregisterAction(const std::string &name) = 0;

    // How many callbacks are registered (diagnostics).
    virtual std::uint32_t registeredActionCount() const = 0;

    // Builds one tree from a description and returns its handle, or
    // invalidTree with a precise log for every structural refusal: an
    // empty tree name, a composite with no children, a decorator with not
    // exactly one child, a leaf with children, a repeat with a zero count,
    // a non-finite wait, or the depth/node caps exceeded. A leaf whose
    // action name is not (yet) registered is NOT a refusal - resolution is
    // lazy and per tick, so native code and scripts may register actions
    // after tree creation.
    virtual TreeHandle createTree(const BehaviorTreeDescription &tree) = 0;

    // Ticks one tree once and returns the root's status. deltaSeconds
    // advances wait leaves (a negative or non-finite delta degrades to 0 -
    // a tick never throws on the frame path). Unknown handles return
    // behavior_status::invalid with a logged warning. Ticking is
    // reentrant-safe against the module's auto-tick only by discipline:
    // a consumer that manual-ticks a tree the module also auto-ticks runs
    // it twice per frame (setAutoTick exists for exactly that choice).
    virtual std::uint32_t tick(TreeHandle handle, double deltaSeconds) = 0;

    // Clears one tree's runtime memory (composite child indices, wait
    // remainders, repeat counts) - the next tick runs it from the root as
    // if freshly created. Returns false for unknown handles.
    virtual bool reset(TreeHandle handle) = 0;

    // Whether the module's frame tick drives this tree (the
    // [behavior_tree] auto_tick default applies at creation). Returns
    // false for unknown handles.
    virtual bool setAutoTick(TreeHandle handle, bool autoTick) = 0;

    // Removes one tree. Handles are never reused; the Lua bridge and
    // native consumers alike are expected to drop the handle. Returns
    // false for unknown handles.
    virtual bool destroyTree(TreeHandle handle) = 0;

    // How many trees are live right now (diagnostics and tests).
    virtual std::uint32_t activeTreeCount() const = 0;

    // The tree's description name (empty for unknown handles) - what the
    // finished event carried, answerable after the fact.
    virtual std::string treeName(TreeHandle handle) const = 0;
};

} // namespace engine

#endif // ENGINE_MODULES_BEHAVIOR_TREE_BEHAVIOR_TREE_CAPABILITY_HPP
