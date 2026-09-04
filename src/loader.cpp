// RTX40MFG.asi -- minimal Ada multi-frame-generation core.
//
// Load early, patch NVIDIA's Ada DLSS-G path once, and ask Streamline for more
// than one generated frame. Everything the mod does happens in three places:
//
//   sl.interposer.dll  -> intercept the resolved slDLSSGSetOptions
//   sl.dlss_g.dll      -> remove the wrapper's generated-frame clamp
//   nvngx_dlssg.dll    -> open the NGX device gate, then correct Ada's
//                         hardcoded 0.5 temporal position before CreateFeature
//
// The frame generation itself remains entirely NVIDIA's.

#include "ada_patch.h"
#include "config.h"
#include "log.h"
#include "ngx.h"
#include "patches.h"
#include "provider_policy.h"
#include "streamline.h"
#include "version.h"

#include <Windows.h>
#include <psapi.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

std::atomic<uint32_t> gRequestedMultiplier{config::kMinimumMultiplier};

namespace
{
constexpr DWORD kRescanIntervalMs = 500;
constexpr DWORD kRescanDurationMs = 60'000;

std::wstring gExecutableDirectory;
std::mutex gInspectMutex;
std::vector<HMODULE> gInspected;

using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

std::atomic<LoadLibraryAFn> gOriginalLoadLibraryA{nullptr};
std::atomic<LoadLibraryWFn> gOriginalLoadLibraryW{nullptr};
std::atomic<LoadLibraryExAFn> gOriginalLoadLibraryExA{nullptr};
std::atomic<LoadLibraryExWFn> gOriginalLoadLibraryExW{nullptr};

std::wstring ModulePath(HMODULE module)
{
    std::wstring path(MAX_PATH * 4, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

std::wstring ParentDirectory(const std::wstring& path)
{
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos
        ? std::wstring{} : path.substr(0, separator);
}

bool FileNameEquals(const std::wstring& path, const wchar_t* expected)
{
    const size_t separator = path.find_last_of(L"\\/");
    const wchar_t* name = separator == std::wstring::npos
        ? path.c_str() : path.c_str() + separator + 1;
    return expected && _wcsicmp(name, expected) == 0;
}

// A Streamline plug-in -- the DLSS-G wrapper among them -- exports this.
bool IsStreamlinePlugin(HMODULE module)
{
    return GetProcAddress(module, "slGetPluginFunction") != nullptr;
}

// The NGX DLSS-G provider: it carries the frame generation implementation and
// is distinguished from a DLSS Super Resolution provider by its exports. The
// same DLL normally exposes both the D3D12 and the Vulkan surface, but accept
// either so a Vulkan-only build is still recognized.
bool IsDlssgProvider(HMODULE module)
{
    const bool createEntry =
        GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature") != nullptr
        || GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature") != nullptr
        || GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1") != nullptr;
    return provider_policy::IsDlssgImplementationModule(module) && createEntry
        && GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture") != nullptr;
}

// Printed whenever a provider is rejected, so a report says what to swap to.
void LogSupportedProviderVersions()
{
    std::wstring list;
    for (const auto& supported : provider_policy::kSupportedVersions)
    {
        wchar_t entry[32]{};
        _snwprintf_s(entry, _countof(entry), _TRUNCATE, L"%s%u.%u.%u",
            list.empty() ? L"" : L", ",
            supported.major, supported.minor, supported.build);
        list += entry;
    }
    mfglog::Write(L"  Supported provider versions: %s", list.c_str());
    mfglog::Write(L"  Replace nvngx_dlssg.dll in the game folder with a "
        L"genuine NVIDIA-signed build at one of those versions.");
}

void InspectModule(HMODULE module)
{
    if (!module)
        return;
    {
        std::lock_guard lock(gInspectMutex);
        for (const HMODULE seen : gInspected)
        {
            if (seen == module)
                return;
        }
        gInspected.push_back(module);
    }

    const std::wstring path = ModulePath(module);
    if (path.empty())
        return;

    if (FileNameEquals(path, L"sl.interposer.dll"))
        streamline::InstallInterposerHooks(module, path.c_str());

    if (IsStreamlinePlugin(module))
    {
        const patches::Result result =
            patches::PatchStreamlineMaximum(module, path.c_str());
        if (result.candidate && result.patched)
            streamline::SetWrapperCompiledMaximum(result.compiledMaximum);
    }

    if (IsDlssgProvider(module))
    {
        provider_policy::VersionTriplet version{};
        const bool haveVersion =
            provider_policy::ReadProviderVersion(path.c_str(), version);
        if (haveVersion)
        {
            mfglog::Write(L"DLSS-G provider found: version %u.%u.%u -- %s",
                version.major, version.minor, version.build, path.c_str());
        }
        else
        {
            mfglog::Write(L"DLSS-G provider found but its file version could "
                L"not be read -- %s", path.c_str());
        }

        if (!provider_policy::IsSupportedProvider(module, path.c_str()))
        {
            mfglog::Write(L"  ^ NOT a supported version; leaving it untouched. "
                L"No frames will be unlocked.");
            LogSupportedProviderVersions();
            return;
        }
        mfglog::Write(L"  ^ supported; patching");
        patches::PatchNgxDeviceSupport(module, path.c_str());
        ngx::InstallCreateFeatureHooks(module, path.c_str());
    }
}

void ScanLoadedModules()
{
    std::vector<HMODULE> modules(512);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
            static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
        return;
    const size_t count = std::min<size_t>(modules.size(),
        needed / sizeof(HMODULE));
    for (size_t index = 0; index < count; ++index)
        InspectModule(modules[index]);
}

HMODULE WINAPI HookLoadLibraryA(LPCSTR fileName)
{
    auto* original = gOriginalLoadLibraryA.load(std::memory_order_acquire);
    if (!original)
        return nullptr;
    HMODULE module = original(fileName);
    if (module)
        InspectModule(module);
    return module;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR fileName)
{
    auto* original = gOriginalLoadLibraryW.load(std::memory_order_acquire);
    if (!original)
        return nullptr;
    HMODULE module = original(fileName);
    if (module)
        InspectModule(module);
    return module;
}

HMODULE WINAPI HookLoadLibraryExA(LPCSTR fileName, HANDLE file, DWORD flags)
{
    auto* original = gOriginalLoadLibraryExA.load(std::memory_order_acquire);
    if (!original)
        return nullptr;
    HMODULE module = original(fileName, file, flags);
    if (module)
        InspectModule(module);
    return module;
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags)
{
    auto* original = gOriginalLoadLibraryExW.load(std::memory_order_acquire);
    if (!original)
        return nullptr;
    HMODULE module = original(fileName, file, flags);
    if (module)
        InspectModule(module);
    return module;
}

template <typename Function>
void InstallLoaderHook(HMODULE kernel32, const char* name,
    void* replacement, std::atomic<Function>& published)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(kernel32, name));
    void* original = nullptr;
    if (!target || MH_CreateHook(target, replacement, &original) != MH_OK)
    {
        mfglog::Write(L"%hs hook failed", name);
        return;
    }
    // Publish the trampoline before the detour goes live, so a call that
    // lands in between still reaches the original function.
    published.store(reinterpret_cast<Function>(original),
        std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK)
    {
        published.store(nullptr, std::memory_order_release);
        MH_RemoveHook(target);
        mfglog::Write(L"%hs hook failed to enable", name);
    }
}

// The loader hooks catch every module loaded from here on. This covers the
// remaining case: a provider pulled in by a static import of a DLL that was
// already resolving when this mod attached.
DWORD WINAPI RescanThread(LPVOID)
{
    for (DWORD elapsed = 0; elapsed < kRescanDurationMs;
         elapsed += kRescanIntervalMs)
    {
        Sleep(kRescanIntervalMs);
        ScanLoadedModules();
    }
    return 0;
}

void Initialize()
{
    const std::wstring executable = ModulePath(GetModuleHandleW(nullptr));
    gExecutableDirectory = ParentDirectory(executable);

    const config::Settings settings = config::Load(gExecutableDirectory.c_str());
    gRequestedMultiplier.store(settings.multiplier, std::memory_order_release);
    if (settings.log)
    {
        mfglog::Open(gExecutableDirectory.c_str());
        ada_patch::SetLogCallback(&mfglog::WriteMessage);
    }

    mfglog::Write(L"RTX40MFG %s -- requested multiplier %ux",
        MFG_VERSION_STRING, settings.multiplier);
    mfglog::Write(L"Host: %s", executable.c_str());

    if (MH_Initialize() != MH_OK)
    {
        mfglog::Write(L"MinHook initialization failed; nothing was hooked");
        return;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32)
    {
        InstallLoaderHook(kernel32, "LoadLibraryA",
            reinterpret_cast<void*>(&HookLoadLibraryA), gOriginalLoadLibraryA);
        InstallLoaderHook(kernel32, "LoadLibraryW",
            reinterpret_cast<void*>(&HookLoadLibraryW), gOriginalLoadLibraryW);
        InstallLoaderHook(kernel32, "LoadLibraryExA",
            reinterpret_cast<void*>(&HookLoadLibraryExA),
            gOriginalLoadLibraryExA);
        InstallLoaderHook(kernel32, "LoadLibraryExW",
            reinterpret_cast<void*>(&HookLoadLibraryExW),
            gOriginalLoadLibraryExW);
    }

    ScanLoadedModules();

    if (HANDLE thread = CreateThread(nullptr, 0, &RescanThread, nullptr, 0,
            nullptr))
        CloseHandle(thread);
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        Initialize();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        mfglog::Close();
    }
    return TRUE;
}
