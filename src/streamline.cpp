#include "streamline.h"
#include "ada_patch.h"
#include "config.h"
#include "log.h"
#include "patches.h"

#include <sl.h>
#include <sl_dlss_g.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

extern std::atomic<uint32_t> gRequestedMultiplier;

namespace streamline
{
namespace
{
std::atomic<PFun_slGetFeatureFunction*> gOriginalGetFeatureFunction{nullptr};
std::atomic<PFun_slDLSSGSetOptions*> gOriginalSetOptions{nullptr};
std::atomic<uint32_t> gWrapperCompiledMaximum{0};
std::atomic<bool> gInterposerHooked{false};
std::atomic<uint64_t> gSetOptionsCalls{0};
std::atomic<uint64_t> gRejectedRequests{0};
std::atomic<PFun_slDLSSGGetState*> gGetState{nullptr};

// Stable pointer-sized prefix of sl::VulkanInfo v1-v3. Mirrored here so the
// core needs no Vulkan SDK header; the fixed ABI offsets are asserted below.
struct VulkanInfoPrefix
{
    sl::BaseStructure* next = nullptr;
    sl::StructType structType{};
    size_t structVersion = 0;
    void* device = nullptr;
    void* instance = nullptr;
    void* physicalDevice = nullptr;
};
static_assert(offsetof(VulkanInfoPrefix, device) == 32);
static_assert(offsetof(VulkanInfoPrefix, physicalDevice) == 48);

using PFun_slSetVulkanInfoAbi = sl::Result(const VulkanInfoPrefix& info);
std::atomic<PFun_slSetVulkanInfoAbi*> gOriginalSetVulkanInfo{nullptr};

// A VkCommandBuffer has no route back to its VkPhysicalDevice, so this is the
// only point where the Vulkan adapter can be identified before NGX builds the
// frame generation pipeline.
sl::Result HookSetVulkanInfo(const VulkanInfoPrefix& info)
{
    auto* original = gOriginalSetVulkanInfo.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    void* physicalDevice = nullptr;
    __try
    {
        if (info.structVersion >= sl::kStructVersion1
            && info.structVersion <= sl::kStructVersion3)
            physicalDevice = info.physicalDevice;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        physicalDevice = nullptr;
    }

    if (!physicalDevice)
    {
        mfglog::Write(L"slSetVulkanInfo: no usable physicalDevice "
            L"(structVersion=%zu); Vulkan adapter unverified",
            info.structVersion);
    }
    else
    {
        const bool verified =
            ada_patch::ObserveVulkanPhysicalDevice(physicalDevice);
        mfglog::Write(L"slSetVulkanInfo: Vulkan adapter verified=%d "
            L"failure=%u (%s)", verified, ada_patch::FailureCode(),
            ada_patch::FailureName(ada_patch::FailureCode()));
    }
    return original(info);
}

// Copies only the fields the game's structVersion actually covers. The game
// may have allocated an older, shorter DLSSGOptions; reading past its version
// would read past its allocation.
sl::DLSSGOptions CopyKnownOptions(const sl::DLSSGOptions& source)
{
    sl::DLSSGOptions copy{};
    copy.next = source.next;
    copy.structType = source.structType;
    copy.structVersion = source.structVersion;
    copy.mode = source.mode;
    copy.numFramesToGenerate = source.numFramesToGenerate;
    copy.flags = source.flags;
    copy.dynamicResWidth = source.dynamicResWidth;
    copy.dynamicResHeight = source.dynamicResHeight;
    copy.numBackBuffers = source.numBackBuffers;
    copy.mvecDepthWidth = source.mvecDepthWidth;
    copy.mvecDepthHeight = source.mvecDepthHeight;
    copy.colorWidth = source.colorWidth;
    copy.colorHeight = source.colorHeight;
    copy.colorBufferFormat = source.colorBufferFormat;
    copy.mvecBufferFormat = source.mvecBufferFormat;
    copy.depthBufferFormat = source.depthBufferFormat;
    copy.hudLessBufferFormat = source.hudLessBufferFormat;
    copy.uiBufferFormat = source.uiBufferFormat;
    copy.onErrorCallback = source.onErrorCallback;
    if (source.structVersion >= sl::kStructVersion2)
        copy.bReserved15 = source.bReserved15;
    if (source.structVersion >= sl::kStructVersion3)
        copy.queueParallelismMode = source.queueParallelismMode;
    if (source.structVersion >= sl::kStructVersion4)
        copy.enableUserInterfaceRecomposition =
            source.enableUserInterfaceRecomposition;
    if (source.structVersion >= sl::kStructVersion5)
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    return copy;
}

// sl::Result names, mirrored from sl_result.h (40 contiguous entries).
// A raw number in a bug report is nearly useless; a name is not.
constexpr const wchar_t* kResultNames[] = {
    L"eOk",
    L"eErrorIO",
    L"eErrorDriverOutOfDate",
    L"eErrorOSOutOfDate",
    L"eErrorOSDisabledHWS",
    L"eErrorDeviceNotCreated",
    L"eErrorNoSupportedAdapterFound",
    L"eErrorAdapterNotSupported",
    L"eErrorNoPlugins",
    L"eErrorVulkanAPI",
    L"eErrorDXGIAPI",
    L"eErrorD3DAPI",
    L"eErrorNRDAPI",
    L"eErrorNVAPI",
    L"eErrorReflexAPI",
    L"eErrorNGXFailed",
    L"eErrorJSONParsing",
    L"eErrorMissingProxy",
    L"eErrorMissingResourceState",
    L"eErrorInvalidIntegration",
    L"eErrorMissingInputParameter",
    L"eErrorNotInitialized",
    L"eErrorComputeFailed",
    L"eErrorInitNotCalled",
    L"eErrorExceptionHandler",
    L"eErrorInvalidParameter",
    L"eErrorMissingConstants",
    L"eErrorDuplicatedConstants",
    L"eErrorMissingOrInvalidAPI",
    L"eErrorCommonConstantsMissing",
    L"eErrorUnsupportedInterface",
    L"eErrorFeatureMissing",
    L"eErrorFeatureNotSupported",
    L"eErrorFeatureMissingHooks",
    L"eErrorFeatureFailedToLoad",
    L"eErrorFeatureWrongPriority",
    L"eErrorFeatureMissingDependency",
    L"eErrorFeatureManagerInvalidState",
    L"eErrorInvalidState",
    L"eWarnOutOfVRAM",
};

const wchar_t* ResultName(sl::Result result) noexcept
{
    const size_t index = static_cast<size_t>(result);
    return index < _countof(kResultNames) ? kResultNames[index] : L"unknown";
}

sl::Result HookSetOptions(const sl::ViewportHandle& viewport,
    const sl::DLSSGOptions& options)
{
    auto* original = gOriginalSetOptions.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    // Whenever the game turns frame generation off, leave it off. This mod
    // changes how many frames are generated, never whether they are.
    if (options.mode == sl::DLSSGMode::eOff)
        return original(viewport, options);

    sl::DLSSGOptions adjusted = CopyKnownOptions(options);
    adjusted.mode = sl::DLSSGMode::eOn;
    adjusted.numFramesToGenerate = EffectiveMultiplier() - 1;

    const sl::Result result = original(viewport, adjusted);
    const uint64_t call =
        gSetOptionsCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log the first call and then powers of two, so a game that reapplies
    // options every frame does not fill the log.
    const bool report = call == 1 || (call & (call - 1)) == 0;

    if (result == sl::Result::eOk)
    {
        if (report)
        {
            mfglog::Write(L"slDLSSGSetOptions call=%llu gameMode=%u "
                L"numFramesToGenerate=%u->%u result=0 (eOk)",
                static_cast<unsigned long long>(call),
                static_cast<uint32_t>(options.mode),
                options.numFramesToGenerate, adjusted.numFramesToGenerate);
        }
        return result;
    }

    // The multiplied request was rejected. Replay the game's own options so
    // the game keeps whatever frame generation it asked for instead of losing
    // it entirely -- and log both results, which says whether the rejection is
    // caused by the higher frame count or would have happened anyway.
    const sl::Result fallback = original(viewport, options);
    gRejectedRequests.fetch_add(1, std::memory_order_relaxed);
    if (report)
    {
        mfglog::Write(L"slDLSSGSetOptions call=%llu gameMode=%u: "
            L"numFramesToGenerate=%u->%u REJECTED result=%u (%s); "
            L"replayed the game's own %u -> result=%u (%s)",
            static_cast<unsigned long long>(call),
            static_cast<uint32_t>(options.mode),
            options.numFramesToGenerate, adjusted.numFramesToGenerate,
            static_cast<uint32_t>(result), ResultName(result),
            options.numFramesToGenerate,
            static_cast<uint32_t>(fallback), ResultName(fallback));
    }
    if (call == 1)
    {
        // Ask the wrapper directly what ceiling it is enforcing. This
        // separates "our patches did not raise the advertised maximum" from
        // "the maximum is fine and the refusal comes from somewhere else".
        if (auto* getState = gGetState.load(std::memory_order_acquire))
        {
            sl::DLSSGState state{};
            const sl::Result stateResult = getState(viewport, state, &options);
            if (stateResult == sl::Result::eOk)
            {
                mfglog::Write(L"  DLSSGState: numFramesToGenerateMax=%u "
                    L"status=%u minWidthOrHeight=%u",
                    state.numFramesToGenerateMax,
                    static_cast<uint32_t>(state.status),
                    state.minWidthOrHeight);
                mfglog::Write(L"  (NVIDIA documents 1 = up to 2x, "
                    L"5 = up to 6x)");
            }
            else
            {
                mfglog::Write(L"  slDLSSGGetState failed: result=%u (%s)",
                    static_cast<uint32_t>(stateResult),
                    ResultName(stateResult));
            }
        }
        else
        {
            mfglog::Write(L"  slDLSSGGetState was never resolved by the game, "
                L"so the wrapper's ceiling could not be queried.");
        }

        if (fallback == sl::Result::eOk)
        {
            mfglog::Write(L"  => The game's own request succeeds but the "
                L"multiplied one does not: the higher frame count is what is "
                L"being refused. Try a lower Multiplier in RTX40MFG.ini.");
        }
        else
        {
            mfglog::Write(L"  => The game's own request fails too, so this is "
                L"not caused by the multiplier. DLSS-G is refusing options at "
                L"this point regardless of what this mod does.");
        }
    }
    return fallback;
}

sl::Result HookGetFeatureFunction(sl::Feature feature,
    const char* functionName, void*& function)
{
    auto* original =
        gOriginalGetFeatureFunction.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    const sl::Result result = original(feature, functionName, function);
    if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G
        || !functionName || !function)
        return result;

    // Keep the state entry so the wrapper can be asked what it believes the
    // generated-frame ceiling is. Not hooked -- only borrowed.
    if (std::strcmp(functionName, "slDLSSGGetState") == 0)
    {
        gGetState.store(reinterpret_cast<PFun_slDLSSGGetState*>(function),
            std::memory_order_release);
        mfglog::Write(L"slDLSSGGetState resolved (kept for diagnostics)");
        return result;
    }

    if (std::strcmp(functionName, "slDLSSGSetOptions") != 0)
        return result;

    // Never chain onto ourselves if the game resolves the entry twice.
    if (function == reinterpret_cast<void*>(&HookSetOptions))
        return result;

    gOriginalSetOptions.store(
        reinterpret_cast<PFun_slDLSSGSetOptions*>(function),
        std::memory_order_release);
    function = reinterpret_cast<void*>(&HookSetOptions);
    mfglog::Write(L"slDLSSGSetOptions resolved and intercepted");
    return result;
}
}

void SetWrapperCompiledMaximum(uint32_t compiledMaximum) noexcept
{
    gWrapperCompiledMaximum.store(compiledMaximum, std::memory_order_release);
}

uint32_t EffectiveMultiplier() noexcept
{
    const uint32_t requested =
        gRequestedMultiplier.load(std::memory_order_acquire);
    const uint32_t wrapperMaximum = patches::SafeMaximumMultiplier(
        gWrapperCompiledMaximum.load(std::memory_order_acquire));
    return std::clamp(std::min(requested, wrapperMaximum),
        config::kMinimumMultiplier, config::kMaximumMultiplier);
}

bool InstallInterposerHooks(HMODULE interposer, const wchar_t* path) noexcept
{
    if (!interposer || gInterposerHooked.load(std::memory_order_acquire))
        return false;

    void* target = reinterpret_cast<void*>(
        GetProcAddress(interposer, "slGetFeatureFunction"));
    if (!target)
    {
        mfglog::Write(L"sl.interposer.dll has no slGetFeatureFunction: %s", path);
        return false;
    }

    void* original = nullptr;
    if (MH_CreateHook(target,
            reinterpret_cast<void*>(&HookGetFeatureFunction), &original)
        != MH_OK)
    {
        mfglog::Write(L"slGetFeatureFunction hook failed: %s", path);
        return false;
    }
    // Publish the trampoline before the detour goes live.
    gOriginalGetFeatureFunction.store(
        reinterpret_cast<PFun_slGetFeatureFunction*>(original),
        std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK)
    {
        gOriginalGetFeatureFunction.store(nullptr, std::memory_order_release);
        MH_RemoveHook(target);
        mfglog::Write(L"slGetFeatureFunction hook failed to enable: %s", path);
        return false;
    }
    gInterposerHooked.store(true, std::memory_order_release);
    mfglog::Write(L"slGetFeatureFunction hooked: %s", path);

    // Optional: only present when the game drives Streamline over Vulkan.
    if (void* vulkanTarget = reinterpret_cast<void*>(
            GetProcAddress(interposer, "slSetVulkanInfo")))
    {
        void* vulkanOriginal = nullptr;
        if (MH_CreateHook(vulkanTarget,
                reinterpret_cast<void*>(&HookSetVulkanInfo), &vulkanOriginal)
            == MH_OK)
        {
            gOriginalSetVulkanInfo.store(
                reinterpret_cast<PFun_slSetVulkanInfoAbi*>(vulkanOriginal),
                std::memory_order_release);
            if (MH_EnableHook(vulkanTarget) == MH_OK)
            {
                mfglog::Write(L"slSetVulkanInfo hooked (Vulkan path available)");
            }
            else
            {
                gOriginalSetVulkanInfo.store(nullptr,
                    std::memory_order_release);
                MH_RemoveHook(vulkanTarget);
                mfglog::Write(L"slSetVulkanInfo hook failed to enable");
            }
        }
        else
        {
            mfglog::Write(L"slSetVulkanInfo hook failed");
        }
    }
    return true;
}
}
