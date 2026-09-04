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
    // 0 = follow the game's own request. Once the arch gates raise the
    // advertised maximum, a game's menu may expose 2x..6x itself; that
    // choice should be the player's, not overridden here.
    uint32_t multiplier = 0;
    bool log = true;
    // The device-support NOP inherited from upstream. It matches on some
    // providers but is not what raises the frame count, and its exact effect
    // is undocumented -- opt-in only.
    bool legacyNgxPatch = false;
    // Diagnostic: 0 skips the Ada temporal correction entirely. Generation
    // then uses NVIDIA's stock Ada kernel (midpoint artifacts at 3x+), which
    // isolates whether the rebuilt kernel is what stops frames appearing.
    bool adaTemporalPatch = true;
};

// Reads RTX40MFG.ini beside the executable. Missing file yields defaults.
Settings Load(const wchar_t* executableDirectory) noexcept;
}
