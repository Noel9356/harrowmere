#include "bvh.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace engine
{

namespace
{

// The [bvh].split choices, in the spelling the configuration accepts. The
// default is the benchmark winner (modules/bvh/benchmark.cpp): under the
// engine's build-per-call contract the midpoint tree has the lowest
// build + query total at every measured box count.
constexpr const char *defaultSplitName = "midpoint";

BvhSplitMethod splitMethodFromName(
    const std::string &name
)
{
    if (name == "median")
    {
        return BvhSplitMethod::Median;
    }
    if (name == "midpoint")
    {
        return BvhSplitMethod::Midpoint;
    }
    if (name == "sah")
    {
        return BvhSplitMethod::Sah;
    }
    return BvhSplitMethod::Midpoint; // unreachable; parseSplitName rejects first
}

// Returns an empty string when the name parses; otherwise the diagnostic.
std::string validateSplitName(
    const std::string &name
)
{
    if (name == "median" || name == "midpoint" || name == "sah")
    {
        return {};
    }
    return "bvh module: [bvh] split '" + name + "' is not a known strategy (median, midpoint, sah)";
}

const char *nameOfSplitMethod(
    BvhSplitMethod method
)
{
    switch (method)
    {
    case BvhSplitMethod::Median:
        return "median";
    case BvhSplitMethod::Midpoint:
        return "midpoint";
    case BvhSplitMethod::Sah:
        return "sah";
    }
    return "midpoint";
}

} // namespace

// The manifest is ready at instantiation time: the kernel verifies it against
// the plugin descriptor right after creating the instance, before any
// construct phase runs.
BvhModule::BvhModule()
{
    componentManifest = ComponentManifest{
        .name    = "bvh",
        .kind    = ComponentKind::Module,
        .version = "0.1.0",
        // The implementation-bound member name of the "bvh" family: a different
        // spatial index declares its own member and wins through the [providers]
        // configuration; consumers keep demanding the family.
        .providedCapabilities = {"bvh.binary"},
        .requiredCapabilities = {"logging"},
        .optionalCapabilities = {},
        .requiredComponents   = {},
        .requiredPermissions  = {"capability.logging"},
        .knownConfigKeys      = {"bvh.split", "bvh.traversal_cost"},
    };
}

void BvhModule::construct(
    ComponentContext &context
)
{
    logger = &context.acquireCapability<LoggerCapability>();

    // Split strategy: [bvh].split, one of median | midpoint | sah. The
    // shipped module default (config.toml) and the code default agree on the
    // benchmark winner; an assembly overrides from engine.toml, which wins
    // over every defaults layer.
    const std::string splitName = context.configurationString("split").value_or(defaultSplitName);
    if (const std::string splitError = validateSplitName(splitName); !splitError.empty())
    {
        throw std::runtime_error(splitError);
    }
    splitMethod = splitMethodFromName(splitName);

    // The SAH cost model's node-visit weight: how expensive one more tree
    // node is relative to one unit of child-surface area. 1.0 (the default)
    // is the historical balance; the value only shapes trees the "sah"
    // strategy builds, so the other strategies accept the key without
    // behavioral effect. Clamped to the sane positive range - a zero or
    // negative weight would tell the sweep that visiting nodes is free, and
    // it would answer with degenerate linear-list trees.
    sahTraversalCost
        = static_cast<float>(std::clamp(context.configurationFloating("traversal_cost").value_or(1.0), 0.1, 10.0));
}

void BvhModule::start()
{
    logger->logMessage(
        LogLevel::Information,
        std::string("bvh module started (binary ") + nameOfSplitMethod(splitMethod) + "-split hierarchy)"
    );
}

void BvhModule::stop()
{
    logger = nullptr;
}

void *BvhModule::capabilityPointerFor(
    const std::string &capabilityName
)
{
    if (capabilityName == "bvh.binary")
    {
        return static_cast<BvhCapability *>(this);
    }
    return nullptr;
}

void BvhModule::overlappingPairs(
    const std::vector<AxisAlignedBoundingBox> &boxes, std::vector<std::vector<std::uint32_t>> &outOverlaps
)
{
    // One hierarchy per call, ONE self-collision traversal per batch, split
    // strategy from [bvh]. At the registry cap the whole batch is
    // microseconds. The hierarchy OBJECT is a retained member scratch: every
    // build rebuilds its content from the given boxes (the stateless
    // per-call contract is untouched), while the node array, box copy and
    // split scratch keep their capacity across calls - the engine's own
    // build-per-call workload stops allocating after the first step.
    //
    // Rebuild elision considered and rejected: when the batch's input is
    // byte-identical to the previous call's, the rebuild would reproduce the
    // exact same node array (build is a deterministic pure function), so a
    // retained-input cache could answer the repeat call without rebuilding.
    // The engine's own broadphase can never benefit, though: the bodies'
    // bounds change every step by construction. A body at rest re-integrates
    // its position every step (the gravity kick is applied before the sleep
    // check, so with the shipped defaults the per-step speed exceeds
    // sleep_speed and resting bodies never sleep), and a body resting on
    // another body creeps at float-ulp scale - the sphere-box separation
    // never closes exactly. The bounds vector is therefore never
    // byte-stable from step to step in a live field, the cache would miss on
    // every step, and the engine would carry a retained input copy (24 bytes
    // per box) for nothing. Any weaker input test (a tolerance, a hash) would
    // answer queries against stale bounds - an observable behavior change -
    // and any incremental update of only the changed subtrees reorders the
    // tree's leaves, which reorders the emitted pairs and with them the
    // contact-resolution order the fixed-timestep run keeps deterministic.
    // The rebuild stays unconditional.
    sharedHierarchy.build(boxes, splitMethod, sahTraversalCost);
    // selfOverlaps applies the same resize discipline this call always had:
    // resize (not clear + resize) preserves every inner vector's capacity
    // when the caller reuses outOverlaps across calls (e.g. the physics
    // module's per-step broadphase), so the per-frame batch stops touching
    // the heap after the high-water step. On a fresh outOverlaps the
    // behavior is identical to a plain resize.
    sharedHierarchy.selfOverlaps(outOverlaps);
}

void BvhModule::queryOverlaps(
    const std::vector<AxisAlignedBoundingBox> &boxes,
    const AxisAlignedBoundingBox &query,
    std::vector<std::uint32_t> &outIndices
)
{
    sharedHierarchy.build(boxes, splitMethod, sahTraversalCost);
    sharedHierarchy.queryOverlaps(query, outIndices);
}

} // namespace engine
