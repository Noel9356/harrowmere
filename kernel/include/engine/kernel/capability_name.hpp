#ifndef ENGINE_KERNEL_CAPABILITY_NAME_HPP
#define ENGINE_KERNEL_CAPABILITY_NAME_HPP

#include <string>

namespace engine
{

// Capability name mechanism. A capability name is either a family name
// ("example" - implementation agnostic) or a family member name
// ("example.impl" - one implementation of that family, named after the
// implementation). The dot separates the two; a member's family is everything
// before its LAST dot, so members may nest one level deep without ambiguity.
//
// This is string mechanism only: the kernel never interprets what any family
// or member means. The semantics of a capability live entirely in the
// providing module's published header and its consumers.

// Whether the name is bound to one implementation ("family.member").
inline bool isImplementationBoundCapabilityName(
    const std::string &capabilityName
)
{
    return capabilityName.find('.') != std::string::npos;
}

// The family a capability name belongs to: the name itself for family names,
// everything before the last dot for member names.
inline std::string capabilityFamilyName(
    const std::string &capabilityName
)
{
    const std::size_t lastDot = capabilityName.rfind('.');
    if (lastDot == std::string::npos)
    {
        return capabilityName;
    }
    return capabilityName.substr(0, lastDot);
}

// Whether a provided member name satisfies a consumer's requirement. A
// requirement matches exactly ("example.impl" needs exactly
// "example.impl"), and a family requirement ("example") is satisfied by
// any member of that family ("example.impl") or by an exact provider of
// the family name itself.
//
// The family test is a parent-of test, not a dot-free test: a member's
// family is everything before its LAST dot, so a requirement may itself
// carry dots and still be a family ("scripting.lua_bridge" is the nested
// family of "scripting.lua_bridge.scene", exactly the nesting the name
// mechanism documents above). The old guard refused every dotted
// requirement outright, which severed exactly the two-level families the
// bridge extension system is built on: the "scripting.lua_bridge"
// requirement resolved to zero members, no bridge ever installed, and the
// demo mod's engine.spawn_object (and every other module bridge) was nil.
// An implementation-bound requirement that also exact-matches still wins
// through the exact pass first; the parent-of test only widens the
// member fallback to names the mechanism already calls families.
//
// Allocation-free by construction: the family comparison inspects the
// provided name in place (its last-dot position must land exactly at the
// requirement's length, and the prefix must compare equal) instead of
// materialising the family with substr. The test sits inside
// classifyCapabilityProviders, which walks every constructed component's
// provided names on every capability acquisition - the lazy acquisitions
// components make at frame time included - so a per-name temporary there
// would put a heap round trip on the engine's one cross-component call
// path for every declaration that is not the exact answer. The result is
// byte-identical to capabilityFamilyName(provided) == requirement; only
// the allocation is gone.
inline bool capabilityRequirementSatisfiedBy(
    const std::string &requirement, const std::string &provided
)
{
    if (requirement == provided)
    {
        return true;
    }
    const std::size_t lastDot = provided.rfind('.');
    if (lastDot == std::string::npos || lastDot != requirement.size())
    {
        return false;
    }
    return provided.compare(0, lastDot, requirement) == 0;
}

} // namespace engine

#endif // ENGINE_KERNEL_CAPABILITY_NAME_HPP
