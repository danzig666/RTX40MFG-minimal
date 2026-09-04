#include "ngx.h"
#include "ada_patch.h"
#include "log.h"

#include <d3d12.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <string>

namespace ngx
{
namespace
{
// Minimal mirror of the NGX D3D12 entry this mod touches. Declaring it here
// keeps the build dependent on the Streamline SDK headers alone, rather than
// also requiring NVIDIA's NGX SDK submodule.
//
// NVSDK_NGX_Feature_FrameGeneration is 11 in NVIDIA's public feature enum.
constexpr uint32_t kFeatureFrameGeneration = 11;

using NgxCreateFeatureFn = int (*)(void* commandList, uint32_t feature,
    void* parameters, void** outHandle);

std::atomic<NgxCreateFeatureFn> gOriginalCreateFeature{nullptr};
std::atomic<HMODULE> gProvider{nullptr};
std::wstring gProviderPath;
std::atomic<bool> gHookInstalled{false};
std::atomic<uint64_t> gFrameGenerationCreates{0};
std::once_flag gPrepareOnce;

// Resolves the D3D12 device that owns the command list NGX was handed. This
// is the adapter frame generation will actually run on, so it is also the
// adapter the Ada check must apply to.
bool AdapterFromCommandList(void* commandList) noexcept
{
    if (!commandList)
        return false;
    ID3D12Device* device = nullptr;
    bool observed = false;
    __try
    {
        auto* object = static_cast<ID3D12DeviceChild*>(commandList);
        if (SUCCEEDED(object->GetDevice(__uuidof(ID3D12Device),
                reinterpret_cast<void**>(&device)))
            && device)
        {
            observed = ada_patch::ObserveD3D12Device(device);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        observed = false;
    }
    if (device)
        device->Release();
    return observed;
}

// Runs once, immediately before NVIDIA builds the frame generation pipeline.
void PrepareProvider(void* commandList) noexcept
{
    std::call_once(gPrepareOnce, [commandList]() noexcept {
        HMODULE provider = gProvider.load(std::memory_order_acquire);
        const wchar_t* path = gProviderPath.empty()
            ? L"(unknown)" : gProviderPath.c_str();
        if (!AdapterFromCommandList(commandList))
        {
            const uint32_t code = ada_patch::FailureCode();
            mfglog::Write(L"Ada verification failed at CreateFeature: "
                L"failure=%u (%s); leaving the provider untouched",
                code, ada_patch::FailureName(code));
            return;
        }
        const bool patched = ada_patch::PatchProvider(provider, path);
        const uint32_t code = ada_patch::FailureCode();
        mfglog::Write(L"Ada temporal patch: ready=%d failure=%u (%s): %s",
            patched, code, ada_patch::FailureName(code), path);
    });
}

int HookCreateFeature(void* commandList, uint32_t feature,
    void* parameters, void** outHandle)
{
    auto* original = gOriginalCreateFeature.load(std::memory_order_acquire);
    if (!original)
        return -1;

    // The provider also carries DLSS Super Resolution and other feature
    // traffic. Those calls must reach NVIDIA completely untouched.
    if (feature == kFeatureFrameGeneration)
    {
        const uint64_t call =
            gFrameGenerationCreates.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 1)
            mfglog::Write(L"NGX CreateFeature(FrameGeneration) intercepted");
        PrepareProvider(commandList);
    }
    return original(commandList, feature, parameters, outHandle);
}
}

bool InstallCreateFeatureHook(HMODULE provider, const wchar_t* path) noexcept
{
    if (!provider || gHookInstalled.load(std::memory_order_acquire))
        return false;

    void* target = reinterpret_cast<void*>(
        GetProcAddress(provider, "NVSDK_NGX_D3D12_CreateFeature"));
    if (!target)
        return false;

    void* original = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookCreateFeature),
            &original) != MH_OK)
    {
        mfglog::Write(L"NGX CreateFeature hook failed: %s", path);
        return false;
    }
    // Everything the detour reads has to be in place before it goes live.
    gProviderPath = path ? path : L"";
    gProvider.store(provider, std::memory_order_release);
    gOriginalCreateFeature.store(
        reinterpret_cast<NgxCreateFeatureFn>(original),
        std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK)
    {
        gOriginalCreateFeature.store(nullptr, std::memory_order_release);
        MH_RemoveHook(target);
        mfglog::Write(L"NGX CreateFeature hook failed to enable: %s", path);
        return false;
    }
    gHookInstalled.store(true, std::memory_order_release);
    mfglog::Write(L"NGX CreateFeature hooked: %s", path);
    return true;
}
}
