#pragma once

#include <cstdint>

#include "harrowmere_capability.hpp"

namespace harrowmere_fixture_mod
{

struct Readback
{
    std::uint64_t tick      = 0;
    std::uint64_t territory = 0;
    std::uint32_t level     = 0;
};

// This intentionally depends only on the published capability contract.
Readback readCoreViews(const harrowmere::SimCapability &core);

} // namespace harrowmere_fixture_mod
