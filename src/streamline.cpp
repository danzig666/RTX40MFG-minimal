#include "streamline.h"
#include "config.h"
#include "log.h"
#include "patches.h"

#include <sl.h>
#include <sl_dlss_g.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
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
    if (call == 1 || (call & (call - 1)) == 0)
    {
        mfglog::Write(L"slDLSSGSetOptions call=%llu gameMode=%u "
            L"numFramesToGenerate=%u->%u result=%u",
            static_cast<unsigned long long>(call),
            static_cast<uint32_t>(options.mode),
            options.numFramesToGenerate, adjusted.numFramesToGenerate,
            static_cast<uint32_t>(result));
    }
    return result;
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
        || !functionName || !function
        || std::strcmp(functionName, "slDLSSGSetOptions") != 0)
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
    return true;
}
}
