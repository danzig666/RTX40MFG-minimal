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
// Minimal mirror of the NGX entries this mod touches. Declaring them here
// keeps the build dependent on the Streamline SDK headers alone, rather than
// also requiring NVIDIA's NGX SDK.
//
// Verified against the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS, NGX API 1.5.0,
// commit a291cc7), headers nvsdk_ngx_defs.h / nvsdk_ngx.h / nvsdk_ngx_vk.h:
//
//   NVSDK_NGX_Feature_FrameGeneration = 11
//   NVSDK_NGX_Result_Success          = 0x1
//   NVSDK_NGX_Result_Fail             = 0xBAD00000
//   NVSDK_CONV                        = __cdecl
//
//   NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature(
//       ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
//       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
//   NVSDK_NGX_Result NVSDK_NGX_VULKAN_CreateFeature(
//       VkCommandBuffer, NVSDK_NGX_Feature,
//       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
//   NVSDK_NGX_Result NVSDK_NGX_VULKAN_CreateFeature1(
//       VkDevice, VkCommandBuffer, NVSDK_NGX_Feature,
//       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
//
// NVSDK_NGX_Result carries values above INT_MAX, so it is an unsigned enum.
using NgxResult = uint32_t;
constexpr uint32_t kFeatureFrameGeneration = 11;
constexpr NgxResult kNgxResultFail = 0xBAD00000u;

using NgxD3D12CreateFeatureFn = NgxResult (*)(void* commandList,
    uint32_t feature, void* parameters, void** outHandle);
using NgxVkCreateFeatureFn = NgxResult (*)(void* commandBuffer,
    uint32_t feature, void* parameters, void** outHandle);
// CreateFeature1 prepends the VkDevice.
using NgxVkCreateFeature1Fn = NgxResult (*)(void* device, void* commandBuffer,
    uint32_t feature, void* parameters, void** outHandle);

std::atomic<NgxD3D12CreateFeatureFn> gOriginalD3D12Create{nullptr};
std::atomic<NgxVkCreateFeatureFn> gOriginalVkCreate{nullptr};
std::atomic<NgxVkCreateFeature1Fn> gOriginalVkCreate1{nullptr};

std::atomic<HMODULE> gProvider{nullptr};
std::wstring gProviderPath;
std::atomic<bool> gHookInstalled{false};
std::atomic<uint64_t> gFrameGenerationCreates{0};
std::once_flag gPrepareOnce;

// Resolves the D3D12 device that owns the command list NGX was handed. That
// is the adapter frame generation will actually run on.
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
//
// `d3d12CommandList` is null on the Vulkan routes: a VkCommandBuffer carries
// no back-pointer to its device, so there the adapter must already have been
// verified from slSetVulkanInfo.
void PrepareProvider(void* d3d12CommandList, const wchar_t* api) noexcept
{
    std::call_once(gPrepareOnce, [d3d12CommandList, api]() noexcept {
        HMODULE provider = gProvider.load(std::memory_order_acquire);
        const wchar_t* path = gProviderPath.empty()
            ? L"(unknown)" : gProviderPath.c_str();

        // D3D12 is authoritative when we have the command list; otherwise
        // fall back to whatever slSetVulkanInfo established.
        bool adapter = false;
        if (d3d12CommandList)
            adapter = AdapterFromCommandList(d3d12CommandList);
        if (!adapter)
            adapter = ada_patch::AdapterVerified();

        if (!adapter)
        {
            const uint32_t code = ada_patch::FailureCode();
            mfglog::Write(L"%s: Ada verification failed at CreateFeature: "
                L"failure=%u (%s); leaving the provider untouched",
                api, code, ada_patch::FailureName(code));
            if (!d3d12CommandList)
            {
                mfglog::Write(L"  On Vulkan the adapter comes from "
                    L"slSetVulkanInfo. If that hook never ran, the game did "
                    L"not route Vulkan setup through Streamline.");
            }
            return;
        }

        const bool patched = ada_patch::PatchProvider(provider, path);
        const uint32_t code = ada_patch::FailureCode();
        mfglog::Write(L"%s: Ada temporal patch: ready=%d failure=%u (%s): %s",
            api, patched, code, ada_patch::FailureName(code), path);
    });
}

void NoteFrameGenerationCreate(const wchar_t* api) noexcept
{
    const uint64_t call =
        gFrameGenerationCreates.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call == 1)
        mfglog::Write(L"%s: CreateFeature(FrameGeneration) intercepted", api);
}

NgxResult HookD3D12CreateFeature(void* commandList, uint32_t feature,
    void* parameters, void** outHandle)
{
    auto* original = gOriginalD3D12Create.load(std::memory_order_acquire);
    if (!original)
        return kNgxResultFail;
    // The provider also carries DLSS Super Resolution and other feature
    // traffic. Those calls must reach NVIDIA completely untouched.
    if (feature == kFeatureFrameGeneration)
    {
        NoteFrameGenerationCreate(L"NGX D3D12");
        PrepareProvider(commandList, L"NGX D3D12");
    }
    return original(commandList, feature, parameters, outHandle);
}

NgxResult HookVkCreateFeature(void* commandBuffer, uint32_t feature,
    void* parameters, void** outHandle)
{
    auto* original = gOriginalVkCreate.load(std::memory_order_acquire);
    if (!original)
        return kNgxResultFail;
    if (feature == kFeatureFrameGeneration)
    {
        NoteFrameGenerationCreate(L"NGX Vulkan");
        PrepareProvider(nullptr, L"NGX Vulkan");
    }
    return original(commandBuffer, feature, parameters, outHandle);
}

NgxResult HookVkCreateFeature1(void* device, void* commandBuffer,
    uint32_t feature, void* parameters, void** outHandle)
{
    auto* original = gOriginalVkCreate1.load(std::memory_order_acquire);
    if (!original)
        return kNgxResultFail;
    if (feature == kFeatureFrameGeneration)
    {
        NoteFrameGenerationCreate(L"NGX Vulkan1");
        PrepareProvider(nullptr, L"NGX Vulkan1");
    }
    return original(device, commandBuffer, feature, parameters, outHandle);
}

// Installs one detour, publishing the trampoline before the hook goes live so
// a call landing in between still reaches the original function.
template <typename Fn>
bool InstallEntry(HMODULE module, const char* name, void* replacement,
    std::atomic<Fn>& published, const wchar_t* label, const wchar_t* path)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target)
        return false;
    void* original = nullptr;
    if (MH_CreateHook(target, replacement, &original) != MH_OK)
    {
        mfglog::Write(L"%s hook failed: %s", label, path);
        return false;
    }
    published.store(reinterpret_cast<Fn>(original), std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK)
    {
        published.store(nullptr, std::memory_order_release);
        MH_RemoveHook(target);
        mfglog::Write(L"%s hook failed to enable: %s", label, path);
        return false;
    }
    mfglog::Write(L"%s hooked: %s", label, path);
    return true;
}
}

bool InstallCreateFeatureHooks(HMODULE provider, const wchar_t* path) noexcept
{
    if (!provider || gHookInstalled.load(std::memory_order_acquire))
        return false;

    // Everything the detours read has to be in place before any goes live.
    gProviderPath = path ? path : L"";
    gProvider.store(provider, std::memory_order_release);

    const bool d3d12 = InstallEntry(provider, "NVSDK_NGX_D3D12_CreateFeature",
        reinterpret_cast<void*>(&HookD3D12CreateFeature), gOriginalD3D12Create,
        L"NGX D3D12 CreateFeature", path);
    const bool vulkan = InstallEntry(provider, "NVSDK_NGX_VULKAN_CreateFeature",
        reinterpret_cast<void*>(&HookVkCreateFeature), gOriginalVkCreate,
        L"NGX Vulkan CreateFeature", path);
    const bool vulkan1 = InstallEntry(provider,
        "NVSDK_NGX_VULKAN_CreateFeature1",
        reinterpret_cast<void*>(&HookVkCreateFeature1), gOriginalVkCreate1,
        L"NGX Vulkan CreateFeature1", path);

    if (!d3d12 && !vulkan && !vulkan1)
    {
        gProvider.store(nullptr, std::memory_order_release);
        mfglog::Write(L"No NGX CreateFeature entry could be hooked: %s", path);
        return false;
    }
    gHookInstalled.store(true, std::memory_order_release);
    mfglog::Write(L"NGX entries hooked: d3d12=%d vulkan=%d vulkan1=%d",
        d3d12, vulkan, vulkan1);
    return true;
}
}
