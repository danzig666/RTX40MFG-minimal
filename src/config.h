#pragma once

#include <cstdint>

namespace config
{
// The first release deliberately stops at 4x. 5x/6x add compatibility and
// pacing questions without teaching anything about the mechanism.
inline constexpr uint32_t kMinimumMultiplier = 2;
inline constexpr uint32_t kMaximumMultiplier = 4;

struct Settings
{
    uint32_t multiplier = kMinimumMultiplier;
    bool log = true;
};

// Reads RTX40MFG.ini beside the executable. Missing file yields defaults.
Settings Load(const wchar_t* executableDirectory) noexcept;
}
