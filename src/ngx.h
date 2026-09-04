#pragma once

#include <Windows.h>

namespace ngx
{
// Detours the DLSS-G provider's CreateFeature entries -- D3D12, Vulkan and
// Vulkan1 -- installing whichever the provider exports. The Ada temporal
// correction has to be published before NVIDIA builds the frame generation
// pipeline, and these are the last points where that is possible.
bool InstallCreateFeatureHooks(HMODULE provider, const wchar_t* path) noexcept;
}
