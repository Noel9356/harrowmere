#ifndef ENGINE_MODULES_BEHAVIOR_TREE_HPP
#define ENGINE_MODULES_BEHAVIOR_TREE_HPP

#include "behavior_tree_capability.hpp"
#include "logging_capability.hpp"
#include "lua_bridge_extension.hpp"
#include "engine/kernel/component_interface.hpp" // EngineFrameEvent (the tick
                                                 // source: declared in the
                                                 // kernel's component-facing
                                                 // header, so the module needs
                                                 // no renderer capability
                                                 // include and auto-ticks in
                                                 // renderer-less runs too)

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine
{

// Module "behavior_tree": provides the BehaviorTreeCapability (family member
// "behavior_tree.basic").
//
// One registry, one tree store, one frame tick for the whole run. The tick
// is frame-paced on the kernel's EngineFrameEvent (the composition root's
// loop heartbeat - the tick every gameplay
// module uses) with the seeded-clock delta discipline the animation and
// game_state modules keep: the first frame seeds the clock, tree-free
// frames never read it, and every auto-ticked tree receives the same frame
// delta the other consumers saw. Manual ticks carry their own delta - the
// module never assumes which clock a consumer drives it with.
//
// The module additionally provides the "scripting.lua_bridge.behavior_tree"
// Lua bridge extension: it installs engine.behavior_create_tree,
// engine.behavior_tick, engine.behavior_reset, engine.behavior_destroy,
// engine.behavior_set_auto_tick, engine.behavior_status_name and
// engine.behavior_tree_count into every script state through a dedicated
// BehaviorTreeLuaBridge object owned by this module (see
// behavior_tree.cpp). The bridge is DECLARATIVE by design - it converts a
// Lua table into the same description the native surface builds and retains
// no Lua references, which is the lifetime discipline every shipped bridge
// follows (the extension object outlives any single script state); leaf
// behavior comes from the native action registry, or from the built-in
// leaves (succeed / fail / wait) that need no registry at all.
class BehaviorTreeModule final : public ModuleInterface, public BehaviorTreeCapability
{
public:
    BehaviorTreeModule();
    ~BehaviorTreeModule();

    const ComponentManifest &manifest() const override
    {
        return componentManifest;
    }

    void construct(ComponentContext &context) override;
    void start() override;
    void stop() override;

    void *capabilityPointerFor(const std::string &capabilityName) override;

    // BehaviorTreeCapability.
    bool registerAction(const std::string &name, std::function<std::uint32_t()> callback) override;
    bool unregisterAction(const std::string &name) override;
    std::uint32_t registeredActionCount() const override;
    TreeHandle createTree(const BehaviorTreeDescription &tree) override;
    std::uint32_t tick(TreeHandle handle, double deltaSeconds) override;
    bool reset(TreeHandle handle) override;
    bool setAutoTick(TreeHandle handle, bool autoTick) override;
    bool destroyTree(TreeHandle handle) override;
    std::uint32_t activeTreeCount() const override;
    std::string treeName(TreeHandle handle) const override;

private:
    // One runtime node: the description as created plus the memory-only-
    // while-running state the composites, repeat and wait kinds keep. The
    // children own their own runtime state recursively - a subtree reset is
    // a recursive walk, and "reset" is exactly what a completing composite
    // does to its children (see behavior_tree.cpp).
    struct RuntimeNode
    {
        BehaviorNodeDescription description;
        std::vector<RuntimeNode> children;
        // Stateful composites: the child the next tick resumes at.
        std::uint32_t childIndex = 0;
        // repeat: completions still needed before success.
        std::uint32_t remainingRepeats = 1;
        // wait: seconds still to burn (float math, double accumulator).
        double waitRemaining = 0.0;
        // The once-only warning for a leaf whose action name is not
        // registered: the first tick logs, the rest fail quietly - a
        // misnamed leaf must be visible, an auto-ticked tree must not spam
        // the log every frame.
        bool warnedMissingAction = false;
        // The leaf registry resolution, version-stamped (see
        // actionRegistryVersion): the callback pointer the leaf's NEXT
        // evaluation will use, valid only while the registry version it was
        // resolved at is current. A registry mutation bumps the version, so
        // every cached resolution is re-derived on the leaf's next
        // evaluation - a re-registered callback is picked up on the next
        // leaf tick exactly like the find-per-tick discipline, without the
        // per-leaf-per-tick string hash every action/condition evaluation
        // paid.
        std::uint64_t boundActionVersion                  = 0;
        const std::function<std::uint32_t()> *boundAction = nullptr;
    };

    // One live tree: the runtime root, the identity (handle, name) and the
    // bookkeeping the tick loop needs - whether the module's frame tick
    // drives it and what the root returned last tick (the finished event's
    // running->done transition detector).
    struct RuntimeTree
    {
        RuntimeNode root;
        std::uint64_t handle = invalidTree;
        std::string name;
        bool autoTick        = true;
        bool lastTickRunning = true;
    };

    // The recursive evaluators, shared by the auto tick and the manual one.
    std::uint32_t evaluateNode(RuntimeNode &node, double deltaSeconds, std::uint32_t depth);
    void resetNode(RuntimeNode &node);
    // One tree's tick, in place: the evaluator walk plus the finished
    // event's running->done transition bookkeeping. Both the manual tick
    // (which finds the tree first) and the auto-tick walk (which already
    // holds the RuntimeTree&) come through here, so the auto tick does not
    // re-scan the tree vector per tree to re-find what it already had.
    std::uint32_t tickTree(RuntimeTree &tree, double deltaSeconds);
    // The structural validator/builder behind createTree: copies the
    // description into runtime nodes while enforcing the kind/arity/cap
    // rules; returns false with the precise reason filled for the log.
    bool buildRuntimeNode(
        const BehaviorNodeDescription &description,
        RuntimeNode &outNode,
        std::uint32_t depth,
        std::uint32_t &nodeBudget,
        std::string &refusalReason
    );

    // The EngineFrameEvent handler: the auto-tick walk in creation order,
    // advanced by the event's deltaSeconds (the frame clock every
    // frame-tick module shares - no clock read of its own).
    void forwardFrameTick(const EngineFrameEvent &event);

    ComponentManifest componentManifest;
    LoggerCapability *logger = nullptr;

    // The action registry: name -> callback. Re-registration replaces.
    // Leaves resolve through a version-stamped cache (see
    // actionRegistryVersion): the first evaluation after any registry
    // mutation re-runs the find, and the resolved callback pointer is reused
    // until the NEXT mutation - so a hot-reloaded callback stays one map
    // assignment away from every consumer of the name, while the steady
    // state stops paying a string hash per leaf per tick.
    std::unordered_map<std::string, std::function<std::uint32_t()>> actions;
    // Bumped by every registry mutation (registerAction, unregisterAction,
    // stop's clear): the stamp every leaf's cached resolution carries, and
    // the value a leaf compares against before reusing its cached callback
    // pointer. Monotonic - a mismatch is always a stale resolution.
    std::uint64_t actionRegistryVersion = 1;

    // The live trees, in creation order (the auto-tick's deterministic
    // order). A vector of values: the count is small and the order IS the
    // contract; handles index nothing - lookups are a linear scan (tiny n),
    // destruction is a swap-and-pop-free erase that preserves order.
    std::vector<RuntimeTree> trees;
    std::uint64_t nextHandle = 1;
    bool autoTickDefault     = true;

    // The finished event channel, resolved in construct.
    EventChannel<BehaviorTreeFinishedEvent> *finishedChannel = nullptr;

    // The frame tick: the subscription. The delta comes from the event (the
    // composition root's frame clock), so there is no clock state here; a
    // frame with no trees returns at the empty check.
    EventChannel<EngineFrameEvent> *frameTickChannel = nullptr;
    EventSubscription frameTickSubscription;

    // The Lua bridge extension object owned by this module. Constructed in
    // construct() and returned from capabilityPointerFor for the
    // "scripting.lua_bridge.behavior_tree" member.
    std::unique_ptr<LuaBridgeExtension> luaBridge;
};

} // namespace engine

#endif // ENGINE_MODULES_BEHAVIOR_TREE_HPP
