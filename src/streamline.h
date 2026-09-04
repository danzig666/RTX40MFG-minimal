#pragma once

#include <Windows.h>

#include <cstdint>

namespace streamline
{
// Intercepts sl.interposer.dll's slGetFeatureFunction so the DLSS-G options
// setter the game resolves is ours. Idempotent per module.
bool InstallInterposerHooks(HMODULE interposer, const wchar_t* path) noexcept;

// Records the native ceiling reported by the patched wrapper clamp so the
// requested multiplier can be held to something the wrapper understands.
void SetWrapperCompiledMaximum(uint32_t compiledMaximum) noexcept;

// The multiplier actually being requested, after clamping.
uint32_t EffectiveMultiplier() noexcept;
}
