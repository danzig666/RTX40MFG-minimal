#include "config.h"

#include <Windows.h>

#include <algorithm>
#include <string>

namespace config
{
Settings Load(const wchar_t* executableDirectory) noexcept
{
    Settings settings{};
    if (!executableDirectory)
        return settings;

    std::wstring path = executableDirectory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path.push_back(L'\\');
    path += L"RTX40MFG.ini";

    const UINT multiplier = GetPrivateProfileIntW(
        L"MFG", L"Multiplier", 0, path.c_str());
    settings.multiplier = multiplier == 0 ? 0u
        : std::clamp(static_cast<uint32_t>(multiplier),
            kMinimumMultiplier, kMaximumMultiplier);
    settings.log = GetPrivateProfileIntW(L"MFG", L"Log", 1, path.c_str()) != 0;
    settings.legacyNgxPatch = GetPrivateProfileIntW(
        L"MFG", L"LegacyNgxPatch", 0, path.c_str()) != 0;
    return settings;
}
}
