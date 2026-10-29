#ifndef ENGINE_KERNEL_VERSION_HPP
#define ENGINE_KERNEL_VERSION_HPP

#include <cstdint>

namespace engine
{

// The engine kernel's version, reported by the app banner (--version and the
// startup line) and mirrored by the Vulkan instance identity (the renderer's
// vulkanInstanceVersion) and the root CMakeLists project() VERSION - the
// three are one product version and change together.
//
// Components carry versions of their own, one step down from this one:
// every module reports its version through ComponentManifest::version (the
// manifest the module's instance hands the kernel), every mod package
// through mod.toml's `version` key, and the component graph report prints
// them next to each component. The product version above is the kernel's;
// these are the parts - they move on their own schedules.
inline constexpr std::uint32_t engineVersionMajor = 0u;
inline constexpr std::uint32_t engineVersionMinor = 1u;
inline constexpr std::uint32_t engineVersionPatch = 0u;
inline constexpr const char *engineVersionString  = "0.1.0";

} // namespace engine

#endif // ENGINE_KERNEL_VERSION_HPP
