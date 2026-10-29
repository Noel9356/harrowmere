#include "behavior_tree.hpp"

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine
{

namespace
{

// The structural caps createTree enforces. They exist because a description
// is consumer data: a table built by a script with one too many recursive
// "children" tables must fail as a precise refusal at creation, never as a
// stack overflow mid-tick. Both caps are far above anything a hand-authored
// tree needs and far below anything that hurts.
constexpr std::uint32_t maxTreeDepth = 64;
constexpr std::uint32_t maxTreeNodes = 1024;

// The Lua table field readers the bridge conversion uses. They exist
// because sol2's get_or overloads go ambiguous when the fallback is a
// std::string, and because a mistyped field (a number where a name belongs)
// must degrade to the default - the same degrades-never-throws contract the
// capability surfaces keep.
std::string stringField(
    const sol::table &table, const char *key, std::string fallback
)
{
    sol::object object = table[key];
    if (!object.valid() || object.get_type() != sol::type::string)
    {
        return fallback;
    }
    return object.as<std::string>();
}

double numberField(
    const sol::table &table, const char *key, double fallback
)
{
    sol::object object = table[key];
    if (!object.valid() || object.get_type() != sol::type::number)
    {
        return fallback;
    }
    return object.as<double>();
}

// The dedicated Lua bridge extension object for the behavior_tree module.
// Owns the sol2 wiring that installs the engine.behavior_* functions into
// every script state. Holds a raw pointer to the BehaviorTreeCapability the
// module provides - the bridge is part of the module, so no capability
// acquisition and no permission check are involved.
//
// The bridge is declarative and stateless by design: a Lua tree table is
// converted into a BehaviorTreeDescription IN THE CALL (the table is read,
// not retained), so the extension never holds a Lua reference that could
// outlive its script state - the lifetime discipline every shipped bridge
// follows. Leaf behavior reaches a Lua-built tree through the native action
// registry or the registry-free built-in leaves.
class BehaviorTreeLuaBridge final : public LuaBridgeExtension
{
public:
    explicit BehaviorTreeLuaBridge(
        BehaviorTreeCapability *capability
    )
        : capability(capability)
    {}

    const char *extensionName() const override
    {
        return "behavior_tree";
    }

    void installLuaBridge(
        luaState *lua
    ) override
    {
        sol::state_view luaView(lua);
        sol::table engine = luaView["engine"];

        // create_tree(name, nodeTable) -> handle (0 = refused, the module
        // logged why). Node tables: { kind = "sequence"|"selector"|
        // "invert"|"force_success"|"force_failure"|"repeat"|"action"|
        // "condition"|"succeed"|"fail"|"wait", children = { ... },
        // name = "...", count = n, seconds = n } - the same vocabulary as
        // the native description, spelled as a content author would.
        engine.set_function(
            "behavior_create_tree", [this](const std::string &treeName, const sol::table &rootTable) -> std::uint64_t {
                BehaviorTreeDescription description;
                description.name = treeName;
                if (!this->nodeFromTable(rootTable, description.root))
                {
                    return BehaviorTreeCapability::invalidTree;
                }
                return capability->createTree(description);
            }
        );
        engine.set_function(
            "behavior_tick", [this](std::uint64_t handle, sol::optional<double> deltaSeconds) -> std::uint32_t {
                return capability->tick(handle, deltaSeconds.value_or(0.0));
            }
        );
        engine.set_function("behavior_reset", [this](std::uint64_t handle) -> bool {
            return capability->reset(handle);
        });
        engine.set_function("behavior_set_auto_tick", [this](std::uint64_t handle, bool autoTick) -> bool {
            return capability->setAutoTick(handle, autoTick);
        });
        engine.set_function("behavior_destroy", [this](std::uint64_t handle) -> bool {
            return capability->destroyTree(handle);
        });
        engine.set_function("behavior_status_name", [](std::uint32_t status) -> std::string {
            switch (status)
            {
            case behavior_status::success:
                return "success";
            case behavior_status::failure:
                return "failure";
            case behavior_status::running:
                return "running";
            default:
                return "invalid";
            }
        });
        engine.set_function("behavior_tree_count", [this]() -> std::uint32_t { return capability->activeTreeCount(); });
    }

private:
    // The Lua table -> description conversion. Returns false (after the
    // module's logger has nothing to say - the refusal surfaces through
    // createTree's own validation) when the table is structurally unusable:
    // not a table, an unknown kind string, a composite without a children
    // table. Field names follow the bridge convention of the other modules
    // (snake_case, short).
    bool nodeFromTable(
        const sol::table &table, BehaviorNodeDescription &outNode
    ) const
    {
        if (!table.valid())
        {
            return false;
        }
        const std::string kindName = stringField(table, "kind", std::string());
        if (kindName == "sequence")
        {
            outNode.kind = behavior_node_kind::sequence;
        } else if (kindName == "selector")
        {
            outNode.kind = behavior_node_kind::selector;
        } else if (kindName == "invert")
        {
            outNode.kind = behavior_node_kind::invert;
        } else if (kindName == "force_success")
        {
            outNode.kind = behavior_node_kind::force_success;
        } else if (kindName == "force_failure")
        {
            outNode.kind = behavior_node_kind::force_failure;
        } else if (kindName == "repeat")
        {
            outNode.kind = behavior_node_kind::repeat;
        } else if (kindName == "action")
        {
            outNode.kind = behavior_node_kind::action;
        } else if (kindName == "condition")
        {
            outNode.kind = behavior_node_kind::condition;
        } else if (kindName == "succeed")
        {
            outNode.kind = behavior_node_kind::succeed;
        } else if (kindName == "fail")
        {
            outNode.kind = behavior_node_kind::fail;
        } else if (kindName == "wait")
        {
            outNode.kind = behavior_node_kind::wait;
        } else
        {
            // An unknown kind string: the description stays empty and
            // createTree refuses it - one validation path, one diagnostic.
            return false;
        }

        outNode.name = stringField(table, "name", std::string());
        // Counts and seconds clamp at the bridge: a script's -3 means "1
        // repeat" and "no wait", not "a huge unsigned count" or "a wait
        // that completes instantly by being negative" - the same degrade-
        // to-the-sane-value contract the capability's own clamps keep.
        const double rawCount   = numberField(table, "count", 1.0);
        outNode.count           = static_cast<std::uint32_t>(std::max(1.0, std::floor(rawCount)));
        const double rawSeconds = numberField(table, "seconds", 0.0);
        outNode.waitSeconds     = static_cast<float>(std::max(0.0, rawSeconds));

        if (outNode.kind == behavior_node_kind::sequence || outNode.kind == behavior_node_kind::selector
            || outNode.kind == behavior_node_kind::invert || outNode.kind == behavior_node_kind::force_success
            || outNode.kind == behavior_node_kind::force_failure || outNode.kind == behavior_node_kind::repeat)
        {
            sol::object childrenObject = table["children"];
            if (!childrenObject.valid() || !childrenObject.is<sol::table>())
            {
                return false;
            }
            sol::table children = childrenObject;
            for (std::size_t index = 1; index <= children.size(); ++index)
            {
                sol::object childObject = children[index];
                if (!childObject.valid() || !childObject.is<sol::table>())
                {
                    return false;
                }
                BehaviorNodeDescription child;
                if (!nodeFromTable(childObject.as<sol::table>(), child))
                {
                    return false;
                }
                outNode.children.push_back(std::move(child));
            }
        }
        return true;
    }

    BehaviorTreeCapability *capability;
};

} // namespace

BehaviorTreeModule::BehaviorTreeModule()
{
    componentManifest = ComponentManifest{
        .name    = "behavior_tree",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        .providedCapabilities
        = {"behavior_tree.basic",
            // The Lua bridge extension member: declares this module as a
            // provider of the "scripting.lua_bridge" family. The
            // lua_scripting host acquires every active member through
            // ComponentContext::acquireEachCapability and asks each one
            // to install its own bindings into the host's engine table.
            "scripting.lua_bridge.behavior_tree"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging"},
        .knownConfigKeys      = {"behavior_tree.auto_tick"},
    };
}

BehaviorTreeModule::~BehaviorTreeModule()
{
    stop();
}

void BehaviorTreeModule::construct(
    ComponentContext &context
)
{
    // The one cross-component touch of the construct phase: the log sink
    // (the requirement is declared in the manifest and in the static plugin
    // descriptor, so the grant was derived from the requirement and the
    // provider is ordered before this module).
    logger = &context.acquireCapability<LoggerCapability>();

    autoTickDefault = context.configurationBooleanInSection("behavior_tree", "auto_tick").value_or(true);

    finishedChannel = &context.eventBus().channel<BehaviorTreeFinishedEvent>();

    // The frame tick: auto-ticked trees advance on the kernel's
    // EngineFrameEvent (the composition root's loop heartbeat), exactly like
    // the scheduler's timers, the tweens, the physics step and the game
    // state walk. The tick source is the loop, not the renderer: no renderer
    // capability include is needed, and a headless run auto-ticks exactly
    // like a rendered one (manual ticks carry their own delta).
    frameTickChannel      = &context.eventBus().channel<EngineFrameEvent>();
    frameTickSubscription = EventSubscription(
        &context.eventBus(),
        typeid(EngineFrameEvent),
        frameTickChannel->subscribe<BehaviorTreeModule, &BehaviorTreeModule::forwardFrameTick>(this)
    );

    // Construct the Lua bridge extension object. The module IS the
    // BehaviorTreeCapability, so `this` is the capability pointer.
    luaBridge = std::make_unique<BehaviorTreeLuaBridge>(this);
}

void BehaviorTreeModule::start()
{
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Information,
            "behavior_tree module started (auto_tick " + std::string(autoTickDefault ? "on" : "off") + ")"
        );
    }
}

void BehaviorTreeModule::stop()
{
    // Reverse of construct: the frame subscription releases before the
    // channels go away with the bus, then every runtime structure empties.
    // Registered actions die with the module - their callbacks were the
    // consumers' closures, and the consumers stop first (reverse
    // dependency order) by the time this module does.
    frameTickSubscription.release();
    trees.clear();
    actions.clear();
    // The registry version moves with the clear: any runtime structure that
    // survives into a next run (nothing does today, but the contract is the
    // contract) must not find a cached resolution stamped with a version
    // the emptied registry also answers.
    ++actionRegistryVersion;
    finishedChannel = nullptr;
    luaBridge.reset();
    logger = nullptr;
}

void *BehaviorTreeModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "behavior_tree.basic")
    {
        return static_cast<BehaviorTreeCapability *>(this);
    }
    // The "scripting.lua_bridge.behavior_tree" member: returns the dedicated
    // BehaviorTreeLuaBridge object this module owns (constructed in
    // construct()).
    if (capabilityName == "scripting.lua_bridge.behavior_tree")
    {
        return luaBridge.get();
    }
    return nullptr;
}

bool BehaviorTreeModule::registerAction(
    const std::string &name, std::function<std::uint32_t()> callback
)
{
    if (name.empty())
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning, "behavior_tree module: registerAction refused (the action name is empty)"
            );
        }
        return false;
    }
    // Replace-on-re-register is the hot-reload story: a live tree resolves
    // its leaves through the version-stamped cache (see
    // actionRegistryVersion), and the version bump below invalidates every
    // cached resolution, so the new callback is one map assignment away
    // from every consumer of the name.
    actions[name] = std::move(callback);
    ++actionRegistryVersion;
    return true;
}

bool BehaviorTreeModule::unregisterAction(
    const std::string &name
)
{
    const bool removed = actions.erase(name) != 0;
    if (removed)
    {
        // Same invalidation as the registration path: leaves holding a
        // resolution for any name re-derive on their next evaluation, so a
        // leaf whose action just vanished fails (with its once-only
        // warning) instead of calling a dangling pointer.
        ++actionRegistryVersion;
    }
    return removed;
}

std::uint32_t BehaviorTreeModule::registeredActionCount() const
{
    return static_cast<std::uint32_t>(actions.size());
}

bool BehaviorTreeModule::buildRuntimeNode(
    const BehaviorNodeDescription &description,
    RuntimeNode &outNode,
    std::uint32_t depth,
    std::uint32_t &nodeBudget,
    std::string &refusalReason
)
{
    if (depth > maxTreeDepth)
    {
        refusalReason = "the tree nests deeper than " + std::to_string(maxTreeDepth) + " levels";
        return false;
    }
    if (nodeBudget == 0)
    {
        refusalReason = "the tree carries more than " + std::to_string(maxTreeNodes) + " nodes";
        return false;
    }
    --nodeBudget;

    const std::uint32_t kind = description.kind;
    const bool isComposite   = kind == behavior_node_kind::sequence || kind == behavior_node_kind::selector;
    const bool isDecorator   = kind == behavior_node_kind::invert || kind == behavior_node_kind::force_success
                            || kind == behavior_node_kind::force_failure || kind == behavior_node_kind::repeat;
    const bool isLeaf        = kind == behavior_node_kind::action || kind == behavior_node_kind::condition
                            || kind == behavior_node_kind::succeed || kind == behavior_node_kind::fail
                            || kind == behavior_node_kind::wait;
    if (!isComposite && !isDecorator && !isLeaf)
    {
        refusalReason = "node kind " + std::to_string(kind) + " is not a known kind";
        return false;
    }

    if (isComposite && description.children.empty())
    {
        refusalReason = std::string(kind == behavior_node_kind::sequence ? "a sequence" : "a selector")
                      + " needs at least one child";
        return false;
    }
    if (isDecorator && description.children.size() != 1)
    {
        refusalReason = "a decorator needs exactly one child (got " + std::to_string(description.children.size()) + ")";
        return false;
    }
    if (isLeaf && !description.children.empty())
    {
        refusalReason = "a leaf carries no children";
        return false;
    }
    if (kind == behavior_node_kind::repeat && description.count == 0)
    {
        refusalReason = "a repeat node needs a count of at least 1";
        return false;
    }
    if (kind == behavior_node_kind::wait && !std::isfinite(description.waitSeconds))
    {
        refusalReason = "a wait node needs finite seconds";
        return false;
    }
    if ((kind == behavior_node_kind::action || kind == behavior_node_kind::condition) && description.name.empty())
    {
        refusalReason = "an action/condition leaf needs a registry name";
        return false;
    }

    outNode                  = RuntimeNode{};
    outNode.description      = description;
    outNode.remainingRepeats = description.count == 0 ? 1u : description.count;
    outNode.waitRemaining    = static_cast<double>(description.waitSeconds);
    outNode.childIndex       = 0;

    outNode.children.reserve(description.children.size());
    for (const BehaviorNodeDescription &childDescription : description.children)
    {
        RuntimeNode child;
        if (!buildRuntimeNode(childDescription, child, depth + 1u, nodeBudget, refusalReason))
        {
            return false;
        }
        outNode.children.push_back(std::move(child));
    }
    return true;
}

BehaviorTreeCapability::TreeHandle BehaviorTreeModule::createTree(
    const BehaviorTreeDescription &tree
)
{
    if (tree.name.empty())
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "behavior_tree module: createTree refused (the tree name is empty - diagnostics and the finished "
                "event reference it)"
            );
        }
        return invalidTree;
    }

    RuntimeTree runtime;
    runtime.name             = tree.name;
    runtime.autoTick         = autoTickDefault;
    runtime.lastTickRunning  = true;
    std::uint32_t nodeBudget = maxTreeNodes;
    std::string refusalReason;
    if (!buildRuntimeNode(tree.root, runtime.root, 1u, nodeBudget, refusalReason))
    {
        if (logger != nullptr)
        {
            logger->logMessage(
                LogLevel::Warning,
                "behavior_tree module: createTree refused tree '" + tree.name + "' (" + refusalReason + ")"
            );
        }
        return invalidTree;
    }

    runtime.handle          = nextHandle++;
    const TreeHandle handle = runtime.handle;
    trees.push_back(std::move(runtime));
    return handle;
}

std::uint32_t BehaviorTreeModule::evaluateNode(
    RuntimeNode &node, double deltaSeconds, std::uint32_t depth
)
{
    // Depth is the structural recursion guard behind the caps: createTree
    // already bounded the shape, so this assert-style clamp is the
    // belt-and-suspenders that keeps a future edit from re-opening the
    // stack overflow a script table could otherwise reach.
    if (depth > maxTreeDepth)
    {
        return behavior_status::failure;
    }

    const std::uint32_t kind = node.description.kind;
    switch (kind)
    {
    case behavior_node_kind::sequence: {
        // Stateful: resume at the running child; a failure or a
        // completion resets the whole subtree's memory (the only
        // semantics that keep wait leaves honest under re-runs).
        for (std::uint32_t index = node.childIndex; index < node.children.size(); ++index)
        {
            const std::uint32_t status = evaluateNode(node.children[index], deltaSeconds, depth + 1u);
            if (status == behavior_status::running)
            {
                node.childIndex = index;
                return behavior_status::running;
            }
            if (status == behavior_status::failure)
            {
                node.childIndex = 0;
                resetNode(node);
                return behavior_status::failure;
            }
        }
        node.childIndex = 0;
        resetNode(node);
        return behavior_status::success;
    }
    case behavior_node_kind::selector: {
        // The mirror of the sequence: first success wins, running
        // resumes, exhaustion fails.
        for (std::uint32_t index = node.childIndex; index < node.children.size(); ++index)
        {
            const std::uint32_t status = evaluateNode(node.children[index], deltaSeconds, depth + 1u);
            if (status == behavior_status::running)
            {
                node.childIndex = index;
                return behavior_status::running;
            }
            if (status == behavior_status::success)
            {
                node.childIndex = 0;
                resetNode(node);
                return behavior_status::success;
            }
        }
        node.childIndex = 0;
        resetNode(node);
        return behavior_status::failure;
    }
    case behavior_node_kind::invert: {
        const std::uint32_t status = evaluateNode(node.children[0], deltaSeconds, depth + 1u);
        if (status == behavior_status::success)
        {
            return behavior_status::failure;
        }
        if (status == behavior_status::failure)
        {
            return behavior_status::success;
        }
        return behavior_status::running;
    }
    case behavior_node_kind::force_success: {
        const std::uint32_t status = evaluateNode(node.children[0], deltaSeconds, depth + 1u);
        return status == behavior_status::running ? behavior_status::running : behavior_status::success;
    }
    case behavior_node_kind::force_failure: {
        const std::uint32_t status = evaluateNode(node.children[0], deltaSeconds, depth + 1u);
        return status == behavior_status::running ? behavior_status::running : behavior_status::failure;
    }
    case behavior_node_kind::repeat: {
        // One child evaluation per tick, completions counted across
        // ticks - the unambiguous reading once wait leaves carry time
        // (a multi-evaluation-per-tick reading would burn the same
        // delta into every evaluation and complete a wait instantly).
        const std::uint32_t status = evaluateNode(node.children[0], deltaSeconds, depth + 1u);
        if (status == behavior_status::running)
        {
            return behavior_status::running;
        }
        if (status == behavior_status::failure)
        {
            node.remainingRepeats = node.description.count == 0 ? 1u : node.description.count;
            resetNode(node.children[0]);
            return behavior_status::failure;
        }
        if (node.remainingRepeats > 0u)
        {
            --node.remainingRepeats;
        }
        if (node.remainingRepeats > 0u)
        {
            return behavior_status::running;
        }
        node.remainingRepeats = node.description.count == 0 ? 1u : node.description.count;
        resetNode(node.children[0]);
        return behavior_status::success;
    }
    case behavior_node_kind::action:
    case behavior_node_kind::condition: {
        // The leaf's registry resolution is version-stamped (see
        // actionRegistryVersion): a callback pointer stays valid until the
        // next registry mutation, so the steady state costs one integer
        // compare instead of the string hash this leaf used to pay per
        // evaluation. The first evaluation after any mutation re-runs the
        // find, which is also what picks up a re-registered callback - the
        // hot-reload story the find-per-tick discipline provided, unchanged.
        if (node.boundActionVersion != actionRegistryVersion)
        {
            const auto found        = actions.find(node.description.name);
            node.boundAction        = found != actions.end() && found->second ? &found->second : nullptr;
            node.boundActionVersion = actionRegistryVersion;
        }
        if (node.boundAction == nullptr)
        {
            if (!node.warnedMissingAction)
            {
                node.warnedMissingAction = true;
                if (logger != nullptr)
                {
                    logger->logMessage(
                        LogLevel::Warning,
                        "behavior_tree module: leaf '" + node.description.name
                            + "' has no registered action (failing until one is registered)"
                    );
                }
            }
            return behavior_status::failure;
        }
        // The callback's own bad status (an uninvited 7) degrades to
        // failure: the registry contract names three outcomes, and a
        // caller bug must not read as "running" (a stuck tree) or
        // "success" (a false positive).
        const std::uint32_t status = (*node.boundAction)();
        return status == behavior_status::success || status == behavior_status::failure
                    || status == behavior_status::running
                 ? status
                 : behavior_status::failure;
    }
    case behavior_node_kind::succeed: {
        return behavior_status::success;
    }
    case behavior_node_kind::fail: {
        return behavior_status::failure;
    }
    case behavior_node_kind::wait: {
        // wait: burn the delta, succeed when the remainder empties.
        // The refill-on-success is what makes a re-entered wait wait
        // again (the composite reset already covers the abort path).
        node.waitRemaining -= deltaSeconds > 0.0 && std::isfinite(deltaSeconds) ? deltaSeconds : 0.0;
        if (node.waitRemaining > 0.0)
        {
            return behavior_status::running;
        }
        node.waitRemaining = static_cast<double>(node.description.waitSeconds);
        return behavior_status::success;
    }
    default: {
        // Unreachable through createTree (unknown kinds are refused at
        // build time); a future kind added to the vocabulary without an
        // evaluator fails honestly instead of behaving like another
        // node by accident.
        return behavior_status::failure;
    }
    }
}

void BehaviorTreeModule::resetNode(
    RuntimeNode &node
)
{
    node.childIndex          = 0;
    node.remainingRepeats    = node.description.count == 0 ? 1u : node.description.count;
    node.waitRemaining       = static_cast<double>(node.description.waitSeconds);
    node.warnedMissingAction = false;
    for (RuntimeNode &child : node.children)
    {
        resetNode(child);
    }
}

std::uint32_t BehaviorTreeModule::tick(
    TreeHandle handle, double deltaSeconds
)
{
    for (RuntimeTree &tree : trees)
    {
        if (tree.handle == handle)
        {
            return tickTree(tree, deltaSeconds);
        }
    }
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Warning, "behavior_tree module: tick refused (handle " + std::to_string(handle) + " is not live)"
        );
    }
    return behavior_status::invalid;
}

// One tree's tick, in place. Split out of the handle-based tick above so
// the auto-tick walk - which already holds the RuntimeTree& - does not send
// every tree through a fresh linear scan of the vector it just walked (with
// n live trees that made the frame tick O(n squared) in scan steps, plus a
// string-building warning path per unknown-handle case it could never
// reach). Status values, event publishing and the delta contract are the
// manual tick's exactly.
std::uint32_t BehaviorTreeModule::tickTree(
    RuntimeTree &tree, double deltaSeconds
)
{
    const std::uint32_t status = evaluateNode(tree.root, deltaSeconds, 1u);
    // The finished event fires on the running->done transition, not on
    // every finished tick: a root that returns success every frame
    // publishes once, then goes quiet until it runs again - the
    // throttle that keeps the bus honest without changing the tree.
    if (status != behavior_status::running)
    {
        if (tree.lastTickRunning)
        {
            tree.lastTickRunning = false;
            finishedChannel->publish(BehaviorTreeFinishedEvent{tree.handle, tree.name, status});
        }
    } else
    {
        tree.lastTickRunning = true;
    }
    return status;
}

bool BehaviorTreeModule::reset(
    TreeHandle handle
)
{
    for (RuntimeTree &tree : trees)
    {
        if (tree.handle == handle)
        {
            resetNode(tree.root);
            tree.lastTickRunning = true;
            return true;
        }
    }
    return false;
}

bool BehaviorTreeModule::setAutoTick(
    TreeHandle handle, bool autoTick
)
{
    for (RuntimeTree &tree : trees)
    {
        if (tree.handle == handle)
        {
            tree.autoTick = autoTick;
            return true;
        }
    }
    if (logger != nullptr)
    {
        logger->logMessage(
            LogLevel::Warning,
            "behavior_tree module: setAutoTick refused (handle " + std::to_string(handle) + " is not live)"
        );
    }
    return false;
}

bool BehaviorTreeModule::destroyTree(
    TreeHandle handle
)
{
    for (std::size_t index = 0; index < trees.size(); ++index)
    {
        if (trees[index].handle == handle)
        {
            // Order-preserving erase: the auto-tick walks in creation
            // order, and that order is part of the determinism contract.
            trees.erase(trees.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
    }
    return false;
}

std::uint32_t BehaviorTreeModule::activeTreeCount() const
{
    return static_cast<std::uint32_t>(trees.size());
}

std::string BehaviorTreeModule::treeName(
    TreeHandle handle
) const
{
    for (const RuntimeTree &tree : trees)
    {
        if (tree.handle == handle)
        {
            return tree.name;
        }
    }
    return std::string();
}

void BehaviorTreeModule::forwardFrameTick(
    const EngineFrameEvent &event
)
{
    // Tree-free frames (the common case for tools and UI-only runs) return
    // at the empty check: no clock read, no walk. The delta is the event's
    // own - the composition root measured it once for the whole iteration
    // (docs/architecture.md, "The frame clock") - so there is no clock state
    // to seed and no first-frame special case: the first frame carries
    // delta 0, which ticks nothing anywhere.
    if (trees.empty())
    {
        return;
    }
    const double frameDelta = event.deltaSeconds;

    // Creation order, every auto-ticked tree, the same delta: the
    // determinism contract is "same trees, same frame, same statuses". The
    // tick body is tickTree(tree, ...), called with the reference in hand -
    // the handle-based tick's re-scan per tree is gone from this loop.
    for (RuntimeTree &tree : trees)
    {
        if (tree.autoTick)
        {
            (void)tickTree(tree, frameDelta);
        }
    }
}

} // namespace engine
