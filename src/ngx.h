#pragma once

#include <Windows.h>

namespace ngx
{
// Detours NVSDK_NGX_D3D12_CreateFeature in the DLSS-G provider. The Ada
// temporal correction has to be published before NVIDIA builds the frame
// generation pipeline, and this is the last point where that is possible.
bool InstallCreateFeatureHook(HMODULE provider, const wchar_t* path) noexcept;
}
