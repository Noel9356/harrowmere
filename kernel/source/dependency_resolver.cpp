#include "engine/kernel/dependency_resolver.hpp"

#include "engine/kernel/capability_name.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace engine
{

namespace
{

using ManifestIndex = std::size_t;

// Depth first search state for cycle extraction. Unvisited nodes have not
// been entered yet; InProgress nodes are on the current search path; a fully
// processed node is Visited and must never be re-entered (a stale InProgress
// state from an earlier, failed search would make a later search report a
// back edge into a node that is not on its path at all - the cycle report
// then slices an empty range and the precise chain is lost).
enum class VisitState
{
    Unvisited,
    InProgress,
    Visited
};

} // namespace

DependencyResolver::DependencyResolver(
    const std::vector<ComponentManifest> &manifests, std::vector<ProviderSelection> selections
)
    : manifests_(&manifests), selections_(std::move(selections))
{}

DependencyResolver::ResolutionResult DependencyResolver::resolve() const
{
    ResolutionResult result;

    // The manifest vector the constructor views (see the header comment).
    const std::vector<ComponentManifest> &manifests = *manifests_;

    const ManifestIndex manifestCount = manifests.size();

    // Component name lookup.
    std::map<std::string, ManifestIndex> componentIndices;
    for (ManifestIndex index = 0; index < manifestCount; ++index)
    {
        componentIndices.emplace(manifests[index].name, index);
    }

    // Capability name to provider indices, in manifest order.
    std::map<std::string, std::vector<ManifestIndex>> capabilityProviders;
    for (ManifestIndex index = 0; index < manifestCount; ++index)
    {
        for (const std::string &capabilityName : manifests[index].providedCapabilities)
        {
            capabilityProviders[capabilityName].push_back(index);
        }
    }

    // Provider candidates for one requirement: exact declarations first, then
    // - when the requirement names a family - every member of that family.
    // A family requirement several exact providers satisfy is ambiguity among
    // the exact providers; members only come into play when no exact provider
    // exists. The family test is capabilityRequirementSatisfiedBy's parent-of
    // test, so a requirement may itself be a nested family (dotted) and still
    // gather its members - "scripting.lua_bridge" is the family of
    // "scripting.lua_bridge.scene" (see capability_name.hpp); the old
    // dotted-requirement short-circuit here cut exactly those families off
    // and left the bridge extension point with zero members and zero edges.
    const auto candidateProviders = [&capabilityProviders](const std::string &requirement) {
        std::vector<ManifestIndex> candidates;
        const auto exact = capabilityProviders.find(requirement);
        if (exact != capabilityProviders.end())
        {
            candidates = exact->second;
            return candidates;
        }
        for (const auto &entry : capabilityProviders)
        {
            if (capabilityRequirementSatisfiedBy(requirement, entry.first))
            {
                candidates.insert(candidates.end(), entry.second.begin(), entry.second.end());
            }
        }
        return candidates;
    };

    // Apply provider selections and report ambiguity or bad selections.
    // Requirement names cover BOTH declaration kinds: a required capability
    // and an optional capability resolve through the identical machinery
    // (exact providers, then family members, then the [providers] choice) so
    // the graph's determinism never depends on how the consumer declared the
    // dependency. The differences live further down: a requirement with zero
    // providers is an error for a required consumer and a plain "not
    // available" for an optional one, and an optional requirement that
    // resolves to SEVERAL members with no [providers] selection is an
    // extension point, not an ambiguity - every member orders before every
    // consumer, the plural acquisition takes all of them, and the singular
    // probe answers "unavailable" (never a silent pick, so scan order can
    // never decide a value). A required multi-provider capability without a
    // selection stays the ambiguity error it always was: the singular acquire
    // would have to pick one.
    std::map<std::string, ManifestIndex> selectedProvider; // requirement -> provider index
    // The extension-point families: optional-only requirements whose member
    // set resolved to several providers with no selection. The edge phase
    // connects every consumer of such a requirement to every provider.
    std::map<std::string, std::vector<ManifestIndex>> extensionProviders;
    {
        // Required vs optional-only, per requirement name: a name any
        // component REQUIRES is required everywhere (the strictest reading
        // drives the diagnostics); a name only ever declared optional is the
        // plural-capable kind.
        std::set<std::string> requiredNames;
        for (const ComponentManifest &manifest : manifests)
        {
            requiredNames.insert(manifest.requiredCapabilities.begin(), manifest.requiredCapabilities.end());
        }
        std::set<std::string> requirementNames(requiredNames);
        for (const ComponentManifest &manifest : manifests)
        {
            requirementNames.insert(manifest.optionalCapabilities.begin(), manifest.optionalCapabilities.end());
        }
        for (const std::string &requirement : requirementNames)
        {
            const bool optionalOnly                    = requiredNames.find(requirement) == requiredNames.end();
            const std::vector<ManifestIndex> providers = candidateProviders(requirement);

            const auto selection = std::find_if(
                selections_.begin(), selections_.end(), [&requirement](const ProviderSelection &candidate) {
                    return candidate.capabilityName == requirement;
                }
            );

            if (providers.size() > 1)
            {
                if (selection == selections_.end())
                {
                    if (optionalOnly)
                    {
                        // Extension point (see the phase comment): every
                        // active member is an ordering edge; no pick happens,
                        // so no [providers] entry is demanded.
                        extensionProviders.emplace(requirement, providers);
                        continue;
                    }
                    std::string providerList;
                    for (ManifestIndex providerIndex : providers)
                    {
                        if (!providerList.empty())
                        {
                            providerList += ", ";
                        }
                        providerList += manifests[providerIndex].name;
                    }
                    ResolutionDiagnostic diagnostic;
                    diagnostic.kind    = ResolutionDiagnostic::Kind::AmbiguousCapability;
                    diagnostic.message = "capability '" + requirement + "' is provided by several components ["
                                       + providerList + "]; choose one in the configuration under [providers].";
                    result.diagnostics.push_back(std::move(diagnostic));
                } else
                {
                    const auto chosen = std::find_if(
                        providers.begin(),
                        providers.end(),
                        [this, &selection, &manifests](ManifestIndex providerIndex) {
                            return manifests[providerIndex].name == selection->providerName;
                        }
                    );
                    if (chosen == providers.end())
                    {
                        ResolutionDiagnostic diagnostic;
                        diagnostic.kind    = ResolutionDiagnostic::Kind::UnknownSelectedProvider;
                        diagnostic.message = "configuration selects '" + selection->providerName + "' for capability '"
                                           + requirement + "' but that component does not provide the capability.";
                        diagnostic.involvementChain.push_back(selection->providerName);
                        diagnostic.involvementChain.push_back(requirement);
                        result.diagnostics.push_back(std::move(diagnostic));
                    } else
                    {
                        selectedProvider.emplace(requirement, *chosen);
                    }
                }
            } else if (providers.size() == 1)
            {
                if (selection != selections_.end() && manifests[providers.front()].name != selection->providerName)
                {
                    ResolutionDiagnostic diagnostic;
                    diagnostic.kind    = ResolutionDiagnostic::Kind::UnknownSelectedProvider;
                    diagnostic.message = "configuration selects '" + selection->providerName + "' for capability '"
                                       + requirement + "' but the only provider is '"
                                       + manifests[providers.front()].name + "'.";
                    result.diagnostics.push_back(std::move(diagnostic));
                } else
                {
                    selectedProvider.emplace(requirement, providers.front());
                }
            }
        }
    }

    // Selections that name a capability nobody provides - neither exactly nor
    // through a family member.
    for (const ProviderSelection &selection : selections_)
    {
        if (candidateProviders(selection.capabilityName).empty())
        {
            ResolutionDiagnostic diagnostic;
            diagnostic.kind    = ResolutionDiagnostic::Kind::UnknownSelectedProvider;
            diagnostic.message = "configuration selects '" + selection.providerName + "' for capability '"
                               + selection.capabilityName + "' but no component provides that capability.";
            result.diagnostics.push_back(std::move(diagnostic));
        }
    }

    // A broken selection phase already carries the root cause (an ambiguity
    // the configuration must settle, or a selection nobody can satisfy). The
    // edge phase below would only repeat that failure per consumer - one
    // precise diagnostic per broken startup means the resolver reports the
    // root cause and stops (docs/architecture.md, the resolution contract).
    if (!result.diagnostics.empty())
    {
        result.successful = false;
        return result;
    }

    // Build consumer -> provider edges. Each edge is (providerIndex, consumerIndex).
    //
    // The capability directions are enforced here, at the only place that
    // turns declarations into graph edges (docs/architecture.md,
    // "Capability directions"): a MODULE consumer never sees a mod provider.
    // For a required capability that resolves to a mod (the only providers are
    // mods, or the [providers] selection names one), the violation is a
    // precise startup error. For an optional capability the mod providers are
    // filtered out - the consumer orders after the module members only, and
    // an optional capability with no module member simply stays unavailable
    // (the same degrade-not-fail semantics a missing provider has), so a
    // family extension point filled by mods keeps working for every mod
    // consumer while modules quietly ignore the members they must not see.
    std::vector<std::pair<ManifestIndex, ManifestIndex>> edges;
    for (ManifestIndex consumerIndex = 0; consumerIndex < manifestCount; ++consumerIndex)
    {
        const ComponentManifest &consumer = manifests[consumerIndex];
        const bool consumerIsModule       = consumer.kind == ComponentKind::Module;

        for (const std::string &capabilityName : consumer.requiredCapabilities)
        {
            const auto provider = selectedProvider.find(capabilityName);
            if (provider == selectedProvider.end())
            {
                std::string availableList;
                for (const auto &entry : capabilityProviders)
                {
                    if (!availableList.empty())
                    {
                        availableList += ", ";
                    }
                    availableList += entry.first;
                }
                ResolutionDiagnostic diagnostic;
                diagnostic.kind    = ResolutionDiagnostic::Kind::UnknownCapability;
                diagnostic.message = "component '" + consumer.name + "' requires capability '" + capabilityName
                                   + "' but no available component provides it."
                                   + (availableList.empty() ? std::string(" no capabilities are provided at all.")
                                                            : " available capabilities: " + availableList + ".");
                diagnostic.involvementChain.push_back(consumer.name);
                diagnostic.involvementChain.push_back(capabilityName);
                result.diagnostics.push_back(std::move(diagnostic));
                continue;
            }
            if (consumerIsModule && manifests[provider->second].kind != ComponentKind::Module)
            {
                ResolutionDiagnostic diagnostic;
                diagnostic.kind    = ResolutionDiagnostic::Kind::ModuleModCapability;
                diagnostic.message = "module '" + consumer.name + "' requires capability '" + capabilityName
                                   + "' whose resolved provider is the mod '" + manifests[provider->second].name
                                   + "'; modules can only use capabilities provided by other modules."
                                     " Move the capability's provider into a module, or consume it from a mod.";
                diagnostic.involvementChain.push_back(consumer.name);
                diagnostic.involvementChain.push_back(capabilityName);
                diagnostic.involvementChain.push_back(manifests[provider->second].name);
                result.diagnostics.push_back(std::move(diagnostic));
                continue;
            }
            if (provider->second != consumerIndex)
            {
                edges.emplace_back(provider->second, consumerIndex);
            }
        }

        // Optional capabilities add the SAME ordering edge when a provider
        // resolved (the consumer will acquire the implementation during its
        // construct phase, so the provider must exist by then), and resolve
        // to plain "not available" when none did: a missing entry in
        // selectedProvider means zero providers, which is the valid optional
        // outcome - never a diagnostic. A capability declared both required
        // and optional keeps only the required edge. An optional requirement
        // that resolved to SEVERAL members without a selection is the
        // extension-point shape (see the selection phase): the consumer
        // orders after EVERY member, which is exactly what the plural
        // acquisition at runtime expects.
        for (const std::string &capabilityName : consumer.optionalCapabilities)
        {
            const bool alsoRequired
                = std::find(consumer.requiredCapabilities.begin(), consumer.requiredCapabilities.end(), capabilityName)
               != consumer.requiredCapabilities.end();
            if (alsoRequired)
            {
                continue;
            }
            const auto provider = selectedProvider.find(capabilityName);
            if (provider == selectedProvider.end())
            {
                const auto extension = extensionProviders.find(capabilityName);
                if (extension != extensionProviders.end())
                {
                    for (const ManifestIndex providerIndex : extension->second)
                    {
                        // A module consumer never orders after (and never
                        // acquires from) a mod provider: the direction rule
                        // filters the extension point's members per consumer.
                        if (providerIndex != consumerIndex
                            && !(consumerIsModule && manifests[providerIndex].kind != ComponentKind::Module))
                        {
                            edges.emplace_back(providerIndex, consumerIndex);
                        }
                    }
                }
                continue;
            }
            // An optional capability that resolved to a mod provider is the
            // same filtered shape for a module consumer: no edge, no
            // acquisition, the capability reads as unavailable at construct
            // time - without failing the run (optional semantics).
            if (consumerIsModule && manifests[provider->second].kind != ComponentKind::Module)
            {
                continue;
            }
            if (provider->second != consumerIndex)
            {
                edges.emplace_back(provider->second, consumerIndex);
            }
        }

        for (const std::string &componentName : consumer.requiredComponents)
        {
            const auto required = componentIndices.find(componentName);
            if (required == componentIndices.end())
            {
                ResolutionDiagnostic diagnostic;
                diagnostic.kind    = ResolutionDiagnostic::Kind::UnknownComponent;
                diagnostic.message = "component '" + consumer.name + "' requires component '" + componentName
                                   + "' but no component with that name is available.";
                diagnostic.involvementChain.push_back(consumer.name);
                diagnostic.involvementChain.push_back(componentName);
                result.diagnostics.push_back(std::move(diagnostic));
                continue;
            }
            if (required->second != consumerIndex)
            {
                edges.emplace_back(required->second, consumerIndex);
            }
        }
    }

    // Deterministic Kahn topological ordering. Duplicate edges collapse through
    // the edge set so shared dependencies are counted once.
    std::set<std::pair<ManifestIndex, ManifestIndex>> uniqueEdges(edges.begin(), edges.end());
    std::vector<ManifestIndex> inDegree(manifestCount, 0);
    std::vector<std::vector<ManifestIndex>> dependents(manifestCount);
    for (const auto &edge : uniqueEdges)
    {
        dependents[edge.first].push_back(edge.second);
        ++inDegree[edge.second];
    }

    std::vector<ManifestIndex> readyQueue;
    for (ManifestIndex index = 0; index < manifestCount; ++index)
    {
        if (inDegree[index] == 0)
        {
            readyQueue.push_back(index);
        }
    }

    std::vector<ManifestIndex> order;
    order.reserve(manifestCount);
    // FIFO pops through a head cursor: the queue only ever grows, so a front
    // erase (an O(n) shift per pop, O(n^2) over the loop) is replaced by one
    // advancing index. Identical visit order - the vector holds the same
    // elements in the same positions; the cursor just stops re-shifting them.
    std::size_t readyCursor = 0;
    while (readyCursor < readyQueue.size())
    {
        const ManifestIndex current = readyQueue[readyCursor++];
        order.push_back(current);
        for (ManifestIndex dependent : dependents[current])
        {
            --inDegree[dependent];
            if (inDegree[dependent] == 0)
            {
                readyQueue.push_back(dependent);
            }
        }
    }

    if (order.size() != manifestCount)
    {
        // A cycle exists among the unordered components. Run a depth first
        // search over the adjacency list until a back edge is found, then
        // report every participant on that cycle path.
        std::vector<std::vector<ManifestIndex>> adjacency(manifestCount);
        for (const auto &edge : uniqueEdges)
        {
            adjacency[edge.first].push_back(edge.second);
        }

        std::vector<VisitState> visitState(manifestCount, VisitState::Unvisited);
        std::vector<ManifestIndex> searchPath;
        std::vector<ManifestIndex> cycleNodes;

        const std::function<bool(ManifestIndex)> searchCycle = [&](ManifestIndex node) -> bool {
            visitState[node] = VisitState::InProgress;
            searchPath.push_back(node);
            for (ManifestIndex next : adjacency[node])
            {
                if (visitState[next] == VisitState::InProgress)
                {
                    const auto start = std::find(searchPath.begin(), searchPath.end(), next);
                    cycleNodes.assign(start, searchPath.end());
                    return true;
                }
                if (visitState[next] == VisitState::Unvisited && searchCycle(next))
                {
                    return true;
                }
                // A Visited next needs no action: its subtree already
                // proved cycle-free, so re-walking it would only duplicate
                // work (and an InProgress next was handled above).
            }
            searchPath.pop_back();
            // Fully processed without finding a cycle: mark Visited so a
            // later search root neither re-walks this subtree (wasted work)
            // nor mistakes the stale InProgress for a back edge into a path
            // it is not on - the bug that turned a precise cycle chain into
            // the generic "among the available components" message.
            visitState[node] = VisitState::Visited;
            return false;
        };

        for (ManifestIndex index = 0; index < manifestCount && cycleNodes.empty(); ++index)
        {
            if (visitState[index] == VisitState::Unvisited)
            {
                searchCycle(index);
            }
        }

        ResolutionDiagnostic diagnostic;
        diagnostic.kind = ResolutionDiagnostic::Kind::CyclicDependency;
        if (!cycleNodes.empty())
        {
            // cycleNodes holds the cycle path [entry, ..., current]; entry is
            // the node the back edge returns to, so closing the chain with the
            // entry name shows the full loop.
            std::string chain;
            for (ManifestIndex node : cycleNodes)
            {
                if (!chain.empty())
                {
                    chain += " -> ";
                }
                chain += manifests[node].name;
            }
            chain += " -> " + manifests[cycleNodes.front()].name;
            diagnostic.message = "dependency cycle detected: " + chain;
            for (ManifestIndex node : cycleNodes)
            {
                diagnostic.involvementChain.push_back(manifests[node].name);
            }
        } else
        {
            diagnostic.message = "dependency cycle detected among the available components.";
        }
        result.diagnostics.push_back(std::move(diagnostic));
        result.successful = false;
        return result;
    }

    result.constructionOrder.reserve(manifestCount);
    for (ManifestIndex index : order)
    {
        result.constructionOrder.push_back(index);
    }
    result.successful = result.diagnostics.empty();
    return result;
}

} // namespace engine
