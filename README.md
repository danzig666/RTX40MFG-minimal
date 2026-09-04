# mfg4rtx4k — minimal Ada MFG core

> ## Status: working on real hardware (D3D12), Vulkan untested
>
> Confirmed on an RTX 40 GPU with *007 First Light* (D3D12, DLSS-G provider
> 310.7.128, driver-OTA Streamline plugin 2.x): the game's menu exposes
> 2x–6x, the runtime accepts the request, the rebuilt Ada kernel is
> published, and `DLSSGState.numFramesActuallyPresented` reports **2 at 2x
> and 4 at 4x**.
>
> ```
> [slSetD3DDevice] Ada temporal patch: ready=1
> [state] DLSSGState: status=eOk presented=2 max=5      (2x)
> [state] DLSSGState: status=eOk presented=4 max=5      (4x)
> ```
>
> Still unverified: image quality of the generated frames (the temporal
> correction's actual visual effect), the **Vulkan** path, and any provider
> other than 310.7.128. Still unsupported research software: it patches
> NVIDIA binaries in memory on a path NVIDIA does not certify for Ada.

A stripped-down rewrite of [dashdogy/RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock):
one `.asi`, one `.ini`, and only the three mechanisms that actually make DLSS
Multi Frame Generation work on RTX 40 series (Ada, SM 8.9) GPUs.

This does **not** implement frame generation. Every generated frame is produced
by NVIDIA's own DLSS-G neural model. All this does is answer three questions
differently:

| Question | Stock RTX 40 | Here |
|---|---|---|
| What architecture is this? | below Blackwell → 1 frame | reads as Ada-allowed |
| How many frames may be requested? | 1 | 1–3 |
| Where in time does Ada evaluate them? | always `t = 0.5` | `t` and `1 - t` |
| How is multi-frame output paced? | hardware flip metering (Ada has none) | software RSYNC |

The third one is the part that matters. Forcing `numFramesToGenerate = 3`
without it makes Ada evaluate all three generated frames at the midpoint, so
they collapse into near-duplicates instead of spacing out at ¼, ½, ¾.

**Unsupported research software.** It patches NVIDIA binaries in memory on a
path NVIDIA does not certify for Ada. Expect artifacts, stutter, black screens
or crashes, and do not use it where anti-cheat is watching. See the untested
warning above before you consider running it at all.

## What it does

```
game
 │
 ▼
sl.interposer.dll ── slGetFeatureFunction ──► our slDLSSGSetOptions
 │                                              numFramesToGenerate = N-1
 ▼
sl.dlss_g.dll ─────── cmovb clamp NOPed ──────► request passes through
 │
 ▼
nvngx_dlssg.dll ───── device gate NOPed
 │                    NVSDK_NGX_{D3D12,VULKAN}_CreateFeature(FrameGeneration)
 │                      ├── verify adapter is Ada (SM 8.9, via CUDA)
 │                      ├── verify provider version + SHA-256 of its fatbin
 │                      ├── rebuild the SM89 PTX: 0.5 → t / (1-t)
 │                      └── publish a cloned kernel descriptor
 ▼
NVIDIA DLSS-G generates at t = .25 / .50 / .75
```

## Install

Requires Windows x64, an RTX 40 GPU, a **D3D12 or Vulkan** game with working
DLSS Frame Generation, and
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

1. Copy `RTX40MFG.asi` and `RTX40MFG.ini` next to the game executable.
2. Install Ultimate ASI Loader under a proxy name the game imports early —
   commonly `dinput8.dll` or `version.dll`. Do not use a name another mod
   already owns.
3. Set `Multiplier` in `RTX40MFG.ini`, launch, enable Frame Generation in the
   game.

If the log shows the provider was patched but frame pacing looks unchanged,
toggle Frame Generation off and on, or restart. The DLSS-G feature has to be
created *after* the patch is published.

## Provider version

## What broke, and what fixed it

Getting from "everything is accepted" to "frames appear" took a bisection on
real hardware, and the answer was not any of the byte patches. Two things this
port had *simplified* away from the upstream project turned out to be the
reasons it did not work:

1. **Adapter verification inside `CreateFeature`.** Recovering the D3D12
   device from the command list at feature-creation time looked neater than
   hooking `slSetD3DDevice`. But verification loads `nvcuda.dll`, calls
   `cuInit`, enumerates devices and frees the library — synchronously, on the
   render thread, in the middle of NVIDIA bringing up its own CUDA context.
   Verification now happens at `slSetD3DDevice`, as upstream does.

2. **Inline detours on the provider's exports.** The upstream project hooks
   `NVSDK_NGX_D3D12_CreateFeature` through a deliberately careful
   atomic-hotpatch framework; the RenoDx fork never touches the provider's
   entry points at all. This port replaced the framework with plain MinHook.
   With those detours present, NVIDIA refuses feature creation with
   `NVSDK_NGX_Result_FAIL_PlatformError`; without them it works. The kernel
   patch never needed them — it swaps a pointer in the provider's data
   section, and `slDLSSGSetOptions` (hooked at the Streamline layer) fires
   right before feature creation. The detours are kept only as an opt-in
   diagnostic (`NgxHooks=1`) for reading result codes.

Every switch in `RTX40MFG.ini` exists because bisecting this needed it; the
defaults are the working configuration.

## Provider version

The mod patches `nvngx_dlssg.dll`, the NGX **provider** — the file that
actually contains NVIDIA's DLSS-G implementation. It ships **with the game**,
not with the driver, usually beside the executable or under a `Streamline` /
`ThirdParty/Win64` subfolder, alongside `sl.interposer.dll`, `sl.common.dll`
and `sl.dlss_g.dll`.

Check its version — right-click → Properties → Details → *File version*, or:

```powershell
Get-ChildItem "<game folder>" -Recurse -Filter nvngx_dlssg.dll |
  ForEach-Object { "$($_.VersionInfo.FileVersion)  $($_.FullName)" }
```

Only these are supported (the fourth version component is ignored):

```
310.1.0  310.2.0  310.2.1  310.3.0  310.4.0  310.5.0  310.5.2
310.5.3  310.6.0  310.7.0  310.7.128  310.7.129  310.8.0  310.9.0
```

**310.x is the DLSS 4 generation.** Those providers carry both SM120
(Blackwell) and SM89 (Ada) code in their fatbin, which is the whole reason this
works. A 1.x or 3.x provider is DLSS 3 frame generation and has no SM89 payload
for the temporal correction to operate on — the mod rejects it at the version
check and leaves it alone. `RTX40MFG.log` prints the version it found and the
list above whenever it rejects one.

### Swapping to a supported version

If your game ships an unsupported provider, replacing `nvngx_dlssg.dll` in the
game folder is the normal fix — the same thing DLSS Swapper and manual DLSS
upgrading do. Points to watch:

- Use a **genuine NVIDIA-signed** DLL. NGX and Streamline verify signatures; a
  tampered file is rejected before this mod ever sees it.
- **Back up the original** — some launchers restore or checksum it.
- Turn off **NVIDIA App → DLSS Override** for that game, or it may supply its
  own provider instead of the one you placed.
- The wrapper (`sl.dlss_g.dll`) and provider (`nvngx_dlssg.dll`) version
  independently. Each is checked separately, so a mismatch shows up in the log
  rather than misbehaving silently.

Do not bundle NVIDIA or Streamline DLLs *with this mod* — it ships none by
design, and patches whatever the game loads.

## How the capability is actually decided

Getting the frame count raised is not one switch. The provider gates
multi-frame on the **NVAPI architecture id**, comparing against `0x1b0`
(GB20x, Blackwell) in more than one place, and the places do different jobs:

```asm
; DLSSGInstanceManager::PopulateParameters -- decides what to advertise
81 FD B0 01 00 00   cmp ebp, 0x1b0
jl  <max = 1>                             ; Ada lands here
mov edi, 5
...                 Set("DLSSG.MultiFrameCountMax", edi)

; a separate runtime capability flag that drives generation itself
3D B0 01 00 00      cmp eax, 0x1b0
0F 93 C0            setae al
88 47 28            mov byte ptr [rdi+0x28], al
```

Every `cmp` against `0x1b0` is rewritten to `0x190` (AD10x, Ada); a
`mov r32, 0x1b0` is the arch-id lookup table, not a gate, and is left alone.
**Patching the first without the second is the worst outcome** — the options
appear, the request is accepted, and the game renders black.

Then the frames have to reach the screen. Blackwell paces multi-frame output
with hardware flip metering; Ada has none, so the present queue waits for
something that never happens and 3x+ freezes the image while audio continues.
Streamline already ships the software fallback (RSYNC) and the plugin logs
`"FG1 DLL has been detected"` when it selects it — so the flag it writes is
found by that marker and pinned. Neither the flag's offset nor its polarity can
be hardcoded: they move between plugin builds, so both are read out of the
plugin's own fallback code at runtime.

Everything is applied to the **mapped image only**. NGX verifies the provider's
Authenticode signature when it loads the file, so the same bytes changed on
disk make frame generation disappear entirely; the mapped copy is never
re-checked.

## Graphics APIs

Both D3D12 and Vulkan are supported. The Ada temporal correction itself is
API-agnostic — it rewrites CUDA kernels inside the provider and does not care
how frames reach the screen — so the only API-specific part is **how the
adapter is identified**:

| | How the Ada check gets its adapter |
|---|---|
| **D3D12** | straight off the command list NGX is handed, at `CreateFeature` |
| **Vulkan** | captured earlier, from `slSetVulkanInfo` |

That difference is forced: a `VkCommandBuffer` carries no back-pointer to its
`VkDevice` or `VkPhysicalDevice`, so on Vulkan the adapter cannot be recovered
at `CreateFeature` time. If a Vulkan game never routes its device setup through
Streamline, `slSetVulkanInfo` never fires, the adapter stays unverified, and
the mod refuses to patch — saying so in the log.

Vulkan support deliberately omits upstream's six `NVSDK_NGX_VULKAN_Init*`
fallback hooks. A game that bypasses Streamline for Vulkan setup is out of
scope anyway, since the frame-count clamp lives in the Streamline wrapper.

`vulkan-1.dll` is loaded dynamically and only when a Vulkan game is present —
the binary has no Vulkan import and needs no Vulkan SDK to build.

### NGX ABI

The mod mirrors three NGX entry points rather than depending on the NGX SDK.
Those mirrors are verified against the official
[NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (NGX API 1.5.0, commit
`a291cc7`), headers `nvsdk_ngx_defs.h`, `nvsdk_ngx.h` and `nvsdk_ngx_vk.h`:

```c
NVSDK_NGX_Feature_FrameGeneration = 11
NVSDK_NGX_Result_Success          = 0x1
NVSDK_NGX_Result_Fail             = 0xBAD00000
NVSDK_CONV                        = __cdecl

NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
NVSDK_NGX_Result NVSDK_NGX_VULKAN_CreateFeature(
    VkCommandBuffer, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
NVSDK_NGX_Result NVSDK_NGX_VULKAN_CreateFeature1(
    VkDevice, VkCommandBuffer, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
```

`nvsdk_ngx_helpers_dlssg.h` confirms DLSS-G is created through those ordinary
`CreateFeature` entries with the frame-generation feature ID, which is exactly
what this mod intercepts.

## Configuration

```ini
[MFG]
Multiplier=4   ; 2, 3 or 4
Log=1
```

The multiplier is also clamped to what the active wrapper natively supports
(a wrapper compiled for 1/3/5 generated frames caps at 2×/4×/6×). Changing it
requires a restart — there is no live UI.

## Build

Only the Streamline SDK's `include/` directory is needed — no NGX SDK, no
ReShade, no Dear ImGui. MinHook is vendored.

With CMake (3.20+):

```bash
cmake -B build -S . -A x64 -DSTREAMLINE_ROOT=<path to Streamline SDK>
cmake --build build --config Release
```

Or without CMake, straight to MSVC:

```bash
set STREAMLINE_ROOT=<path to Streamline SDK>
build.bat
```

Both produce a ~200 KB x64 DLL — `build\Release\RTX40MFG.asi` and
`build\RTX40MFG.asi` respectively — linked against the **static** CRT, so it
depends only on `kernel32`, `bcrypt` and `version` and needs no VC++
redistributable beside the game.

Both paths are verified on VS 2019 Build Tools 14.29 with Windows SDK
10.0.19041, and compile clean at `/W4`.

## What was cut, and why

The upstream project is ~14,500 lines of native code, `patcher.cpp` alone
~6,700. Almost all of that supports being *universal* — every game, both
graphics APIs, every wrapper delivery path, a live UI. None of it is required
to demonstrate or use the mechanism.

Kept:

| Component | Note |
|---|---|
| Ada temporal correction | the genuinely necessary part; kept intact |
| Arch gates (`0x1b0` → `0x190`) | what actually raises the advertised frame count |
| Flip metering fallback | without it, 3x+ generates frames that never appear |
| NGX device-support patch | 6-byte NOP; matches on some builds, not what does the work |
| Streamline maximum patch | 3-byte NOP, unchanged |
| `slDLSSGSetOptions` interception | reduced to ~10 meaningful lines |
| Provider version policy | kept — patching unknown PTX is how you get crashes |
| MinHook | replaces the 1,000-line entry-detour framework |

Cut: the ReShade/ImGui front end (~2,200 lines), the NGX `EvaluateFeature`
hook, `slDLSSGGetState`, Dynamic MFG, the NVIDIA per-game profile manifest,
all OTA wrapper handling, FPS and temporal-interval telemetry, UI
recomposition, live reconfiguration, and 5×/6×.

Vulkan was cut in the first pass and added back in v0.2 — see below.

The design rule was **less universal, not less safe**: every identity check —
Ada verification, provider version, the three SHA-256 digests, and the
exactly-one-match rule on both byte patterns — is still there, and everything
still fails closed.

Two deliberate trade-offs the upstream project handles and this does not:

- MinHook writes a plain jump, so a game where another mod has already
  detoured the same entry point may not work.
- Hooks are installed from inside the loader-lock path when a module loads,
  which is standard practice for ASI mods but not free of risk.

## Layout

```
src/
├── loader.cpp          DllMain, config, module discovery, dispatch    328
├── config.cpp/.h       RTX40MFG.ini                                    48
├── log.cpp/.h          RTX40MFG.log                                    81
├── patches.cpp/.h      the two byte patches                           245
├── gates.cpp           arch gates + flip metering                     290
├── streamline.cpp/.h   slGetFeatureFunction, SetOptions, VulkanInfo    281
├── ngx.cpp/.h          provider registration, Ada patch trigger,      ~420
│                       optional diagnostic detours
├── ada_patch.cpp/.h    Ada SM89 temporal correction                 1,326
├── provider_policy.*   supported DLSS-G provider versions             159
└── version.h           version string                                   5
```

2,708 lines, against 14,499 upstream. MinHook is vendored under
`src/third_party/` and not counted.

## Credit and licence

The mechanisms, the byte patterns, the provider profiles and the entire
temporal correction are the work of **Michael Robles** in
[RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock), MIT licensed —
see `LICENSE.upstream`. This repository is a reduction of that work, not an
independent discovery.

The **arch-gate** and **flip-metering** techniques in `src/gates.cpp`, and the
analysis of why they are both required, come from
[mavismmg/MFGAdaUnlock-RenoDx](https://github.com/mavismmg/MFGAdaUnlock-RenoDx),
also MIT. That project diagnosed what the byte patches inherited from upstream
do not cover; this is a port of its findings into this codebase.

MinHook is © Tsuda Kageyu, BSD 2-clause.
