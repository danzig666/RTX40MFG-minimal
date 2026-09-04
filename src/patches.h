#pragma once

#include <Windows.h>

#include <cstdint>

// The two in-memory code patches that let an Ada GPU reach NVIDIA's
// multi-frame path. Both are strictly fail-closed: a pattern that appears
// zero times or more than once is never touched.
namespace patches
{
struct Result
{
    bool candidate = false; // the module contained the expected code shape
    bool patched = false;
    uint32_t compiledMaximum = 0; // Streamline clamp only: 1, 3 or 5
};

// Turns the NGX per-GPU support branch in nvngx_dlssg.dll into a fallthrough.
//
//   test dl, dl
//   je   reject        <- six bytes replaced with NOPs
//   mov  esi, 5
Result PatchNgxDeviceSupport(HMODULE module, const wchar_t* path) noexcept;

// Removes the Streamline wrapper's clamp on the requested generated-frame
// count in sl.dlss_g.dll.
//
//   mov   edx, <compiled maximum>   ; 1, 3 or 5
//   cmp   ecx, edx
//   cmovb edx, ecx                  <- three bytes replaced with NOPs
Result PatchStreamlineMaximum(HMODULE module, const wchar_t* path) noexcept;

// A wrapper compiled for N generated frames natively supports an N+1
// multiplier; anything unrecognized falls back to plain 2x.
constexpr uint32_t SafeMaximumMultiplier(uint32_t compiledMaximum) noexcept
{
    return (compiledMaximum == 1 || compiledMaximum == 3
        || compiledMaximum == 5) ? compiledMaximum + 1u : 2u;
}
}
