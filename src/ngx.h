#pragma once

#include <Windows.h>

namespace ngx
{
// Detours the DLSS-G provider's CreateFeature entries -- D3D12, Vulkan and
// Vulkan1 -- installing whichever the provider exports. The Ada temporal
// correction has to be published before NVIDIA builds the frame generation
// pipeline, and these are the last points where that is possible.
bool InstallCreateFeatureHooks(HMODULE provider, const wchar_t* path) noexcept;

// Diagnostic switch: false leaves NVIDIA's stock Ada kernel in place, so a
// generation failure can be attributed to the rebuilt kernel or ruled out.
void SetApplyTemporalPatch(bool apply) noexcept;
}
