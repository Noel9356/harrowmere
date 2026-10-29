#ifndef ENGINE_KERNEL_MODULE_API_HPP
#define ENGINE_KERNEL_MODULE_API_HPP

#include "engine/kernel/component_interface.hpp"

#include <cstdint>

namespace engine
{

// ABI version of the module plugin entry point. The kernel refuses any plugin
// whose reported version differs: a rebuilt kernel combined with stale plugin
// binaries from an older interface must fail with a precise diagnostic, never
// crash inside a mismatched vtable.
//
// Version 2 added the static capability declaration (providedCapabilities /
// requiredCapabilities): the kernel resolves the capability demand closure
// from this metadata BEFORE instantiating anything, so a module whose
// capabilities nobody requires is never created and its binary is released.
// Version 3 added optionalCapabilities: capabilities the module uses when a
// provider is active and functions without otherwise (the resolver orders the
// provider first and the manifest must agree with the descriptor, like the
// required set).
//
// Version 4 grew the kernel-side consumer surface the plugin binaries
// compile against: priority-ordered event subscriptions (EventChannel's
// handler layout changed), the multi-provider acquisition
// (ComponentContext::acquireEachCapability / CapabilityResolver::
// resolveEachCapabilityPointer), the worker-pool form of
// AsyncTaskDispatcher::start and the ConfigurationReloadedEvent the kernel
// publishes. A stale plugin binary from interface 3 must fail with a precise
// version diagnostic, never crash inside a mismatched vtable.
//
// Version 5 grew the surfaces again: EventChannel and EventBus gained the
// deferred-delivery members (publishDeferred / flushDeferred / the worker
// handoff queues), which changes the layout of every channel a plugin
// instantiates; CapabilityResolver gained tryResolveCapabilityPointer and
// ComponentContext gained the main-thread scheduler reference behind
// scheduler() and the non-throwing tryAcquireCapability. A stale plugin
// binary from interface 4 would read the old channel layout through the new
// kernel - the exact mismatched-layout crash the version gate exists to
// refuse.
//
// Version 6 grew ComponentManifest by the version member. The kernel copies
// manifests out of plugin binaries (record.manifest = instance->manifest()),
// so the struct's layout crosses the library boundary even though nothing
// virtual sits in it: a plugin binary from interface 5 would construct
// manifests without the member and the kernel would read past their end. The
// member itself is informational identity, but the layout change is a
// rebuild-everything change, and the gate is what turns it into a precise
// diagnostic instead of a silent over-read.
//
// Version 7 grew THIS descriptor by the declaredPermissions pair: the
// environment-scope permissions (anything outside the capability.* namespace,
// such as filesystem.write) the module's manifest requests. The kernel reads
// them BEFORE instantiation to risk-assess the module for the user - the
// trust gate must see what a module can reach outside the capability system
// before any of its code is agreed to (docs/architecture.md, "Risk advisories
// and the trust gate"). Capability-scope grants stay derived from the
// capability lists and are not declared here. A stale plugin binary from
// interface 6 would answer the descriptor query with a shorter struct; the
// version gate refuses it with a precise diagnostic instead of reading past
// its end.
inline constexpr std::uint32_t engineModulePluginAbiVersion = 7u;

// The descriptor every module plugin returns from its single exported entry
// point. Plain data and plain function pointers: nothing that crosses the
// library boundary assumes more than one heap for the whole process (one CRT
// on Windows, a shared libstdc++ on POSIX), which this build guarantees.
//
// The capability lists are fixed-size arrays of literals paired with an
// explicit count, not std::vector or std::span, on purpose. The kernel reads
// this object straight out of the plugin binary's static storage: a container
// member would put the shared struct layout under STL implementation details,
// would allocate and free across the boundary, and would give the descriptor
// dynamic construction plus destruction-order state at unload. The count of
// each array is derived in module_plugin.cpp with std::size(array) - the same
// "always matches the container" property as vector::size() - so a count can
// never silently drift from the array it describes.
struct EngineModulePluginEntry
{
    // Must equal engineModulePluginAbiVersion at the time of loading.
    std::uint32_t abiVersion;
    // The module's own name, exactly what the module's manifest reports.
    const char *moduleName;
    // Creates one module instance. The kernel owns the returned object and
    // releases it with delete: one process, one heap.
    ModuleInterface *(*createModule)();
    // Static capability declaration, read before any instantiation. Each
    // array holds plain string literals with static storage duration (or is
    // null when the count is zero). The kernel verifies, after creating an
    // instance, that the instance manifest agrees with this declaration: a
    // mismatch is a precise startup problem, never silent drift.
    const char *const *providedCapabilities;
    std::uint32_t providedCapabilityCount;
    const char *const *requiredCapabilities;
    std::uint32_t requiredCapabilityCount;
    // Capabilities used when a provider is active, never demanded: a missing
    // provider is a valid state. Same agreement rule as the required set.
    const char *const *optionalCapabilities;
    std::uint32_t optionalCapabilityCount;
    // The module's ENVIRONMENT-scope permission requests: every manifest
    // permission outside the capability.* namespace (filesystem.write and
    // friends), one literal per entry, null/zero when the module requests
    // none. Declared statically so the kernel can risk-assess the module -
    // and ask the user to agree to it - BEFORE the module's binary is
    // instantiated (the capability-scope grants need no declaration here:
    // they derive from the capability lists above). The kernel verifies,
    // after creating an instance, that the manifest's environment-scope
    // permissions agree with this declaration; a mismatch is a precise
    // startup problem, never silent drift.
    const char *const *declaredPermissions;
    std::uint32_t declaredPermissionCount;
};

} // namespace engine

// The one exported symbol of a module plugin. Every module binary defines it
// through its module_plugin.cpp translation unit; the kernel resolves it by
// name after dlopen and never links a plugin's import library.
#if defined(_WIN32)
#define ENGINE_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define ENGINE_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Returns the static plugin descriptor of this module binary. The pointed-to
// object must have static storage duration.
ENGINE_MODULE_EXPORT const engine::EngineModulePluginEntry *engineQueryModulePlugin();

#endif // ENGINE_KERNEL_MODULE_API_HPP
